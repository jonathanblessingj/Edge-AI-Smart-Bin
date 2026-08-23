"""Waste segregator - Linux side.

Owns the decision. The MCU owns the hardware and does what it is told.

REQUIRES: set STANDALONE_MODE to 0 in sketch.ino, or the MCU will keep
deciding on its own and the two will fight over the indicator.

WHY VISION IS WORTH ADDING
  Moisture alone gets one whole category wrong: DRY ORGANIC waste. Eggshells,
  dry leaves, bread crusts and nut shells are all compostable but physically
  dry, so a moisture sensor files them as recyclable. The camera identifies
  WHAT the object is; the moisture sensor measures HOW WET it is. Fusing them
  covers both failure modes:

    wet + anything      -> WET   (moisture overrides; wet contaminates recycling)
    dry + organic       -> WET   (this is the case vision unlocks)
    dry + paper/plastic -> DRY
    unsure              -> HOLD  (refuse to guess)

GRACEFUL DEGRADATION
  Every Brick import is optional. Run this today with no model trained and it
  works as a moisture-only sorter with a dashboard. Add the model later and the
  vision path switches on by itself. Nothing to rewire.
"""

import csv
import json
import os
import time
from datetime import datetime, UTC
from pathlib import Path

from arduino.app_utils import App, Bridge

# --------------------------------------------------------------------------
# Brick discovery.
#
# Module paths differ between App Lab versions, so rather than hard-code a
# guess we try a list of candidates and, if none match, print what IS
# installed. That turns "not found" into an answer instead of a dead end.
# --------------------------------------------------------------------------
import importlib
import pkgutil
import traceback

HAS_UI = False
HAS_VISION = False
WebUI = None
Vision = None


def _list_installed_bricks():
    """NOTE: this lists the whole shipped catalogue, not what is enabled.
    A name appearing here does not mean the Brick is active."""
    try:
        import arduino.app_bricks as _b
        names = sorted(m.name for m in pkgutil.iter_modules(_b.__path__))
        print("   modules present in the catalogue:", ", ".join(names) if names else "(none)")
        return names
    except Exception as e:
        print("   could not enumerate arduino.app_bricks:", e)
        return []


def _try_import(candidates, what):
    """candidates is [(module_path, class_name), ...]. Returns the class.

    Catches Exception, not just ImportError. A Brick module that exists but
    fails partway through - a missing dependency, a bad config - raises
    something else entirely, and swallowing that turns a fixable error into a
    misleading "not found".
    """
    for mod_path, cls_name in candidates:
        try:
            mod = importlib.import_module(mod_path)
        except ModuleNotFoundError as e:
            if e.name == mod_path:
                continue                      # genuinely absent, try the next
            print(f"{what}: {mod_path} exists but needs '{e.name}', which is missing")
            print(f"        -> the Brick is probably not enabled in app.yaml")
            continue
        except Exception as e:
            print(f"{what}: {mod_path} failed to import: {type(e).__name__}: {e}")
            traceback.print_exc()
            continue

        obj = getattr(mod, cls_name, None)
        if obj is not None:
            print(f"{what} Brick -> {mod_path}.{cls_name}")
            return obj
        public = [n for n in dir(mod) if not n.startswith("_") and n[0].isupper()]
        print(f"{what}: found {mod_path} but no {cls_name}. Classes: {public}")
    print(f"{what} Brick unavailable.")
    _list_installed_bricks()
    return None


WebUI = _try_import([
    ("arduino.app_bricks.web_ui", "WebUI"),
    ("arduino.app_bricks.webui", "WebUI"),
    ("arduino.app_bricks.webui_html", "WebUI"),
], "web_ui")
HAS_UI = WebUI is not None

Vision = _try_import([
    ("arduino.app_bricks.video_classification", "VideoClassification"),
    ("arduino.app_bricks.video_imageclassification", "VideoImageClassification"),
    ("arduino.app_bricks.video_image_classification", "VideoImageClassification"),
    ("arduino.app_bricks.image_classification", "ImageClassification"),
    ("arduino.app_bricks.video_objectdetection", "VideoObjectDetection"),
], "vision")
HAS_VISION = Vision is not None

# --------------------------------------------------------------------------
# Tunables
# --------------------------------------------------------------------------
DELTA_WET_DEFAULT = 150   # starting judgement; tune live from the dashboard

# Two DIFFERENT thresholds - keeping them separate matters.
#   REPORT: how confident the Brick must be before it tells us anything.
#           Keep this LOW so we can see what the model actually thinks.
#   TRUST:  how confident we must be before acting on it. This is the real
#           decision threshold.
# Setting REPORT high (the mistake) means the callback never fires on an
# uncertain model, and every decision silently falls back to moisture.
VISION_REPORT_THRESHOLD = 0.05
CONFIDENCE_MIN          = 0.65
SENSING_S         = 3.0   # must match SENSING_MS in the sketch
VISION_MAX_AGE_S  = 4.0
POLL_S            = 0.1

# Live delta straight to the console. This is what the dashboard slider was
# for - you do not need a browser to calibrate, and this path cannot break.
CALIBRATION_LOG   = True
CAL_MIN_CHANGE    = 4     # only print when delta moves by at least this much

# Vision logging. With a stock 1000-class model pointed at a room, printing
# every classification is pure noise. Only log while an item is actually being
# measured; otherwise a rare heartbeat is enough to show the camera is alive.
VISION_LOG_IDLE_S = 60    # seconds between idle heartbeat lines
MAX_UNKNOWN_NOTES = 8     # stop listing unmapped labels after this many

# Camera preview in the browser. Frames go over the existing WebSocket as
# base64 JPEG. Kept small and slow on purpose - this is a framing aid, not a
# video product, and the board has better things to do than encode video.
CAMERA_PREVIEW    = True
PREVIEW_FPS       = 4
PREVIEW_WIDTH     = 320
PREVIEW_QUALITY   = 60

# Mode ids - must match the sketch
BIN_WET, BIN_DRY, BIN_HOLD, MODE_IDLE, MODE_SENSING = 0, 1, 2, 3, 4
NAMES = {BIN_WET: "WET", BIN_DRY: "DRY", BIN_HOLD: "HOLD"}

# Model class -> stream. Change this without retraining the model.
LABEL_TO_BIN = {
    "organic": BIN_WET,
    "paper":   BIN_DRY,
    "plastic": BIN_DRY,
}

STATE_DIR = Path(os.environ.get("HOME", "/tmp"))
COUNTS_FILE = STATE_DIR / "waste_sorter_counts.json"
LOG_FILE = STATE_DIR / "waste_sorter_log.csv"

# --------------------------------------------------------------------------
state = {
    "delta_wet": DELTA_WET_DEFAULT,
    "armed": True,
    "sensing_until": 0.0,
    "phase": "idle",
}
latest = {"label": None, "confidence": 0.0, "ts": 0.0}
vision_stats = {"callbacks": 0, "last_log": 0.0, "warned": False}
cal = {"last": None, "peak": 0, "last_print": 0.0}
preview = {"src": None, "read": None, "last": 0.0, "fails": 0, "reported": False}

ui = WebUI() if HAS_UI else None

vision = None
if HAS_VISION:
    try:
        vision = Vision(confidence=VISION_REPORT_THRESHOLD, debounce_sec=0.0)
    except TypeError:
        # Constructor signature differs between Brick versions.
        try:
            vision = Vision(confidence=VISION_REPORT_THRESHOLD)
        except TypeError:
            vision = Vision()
    api = [m for m in dir(vision) if not m.startswith("_")]
    print(f"vision API: {api}")


# --------------------------------------------------------------------------
# Persistence. The MCU has no storage, so counts live here and get pushed
# back down at startup.
# --------------------------------------------------------------------------
def load_counts():
    try:
        data = json.loads(COUNTS_FILE.read_text())
        return int(data.get("wet", 0)), int(data.get("dry", 0))
    except Exception:
        return 0, 0


def save_counts(wet, dry):
    try:
        COUNTS_FILE.write_text(json.dumps({"wet": wet, "dry": dry}))
    except Exception as e:
        print(f"could not save counts: {e}")


def log_decision(row):
    try:
        new = not LOG_FILE.exists()
        with LOG_FILE.open("a", newline="") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["timestamp", "decision", "reason", "label",
                            "confidence", "delta", "raw"])
            w.writerow(row)
    except Exception as e:
        print(f"could not write log: {e}")


# --------------------------------------------------------------------------
def on_vision(results):
    """Brick callback. Normally {label: confidence}; tolerate other shapes."""
    if not results:
        return
    if isinstance(results, dict):
        pairs = list(results.items())
    elif isinstance(results, (list, tuple)):
        # e.g. [{"label": "organic", "value": 0.9}, ...]
        pairs = []
        for r in results:
            if isinstance(r, dict):
                lbl = r.get("label") or r.get("name") or r.get("class")
                val = r.get("value") or r.get("confidence") or r.get("score") or 0
                if lbl:
                    pairs.append((lbl, val))
    else:
        print(f"vision returned an unexpected shape: {type(results)} {results!r}")
        return
    if not pairs:
        return

    pairs.sort(key=lambda kv: kv[1], reverse=True)
    label, conf = pairs[0]
    latest.update(label=label, confidence=float(conf), ts=time.time())

    vision_stats["callbacks"] += 1
    now = time.time()

    # Loud while measuring, near-silent when idle.
    measuring = state["phase"] == "sensing"
    interval = 1.0 if measuring else VISION_LOG_IDLE_S
    if now - vision_stats["last_log"] >= interval:
        vision_stats["last_log"] = now
        top = ", ".join(f"{l} {float(c):.2f}" for l, c in pairs[:3])
        print(f"  vision{'*' if measuring else ''}: {top}")


if vision is not None:
    # Callback name varies by Brick version - bind to whichever exists.
    for hook in ("on_detect_all", "on_classify", "on_result", "on_detect",
                 "on_classification", "on_inference"):
        fn = getattr(vision, hook, None)
        if callable(fn):
            fn(on_vision)
            print(f"vision callback -> {hook}()")
            break
    else:
        print("WARNING: no known result callback on the vision Brick.")
        print("         Check the 'vision API' list above and tell me which to use.")


def decide(label, confidence, delta):
    """Returns (bin_id, reason)."""

    # 1. Physically wet always wins. A wet item in the dry stream contaminates
    #    everything downstream, so no amount of visual confidence overrides it.
    if delta >= state["delta_wet"]:
        return BIN_WET, f"moisture: delta {delta} >= {state['delta_wet']}"

    # 2. Physically dry. Without vision we can only call it dry.
    if not HAS_VISION:
        return BIN_DRY, f"moisture only: dry (delta {delta})"

    if label is None:
        if vision_stats["callbacks"] == 0:
            return BIN_DRY, f"dry (delta {delta}) - vision has never reported"
        return BIN_DRY, f"dry (delta {delta}) - vision saw nothing this time"

    if time.time() - latest["ts"] > VISION_MAX_AGE_S:
        age = time.time() - latest["ts"]
        return BIN_DRY, f"dry (delta {delta}) - vision stale by {age:.1f}s"

    if label == "empty":
        return BIN_HOLD, "camera sees an empty tray"

    if label in LABEL_TO_BIN:
        if confidence >= CONFIDENCE_MIN:
            target = LABEL_TO_BIN[label]
            if target == BIN_WET:
                # The case moisture alone cannot get right.
                return BIN_WET, f"vision: dry organic ({label} {confidence:.2f})"
            return BIN_DRY, f"vision: {label} {confidence:.2f}, dry"

        # A known class, but the model is hedging between them. Wet in the dry
        # bin ruins recyclables; dry in the wet bin ruins compost. Refusing to
        # guess is the correct answer here.
        return BIN_HOLD, f"unsure ({label}, {confidence:.2f})"

    # 3. The label is not one of ours at all - the model has no concept of this
    #    object. That is NOT the same as being unsure between known classes, so
    #    do not HOLD. Fall back to the physical measurement, which is exactly
    #    as good as it was before vision was switched on.
    note_unknown_label(label)
    return BIN_DRY, f"moisture: dry (vision saw '{label}', not a known class)"


def resolve_frame_source():
    """Find something that hands us camera frames.

    The Brick owns /dev/video0, so we cannot open the camera a second time -
    we have to borrow the one it is already using. Its attribute name is not
    documented, so try the plausible ones and report what we find.
    """
    if vision is None:
        return None, None

    cam = None
    for attr in ("camera", "_camera", "cam", "_cam", "video", "_video"):
        c = getattr(vision, attr, None)
        if c is not None and not callable(c):
            cam = c
            print(f"preview: camera object via vision.{attr} ({type(c).__name__})")
            break

    if cam is None:
        print("preview: no camera attribute on the vision Brick.")
        print(f"         vision attrs: {[a for a in dir(vision) if not a.startswith('__')]}")
        return None, None

    for meth in ("read", "capture", "get_frame", "grab", "latest_frame",
                 "get_latest_frame", "frame"):
        fn = getattr(cam, meth, None)
        if callable(fn):
            print(f"preview: frame reader -> {meth}()")
            return cam, fn

    print("preview: camera object has no recognised read method.")
    print(f"         camera attrs: {[a for a in dir(cam) if not a.startswith('_')]}")
    return cam, None


def encode_jpeg(frame):
    """Frame -> base64 JPEG string, resized down."""
    import base64
    import cv2
    if frame is None:
        return None
    if isinstance(frame, tuple):        # some read() return (ok, frame)
        if not frame[0]:
            return None
        frame = frame[1]
    h, w = frame.shape[:2]
    if w > PREVIEW_WIDTH:
        scale = PREVIEW_WIDTH / float(w)
        frame = cv2.resize(frame, (PREVIEW_WIDTH, int(h * scale)))
    ok, buf = cv2.imencode(".jpg", frame,
                           [int(cv2.IMWRITE_JPEG_QUALITY), PREVIEW_QUALITY])
    if not ok:
        return None
    return base64.b64encode(buf.tobytes()).decode("ascii")


def push_preview():
    """Send one frame to the browser, rate-limited. Never fatal."""
    if not (CAMERA_PREVIEW and ui and vision):
        return
    now = time.time()
    if now - preview["last"] < 1.0 / PREVIEW_FPS:
        return
    preview["last"] = now

    if preview["read"] is None:
        if preview["reported"]:
            return
        preview["reported"] = True
        preview["src"], preview["read"] = resolve_frame_source()
        if preview["read"] is None:
            print("preview: disabled - no usable frame source")
            return

    try:
        data = encode_jpeg(preview["read"]())
        if data:
            ui.send_message("frame", {"jpeg": data})
            preview["fails"] = 0
    except Exception as e:
        preview["fails"] += 1
        if preview["fails"] in (1, 20):
            print(f"preview: frame grab failed ({type(e).__name__}: {e})")
        if preview["fails"] > 50:
            preview["read"] = None
            print("preview: giving up after repeated failures")


def log_delta(delta, present):
    """Print delta whenever it moves meaningfully, with a bar and the peak.

    Hold a wet item and watch the number climb; hold a dry one and watch it
    stay flat. Set DELTA_WET between the two clusters. That is the whole
    calibration procedure, and it needs no browser.
    """
    now = time.time()
    if cal["last"] is None or abs(delta - cal["last"]) >= CAL_MIN_CHANGE:
        cal["last"] = delta
        cal["peak"] = max(cal["peak"], delta)
        bar_n = max(0, min(40, delta // 15))
        verdict = "WET" if delta >= state["delta_wet"] else "dry"
        print(f"  delta {delta:>4} |{'#' * bar_n:<40}| peak {cal['peak']:>4} "
              f"thr {state['delta_wet']:>4} -> {verdict}"
              f"{'  [item]' if present else ''}")
        cal["last_print"] = now
    elif not present and delta < CAL_MIN_CHANGE and now - cal["last_print"] > 30:
        cal["last_print"] = now
        cal["peak"] = 0     # forget the peak after a long quiet spell


def publish(payload):
    if ui:
        ui.send_message("update", payload)


_unknown_labels = set()


def note_unknown_label(label):
    """Log each unmapped label once.

    Useful in two situations: the stock model emits generic ImageNet labels
    that mean nothing here, and a custom model's class names may not match
    LABEL_TO_BIN exactly. Either way this tells you what the model actually
    calls things, so you can map them without guessing.
    """
    if not label or label in _unknown_labels:
        return
    _unknown_labels.add(label)
    if len(_unknown_labels) <= MAX_UNKNOWN_NOTES:
        print(f"  note: model reports '{label}', which is not in LABEL_TO_BIN")
    elif len(_unknown_labels) == MAX_UNKNOWN_NOTES + 1:
        print(f"  note: {MAX_UNKNOWN_NOTES}+ unmapped labels seen - "
              f"the stock model has no waste classes, so this is expected. "
              f"Silencing further notes.")


# --------------------------------------------------------------------------
def set_threshold(v):
    try:
        v = int(v)
    except (TypeError, ValueError):
        return
    state["delta_wet"] = v
    Bridge.call("set_delta_wet", v)
    print(f"threshold -> {v}")


def do_reset():
    Bridge.call("reset_counts", 0)
    save_counts(0, 0)
    print("counts reset")


def boot():
    wet, dry = load_counts()
    Bridge.call("set_wet_count", wet)
    Bridge.call("set_dry_count", dry)
    Bridge.call("set_delta_wet", state["delta_wet"])
    Bridge.call("indicate", MODE_IDLE)

    if ui:
        ui.on_message("set_threshold", lambda sid, v: set_threshold(v))
        ui.on_message("reset_counts", lambda sid, _=None: do_reset())
        ui.on_message("recalibrate", lambda sid, _=None: Bridge.call("recalibrate"))

        # Push current state the moment a browser connects, so the page is not
        # blank until the next poll. on_connect may not exist on every Brick
        # version, hence the guard.
        if hasattr(ui, "on_connect"):
            ui.on_connect(lambda sid: ui.send_message("welcome", {
                "threshold": state["delta_wet"],
                "wet": Bridge.call("get_wet_count"),
                "dry": Bridge.call("get_dry_count"),
                "phase": state["phase"],
            }))

    print("=" * 66)
    print(f"Waste sorter ready   vision={HAS_VISION}   ui={HAS_UI}")
    print(f"restored counts: wet={wet} dry={dry}")
    print(f"threshold: delta >= {state['delta_wet']} means WET")
    print(f"state dir: {STATE_DIR}")
    print("=" * 66)


_booted = False
_t0 = [time.time()]


def loop():
    global _booted
    if not _booted:
        _booted = True
        _t0[0] = time.time()
        boot()

    time.sleep(POLL_S)

    # If vision is up but has never produced a single classification, say so
    # once. Silence here is otherwise indistinguishable from "no vision".
    if HAS_VISION and not vision_stats["warned"] and vision_stats["callbacks"] == 0:
        if time.time() - _t0[0] > 15:
            vision_stats["warned"] = True
            print("WARNING: vision Brick is running but has reported nothing in 15s.")
            print("         Camera may be pointed at nothing, too dark, or the")
            print("         model may be below even the 0.05 report threshold.")

    present = bool(Bridge.call("get_item_present"))
    delta = Bridge.call("get_delta")

    if CALIBRATION_LOG:
        log_delta(delta, present)

    push_preview()

    # Live telemetry so the dashboard shows delta while you tune the slider.
    publish({
        "phase": state["phase"],
        "delta": delta,
        "threshold": state["delta_wet"],
        "present": present,
        "label": latest["label"],
        "confidence": round(latest["confidence"], 2),
    })

    # --- measuring ---
    if state["phase"] == "sensing":
        if not present:
            state["phase"] = "idle"
            state["armed"] = True
            Bridge.call("indicate", MODE_IDLE)
            print("cancelled - item removed during sensing")
            return
        if time.time() < state["sensing_until"]:
            return

        raw = Bridge.call("get_moisture")
        delta = Bridge.call("get_delta")
        label, conf = latest["label"], latest["confidence"]

        bin_id, reason = decide(label, conf, delta)
        Bridge.call("indicate", bin_id)   # MCU lights, beeps, counts, displays

        wet = Bridge.call("get_wet_count")
        dry = Bridge.call("get_dry_count")
        save_counts(wet, dry)

        ts = datetime.now(UTC).isoformat()
        log_decision([ts, NAMES[bin_id], reason, label or "-",
                      round(conf, 2), delta, raw])
        print(f"[{NAMES[bin_id]}] {reason}  | wet={wet} dry={dry}")

        publish({"phase": "result", "decision": NAMES[bin_id], "reason": reason,
                 "wet": wet, "dry": dry, "delta": delta, "timestamp": ts})

        state["phase"] = "result"
        state["armed"] = False
        return

    # --- showing a result, wait for removal ---
    if not state["armed"]:
        if not present:
            state["armed"] = True
            state["phase"] = "idle"
            Bridge.call("indicate", MODE_IDLE)
        return

    # --- idle ---
    if not present:
        return

    state["phase"] = "sensing"
    state["sensing_until"] = time.time() + SENSING_S
    Bridge.call("indicate", MODE_SENSING)   # both LEDs blink for 3 s


App.run(user_loop=loop)
