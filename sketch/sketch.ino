// Waste segregator - MCU (controller) side.
// MOISTURE-ONLY build: no IR sensor, no actuator. Two LEDs + one sensor.
//
// ---------------------------------------------------------------------------
// HOW DETECTION WORKS
//
// On boot the board samples the EMPTY tray for a few seconds and stores that
// as `baseline`. Everything afterwards is measured as a SHIFT from baseline:
//
//     delta < DELTA_PRESENT   -> nothing there            -> IDLE
//     DELTA_PRESENT..DELTA_WET-> something present, dry    -> DRY
//     delta >= DELTA_WET      -> something present, wet    -> WET
//
// Deltas are used instead of absolute ADC values because absolute readings
// drift with supply voltage, temperature and sensor variation. A delta
// cancels all of that, since the baseline drifts along with the reading.
//
// IMPORTANT: keep the tray EMPTY while the board boots, or the baseline will
// be wrong. The LEDs flash both-on during baselining so you know when.
//
// RESISTIVE SENSOR CAVEAT: a resistive probe conducts nothing through a dry
// paper ball, so dry items may produce no delta at all and never trigger.
// Use the manual trigger below if that happens to you.
//
// MANUAL TRIGGER: touch a jumper wire from D3 to GND. No button component
// needed - the pin uses an internal pull-up, so leaving it unconnected is
// harmless. This forces an immediate classification of whatever is on the tray.
// ---------------------------------------------------------------------------

#define STANDALONE_MODE 0     // 1 = MCU decides alone. 0 = wait for Linux side.

#include <Arduino_RouterBridge.h>

// ---------- Pins ----------
const int PIN_LED_WET  = 5;   // green LED + 220 ohm to GND
const int PIN_LED_DRY  = 6;   // red LED   + 220 ohm to GND
const int PIN_MOISTURE = A0;  // soil moisture module AO  (NOT DO)
const int PIN_IR       = 2;   // IR obstacle module OUT. Active LOW = detected.
const int PIN_TRIGGER  = 3;   // jumper to GND = classify now. Optional.
const int PIN_BUZZER   = 4;   // buzzer signal
const int PIN_WET_CLK  = 7;   // wet counter display  CLK
const int PIN_WET_DIO  = 8;   // wet counter display  DIO
const int PIN_DRY_CLK  = 9;   // dry counter display  CLK
const int PIN_DRY_DIO  = 10;  // dry counter display  DIO

// ---------- Buzzer type ----------
// 1 = ACTIVE buzzer: has its own oscillator, just needs DC. Fixed pitch.
// 0 = PASSIVE piezo: we generate the square wave ourselves, so pitch varies.
//
// How to tell: touch the buzzer straight to 3 V. A steady tone means ACTIVE.
// A single click, or silence, means PASSIVE.
//
// tone() is deliberately not used here - it is not confirmed on this core.
// The passive path generates the waveform by hand, which works everywhere.
#define BUZZER_ACTIVE 1

// ---------- What is allowed to trigger a decision ----------
// 1 = STRICT. A decision only happens when the IR module actually detects
//     something (or you jumper D3). The moisture reading is then used to
//     classify it - but it can never start a decision on its own.
// 0 = LOOSE. A large enough moisture shift can also trigger a decision, for
//     items the IR beam misses (thin, dark or matte surfaces reflect poorly).
//
// STRICT is the safe default: it means sensor drift can never produce a
// decision with an empty tray. If a dark item refuses to trigger, either
// aim the IR module better or use the D3 jumper.
#define REQUIRE_IR 1

// ---------- Sensor polarity ----------
// Most modules read LOWER as moisture rises. If your wet readings come out
// HIGHER than dry ones during calibration, change this to 0.
#define SENSOR_LOWER_IS_WETTER 1

// ---------- Detection thresholds (calibrate with bringup.py) ----------
int DELTA_PRESENT = 25;       // shift that counts as "something is there"
int DELTA_WET     = 120;      // shift that counts as "it is wet"

// ---------- Baselining ----------
const unsigned long BASELINE_MS       = 3000;   // sampling window at boot
const unsigned long REBASELINE_IDLE_MS = 120000; // re-baseline after 2 min idle

// ---------- Timing ----------
const unsigned long SAMPLE_EVERY_MS    = 50;
const unsigned long SENSING_MS         = 3000;  // "thinking" window, both LEDs
const unsigned long SENSING_BLINK_MS   = 250;   // blink rate while sensing
const unsigned long ANNOUNCE_BEEP_MS   = 2000;  // beep length on a decision
const unsigned long BLINK_FAST_MS      = 150;
const unsigned long HEARTBEAT_CYCLE_MS = 2000;
const unsigned long HEARTBEAT_ON_MS    = 40;
const unsigned long REPORT_EVERY_MS    = 1000;
const unsigned long PRESENCE_DEBOUNCE_MS = 150;  // signal must hold this long

// Modes - must match main.py
const int BIN_WET   = 0;
const int BIN_DRY   = 1;
const int BIN_HOLD  = 2;
const int MODE_IDLE    = 3;
const int MODE_SENSING = 4;   // item detected, measuring

int  baseline  = 0;
int  lastRaw   = 0;
int  lastDelta = 0;
int  peakDelta = 0;   // highest delta since the tray was last empty

int  mode  = MODE_IDLE;
bool armed = true;
bool phase = false;

unsigned long lastSample    = 0;
unsigned long lastToggle    = 0;
unsigned long lastReport    = 0;
unsigned long heartbeatMark = 0;
unsigned long idleSince     = 0;
unsigned long sensingStart  = 0;

bool presenceRaw       = false;
bool presenceStable    = false;
unsigned long presenceChangedAt = 0;

// ---------------------------------------------------------------------------
// TM1637 driver, written inline so there is no library dependency.
//
// The TM1637 looks like I2C but is NOT - it has no device address, so two
// displays cannot share a bus. Each one gets its own CLK/DIO pair.
//
// The protocol is open-drain: to send a 1 we RELEASE the line and let the
// module's onboard pull-up raise it; to send a 0 we drive it LOW. That is why
// the code switches pinMode instead of writing HIGH.
// ---------------------------------------------------------------------------

const uint8_t DIGIT_SEG[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

class Tm1637 {
public:
  void begin(int clkPin, int dioPin) {
    clk = clkPin;
    dio = dioPin;
    release(clk);
    release(dio);
  }

  // 0 = dimmest, 7 = brightest
  void setBrightness(uint8_t b) { bright = 0x88 | (b & 0x07); }

  void showNumber(int n) {
    if (n < 0)    n = 0;
    if (n > 9999) n = 9999;

    uint8_t seg[4] = { 0, 0, 0, 0 };   // 0x00 = blank, so leading zeros vanish
    int v = n;
    int i = 3;
    do {
      seg[i--] = DIGIT_SEG[v % 10];
      v /= 10;
    } while (v > 0 && i >= 0);

    writeSegments(seg);
  }

  void showAllEights() {
    uint8_t seg[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
    writeSegments(seg);
  }

private:
  int clk = 0;
  int dio = 0;
  uint8_t bright = 0x8B;      // display on, brightness 3

  void drive(int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  void release(int p) { pinMode(p, INPUT); }
  void bitDelay() { delayMicroseconds(100); }

  void start() {
    release(clk); release(dio); bitDelay();
    drive(dio); bitDelay();
  }

  void stop() {
    drive(clk); bitDelay();
    drive(dio); bitDelay();
    release(clk); bitDelay();
    release(dio); bitDelay();
  }

  void writeByte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {     // LSB first
      drive(clk); bitDelay();
      if (b & 0x01) release(dio); else drive(dio);
      bitDelay();
      release(clk); bitDelay();
      b >>= 1;
    }
    drive(clk); release(dio); bitDelay();  // 9th clock: the chip ACKs
    release(clk); bitDelay();
    drive(clk); bitDelay();
  }

  void writeSegments(const uint8_t* seg) {
    start(); writeByte(0x40); stop();      // auto-increment address mode
    start(); writeByte(0xC0);              // start writing at digit 0
    for (int i = 0; i < 4; i++) writeByte(seg[i]);
    stop();
    start(); writeByte(bright); stop();    // display on + brightness
  }
};

Tm1637 dispWet;
Tm1637 dispDry;

int wetCount = 0;
int dryCount = 0;

void refreshCounters() {
  dispWet.showNumber(wetCount);
  dispDry.showNumber(dryCount);
}

// ---------------------------------------------------------------------------

void ledsOff() {
  digitalWrite(PIN_LED_WET, LOW);
  digitalWrite(PIN_LED_DRY, LOW);
}

// Blocking on purpose. Beeps only fire the instant a decision is made, which
// is not a time-critical moment, and the whole pattern is under 300 ms. Making
// this non-blocking would add a state machine for no real benefit.
void beep(int freq, int ms) {
#if BUZZER_ACTIVE
  (void)freq;                       // active buzzers ignore pitch
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
#else
  long halfPeriod = 500000L / freq;             // microseconds
  long cycles = ((long)ms * 1000L) / (halfPeriod * 2);
  for (long i = 0; i < cycles; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(halfPeriod);
    digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(halfPeriod);
  }
#endif
}

// Each outcome gets its own rhythm, so you can tell the result by ear without
// looking at the LEDs.
//   WET  = one long low beep
//   DRY  = two short high beeps
//   HOLD = three urgent chirps
void announce(int m) {
  switch (m) {
    case BIN_WET:
      beep(800, ANNOUNCE_BEEP_MS);
      break;
    case BIN_DRY:
      beep(1200, ANNOUNCE_BEEP_MS);
      break;
    case BIN_HOLD:
      for (int i = 0; i < 3; i++) { beep(1600, 55); delay(55); }
      break;
  }
}

int readRaw() {
  long sum = 0;
  const int samples = 8;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(PIN_MOISTURE);
    delay(2);
  }
  return (int)(sum / samples);
}

// Positive delta always means "wetter than the empty tray", whichever way
// round the sensor happens to be wired.
int deltaFrom(int raw) {
#if SENSOR_LOWER_IS_WETTER
  return baseline - raw;
#else
  return raw - baseline;
#endif
}

// Sample the empty tray and store it as the reference point.
void calibrateBaseline() {
  digitalWrite(PIN_LED_WET, HIGH);
  digitalWrite(PIN_LED_DRY, HIGH);      // both on = "keep the tray empty"

  long sum = 0;
  int  n   = 0;
  unsigned long start = millis();
  while (millis() - start < BASELINE_MS) {
    sum += analogRead(PIN_MOISTURE);
    n++;
    delay(10);
  }
  baseline = (int)(sum / n);

  ledsOff();
  idleSince = millis();

  Monitor.print("baseline = ");
  Monitor.print(baseline);
  Monitor.print("  (from ");
  Monitor.print(n);
  Monitor.println(" samples)");
}

void selfTest() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_WET, HIGH); delay(150); digitalWrite(PIN_LED_WET, LOW);
    digitalWrite(PIN_LED_DRY, HIGH); delay(150); digitalWrite(PIN_LED_DRY, LOW);
  }
  delay(400);

  for (int i = 0; i < 4; i++) {       // WET pattern: green, slow
    digitalWrite(PIN_LED_WET, HIGH); delay(250);
    digitalWrite(PIN_LED_WET, LOW);  delay(250);
  }
  delay(400);

  for (int i = 0; i < 4; i++) {       // DRY pattern: red, slow
    digitalWrite(PIN_LED_DRY, HIGH); delay(250);
    digitalWrite(PIN_LED_DRY, LOW);  delay(250);
  }
  delay(400);

  for (int i = 0; i < 8; i++) {       // HOLD pattern: alternating, fast
    digitalWrite(PIN_LED_WET, HIGH); digitalWrite(PIN_LED_DRY, LOW);  delay(150);
    digitalWrite(PIN_LED_WET, LOW);  digitalWrite(PIN_LED_DRY, HIGH); delay(150);
  }
  ledsOff();

  dispWet.showAllEights();            // every segment lit - spot dead digits
  dispDry.showAllEights();
  delay(800);
  refreshCounters();

  beep(1000, 80);                     // confirms the buzzer is wired
}

bool triggerPressed() {
  return digitalRead(PIN_TRIGGER) == LOW;
}

// The IR module pulls its output LOW when something is in front of it.
bool irDetects() {
  return digitalRead(PIN_IR) == LOW;
}

// Raw detection, before debouncing. See REQUIRE_IR above.
bool objectDetected() {
#if REQUIRE_IR
  return irDetects() || triggerPressed();
#else
  return irDetects() || triggerPressed() || (lastDelta >= DELTA_PRESENT);
#endif
}

// Debounced presence. The raw signal has to hold steady for
// PRESENCE_DEBOUNCE_MS before it counts, so a single noisy sample or a hand
// passing over the sensor cannot start a measurement.
void updatePresence() {
  bool now_ = objectDetected();
  if (now_ != presenceRaw) {
    presenceRaw = now_;
    presenceChangedAt = millis();
  }
  if (millis() - presenceChangedAt >= PRESENCE_DEBOUNCE_MS) {
    presenceStable = presenceRaw;
  }
}

bool itemPresent() {
  return presenceStable;
}

// ---------- RPC surface ----------
int getMoisture()  { return lastRaw; }
int getDelta()     { return lastDelta; }
int getBaseline()  { return baseline; }

// Presence, so main.py keeps the same interface it always had.
int getItemPresent() {
  return itemPresent() ? 1 : 0;
}

int setIndicator(int newMode) {
  mode = newMode;
  lastToggle = millis();
  heartbeatMark = millis();
  sensingStart = millis();
  phase = false;
  ledsOff();

  if (newMode == MODE_IDLE) {
    idleSince = millis();
  } else if (newMode == BIN_WET || newMode == BIN_DRY) {
    // Count first so the display updates together with the LED, then hold the
    // tone. The colour and the number must both be visible for the whole beep.
    if (newMode == BIN_WET) wetCount++; else dryCount++;
    refreshCounters();
    digitalWrite(newMode == BIN_WET ? PIN_LED_WET : PIN_LED_DRY, HIGH);
    announce(newMode);
  } else if (newMode == BIN_HOLD) {
    announce(newMode);
  }
  // MODE_SENSING is silent - the blinking is the signal.
  return newMode;
}

int getWetCount() { return wetCount; }
int getDryCount() { return dryCount; }

// Let the Linux side restore counts after a reboot. The MCU has no storage,
// so persistence lives on the MPU and is pushed back down at startup.
int setWetCount(int n) { wetCount = n < 0 ? 0 : n; refreshCounters(); return wetCount; }
int setDryCount(int n) { dryCount = n < 0 ? 0 : n; refreshCounters(); return dryCount; }

int resetCounts(int unused) {
  (void)unused;
  wetCount = 0;
  dryCount = 0;
  refreshCounters();
  Monitor.println("counters reset");
  return 0;
}

int recalibrate() {
  calibrateBaseline();
  setIndicator(MODE_IDLE);
  armed = true;
  return baseline;
}

// Let the thresholds be tuned live from Python without reflashing.
int setDeltaPresent(int v) { DELTA_PRESENT = v; return v; }
int setDeltaWet(int v)     { DELTA_WET = v;     return v; }

// ---------------------------------------------------------------------------
// Indicator - non-blocking.
// Never use delay() here: Bridge dispatches its queued provide_safe callbacks
// from loop(), so blocking stalls RPC from the Linux side.
// ---------------------------------------------------------------------------
void runIndicator() {
  unsigned long now = millis();

  if (mode == MODE_IDLE) {
    // Brief green flash every 2 s. Without it, "idle" and "crashed" look
    // identical, which is a miserable thing to debug.
    unsigned long pos = (now - heartbeatMark) % HEARTBEAT_CYCLE_MS;
    digitalWrite(PIN_LED_WET, pos < HEARTBEAT_ON_MS ? HIGH : LOW);
    digitalWrite(PIN_LED_DRY, LOW);
    return;
  }

  if (mode == MODE_SENSING) {
    // Both LEDs together, in phase. Deliberately different from HOLD, which
    // alternates them - you can tell the two apart at a glance.
    if (now - lastToggle >= SENSING_BLINK_MS) {
      lastToggle = now;
      phase = !phase;
      digitalWrite(PIN_LED_WET, phase ? HIGH : LOW);
      digitalWrite(PIN_LED_DRY, phase ? HIGH : LOW);
    }
    return;
  }

  if (mode == BIN_WET || mode == BIN_DRY) {
    // Solid, not blinking. The result stays lit until the item is removed.
    digitalWrite(PIN_LED_WET, mode == BIN_WET ? HIGH : LOW);
    digitalWrite(PIN_LED_DRY, mode == BIN_DRY ? HIGH : LOW);
    return;
  }

  if (mode == BIN_HOLD) {
    if (now - lastToggle < BLINK_FAST_MS) return;
    lastToggle = now;
    phase = !phase;
    digitalWrite(PIN_LED_WET, phase ? HIGH : LOW);
    digitalWrite(PIN_LED_DRY, phase ? LOW  : HIGH);
  }
}

// ---------------------------------------------------------------------------
#if STANDALONE_MODE
void runStandalone() {
  bool present = itemPresent();

  // --- Measuring: both LEDs blinking, waiting out the sensing window ---
  if (mode == MODE_SENSING) {
    if (!present) {
      // Item pulled away mid-measurement. Abandon it, don't guess.
      setIndicator(MODE_IDLE);
      armed = true;
      Monitor.println("-- item removed during sensing, cancelled --");
      return;
    }
    if (millis() - sensingStart < SENSING_MS) return;   // still thinking

    lastRaw   = readRaw();
    lastDelta = deltaFrom(lastRaw);
    if (lastDelta > peakDelta) peakDelta = lastDelta;

    int decision = (lastDelta >= DELTA_WET) ? BIN_WET : BIN_DRY;
    setIndicator(decision);      // lights the LED, then beeps for 2 s
    armed = false;

    Monitor.print("DECISION: ");
    Monitor.print(decision == BIN_WET ? "WET" : "DRY");
    Monitor.print("  raw=");
    Monitor.print(lastRaw);
    Monitor.print("  delta=");
    Monitor.print(lastDelta);
    Monitor.print("  ir=");
    Monitor.print(irDetects() ? "yes" : "no");
    Monitor.print("  (wet>=");
    Monitor.print(DELTA_WET);
    Monitor.println(")");
    return;
  }

  // --- Showing a result: hold it until the item is taken away ---
  if (!armed) {
    if (!present) {
      setIndicator(MODE_IDLE);
      armed = true;
      Monitor.print("-- cleared, ready --  peak delta was ");
      Monitor.println(peakDelta);
      peakDelta = 0;
    }
    return;
  }

  // --- Idle: waiting for something to arrive ---
  if (!present) {
    // Long idle with nothing on the tray: re-baseline to absorb slow drift
    // from temperature or a slowly drying sensor.
    if (millis() - idleSince > REBASELINE_IDLE_MS) {
      Monitor.println("idle drift - re-baselining");
      calibrateBaseline();
    }
    return;
  }

  // Something arrived. Start the 3 s sensing window. Nothing blocks here -
  // the wait happens across loop() iterations so Bridge RPC stays alive.
  setIndicator(MODE_SENSING);
  Monitor.println("item detected - sensing...");
}
#endif

// ---------------------------------------------------------------------------
void sampleSensor() {
  unsigned long now = millis();
  if (now - lastSample < SAMPLE_EVERY_MS) return;
  lastSample = now;

  lastRaw   = readRaw();
  lastDelta = deltaFrom(lastRaw);
  if (lastDelta > peakDelta) peakDelta = lastDelta;
}

void reportStatus() {
  unsigned long now = millis();
  if (now - lastReport < REPORT_EVERY_MS) return;
  lastReport = now;

  Monitor.print("ir=");
  Monitor.print(irDetects() ? "DETECT" : "clear ");
  Monitor.print("  raw=");
  Monitor.print(lastRaw);
  Monitor.print("  baseline=");
  Monitor.print(baseline);
  Monitor.print("  delta=");
  Monitor.print(lastDelta);
  Monitor.print("  peak=");
  Monitor.print(peakDelta);
  Monitor.print("  present=");
  Monitor.print(itemPresent() ? "yes" : "no ");
  Monitor.print("  wet=");
  Monitor.print(wetCount);
  Monitor.print("  dry=");
  Monitor.print(dryCount);
  Monitor.print("  -> ");
  if (!itemPresent())                  Monitor.println("idle");
  else if (lastDelta >= DELTA_WET)     Monitor.println("WET");
  else                                 Monitor.println("DRY");
}

// ---------------------------------------------------------------------------
void setup() {
  pinMode(PIN_LED_WET, OUTPUT);
  pinMode(PIN_LED_DRY, OUTPUT);
  pinMode(PIN_IR, INPUT);
  pinMode(PIN_TRIGGER, INPUT_PULLUP);   // unconnected reads HIGH - harmless
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  ledsOff();

  dispWet.begin(PIN_WET_CLK, PIN_WET_DIO);
  dispDry.begin(PIN_DRY_CLK, PIN_DRY_DIO);
  dispWet.setBrightness(3);
  dispDry.setBrightness(3);

  selfTest();

  Bridge.begin();
  Monitor.begin();   // required for output to reach App Lab's Serial Monitor

  Monitor.println("Keep the tray EMPTY - baselining...");
  calibrateBaseline();

  // provide_safe(), not provide(). provide() dispatches on a background RPC
  // thread where GPIO and ADC calls can fail intermittently.
  Bridge.provide_safe("get_item_present",  getItemPresent);
  Bridge.provide_safe("get_moisture",      getMoisture);
  Bridge.provide_safe("get_delta",         getDelta);
  Bridge.provide_safe("get_baseline",      getBaseline);
  Bridge.provide_safe("recalibrate",       recalibrate);
  Bridge.provide_safe("indicate",          setIndicator);
  Bridge.provide_safe("set_delta_present", setDeltaPresent);
  Bridge.provide_safe("set_delta_wet",     setDeltaWet);
  Bridge.provide_safe("get_wet_count",     getWetCount);
  Bridge.provide_safe("get_dry_count",     getDryCount);
  Bridge.provide_safe("reset_counts",      resetCounts);
  Bridge.provide_safe("set_wet_count",      setWetCount);
  Bridge.provide_safe("set_dry_count",      setDryCount);

#if STANDALONE_MODE
  Monitor.println("Controller ready - STANDALONE");
#else
  Monitor.println("Controller ready - BRIDGE (waiting for Linux side)");
#endif
#if REQUIRE_IR
  Monitor.println("Trigger: IR (or D3 jumper) required for a decision");
#else
  Monitor.println("Trigger: IR, moisture shift, or D3 jumper");
#endif

  // Your ADC range: tops out near 1023 = 10-bit, near 4095 = 12-bit.
  // Scale the delta thresholds by 4x if you are on 12-bit.
  Monitor.print("raw A0 at boot: ");
  Monitor.println(analogRead(PIN_MOISTURE));

  heartbeatMark = millis();
  idleSince = millis();
}

void loop() {
  sampleSensor();
  updatePresence();
#if STANDALONE_MODE
  runStandalone();
#endif
  runIndicator();
  reportStatus();
}
