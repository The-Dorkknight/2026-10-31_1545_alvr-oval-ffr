"""Live numbers from the running VR game, the fps log, and commands sent to the game.

* The layer writes status-<pid>.txt about once a second (fps, refresh rate, missed frames, ...).
* It writes fps_log.csv: one row per setting, averaged over 2 minutes (or over a shorter time if the
  setting was changed sooner, at least 20 s). summarise() turns that into "fps per setting".
* send_command() writes commands.txt; the layer in the focused VR game reads and deletes it within
  about half a second. Same commands as the Ctrl+Alt keys.
"""
import csv
import io
import os
import time

from . import config as cfgmod
from . import installer

# command -> (key while playing, what it does)
COMMANDS = {
    "overlay": ("Ctrl+Alt+O", "fps box in the headset on/off"),
    "zones": ("Ctrl+Alt+Z", "show the low-res zones in the headset (yellow 2x2, red 4x4, purple = not applied)"),
    "ffr": ("Ctrl+Alt+F", "foveated rendering on/off"),
    "smaller": ("Ctrl+Alt+[", "sharp area 5% smaller"),
    "bigger": ("Ctrl+Alt+]", "sharp area 5% bigger"),
    "edge": ("Ctrl+Alt+E", "low-res edge 2x2 / 4x4"),
    "smooth": ("Ctrl+Alt+S", "Smooth low-res edge on/off"),
    "auto": ("Ctrl+Alt+A", "Auto foveation on/off"),
    "abtest": ("Ctrl+Alt+T", "A/B test: FFR off/on/off/on, 10 s each, then the result (press again to stop)"),
}

LIVE_MAX_AGE_S = 10.0


def commands_path():
    return os.path.join(cfgmod.data_dir(), "commands.txt")


def send_command(cmd, now=None):
    """Queue a command for the running VR game. Raises ValueError for an unknown command."""
    if cmd not in COMMANDS:
        raise ValueError("unknown command %r (known: %s)" % (cmd, ", ".join(sorted(COMMANDS))))
    line = "%d %s\n" % (int(now if now is not None else time.time()), cmd)
    with open(commands_path(), "a", encoding="utf-8", newline="\n") as f:
        f.write(line)
    return commands_path()


def _f(d, k, default=None):
    try:
        return float(d.get(k, ""))
    except (TypeError, ValueError):
        return default


def live_status():
    """Status of a VR game running right now (status written in the last few seconds), else None."""
    st = installer.read_status()
    if not st or st.get("ended") == "1" or st.get("age_s", 1e9) > LIVE_MAX_AGE_S:
        return None
    if st.get("state") != "active":
        return None
    return st


def describe_live(st):
    """(headline, [detail lines]) for a live status dict, in plain words."""
    if not st:
        return "No VR game running", ["Start a game through ALVR; numbers appear here within a few seconds."]
    fps, hz = _f(st, "fps", -1), _f(st, "hz", 0)
    on = st.get("ffr_now", "1") == "1"
    head = ("%.0f fps / %.0f Hz" % (fps, hz) if fps is not None and fps >= 0 else "-- fps") + ("  FFR on" if on else "  FFR off")
    lines = []
    miss = _f(st, "missed_pct")
    if miss is not None:
        lines.append("Late frames: %.0f%%" % miss)
    spare = _f(st, "spare_ms")
    if spare is not None:
        lines.append("Spare time per frame: %.1f ms (rough; more = more headroom)" % spare)
    sav = _f(st, "saving")
    if sav is not None:
        lines.append("Estimated shading saved: %.0f%%" % (sav * 100) if on else "Estimated shading saved: 0% (FFR off)")
    vrs = _f(st, "vrs_seen", -1)
    if vrs is not None and vrs >= 0:
        verdict = "working" if vrs >= 70 else ("partly" if vrs >= 25 else "NOT reaching the image the headset gets")
        lines.append("VRS check: %.0f%% of sampled low-res blocks really are low-res - %s" % (vrs, verdict))
    else:
        lines.append("VRS check: turn on Zones (Ctrl+Alt+Z) or Smooth low-res edge to measure it")
    ab = st.get("ab", "idle")
    if ab.startswith("running"):
        lines.append("A/B test running: FFR %s now" % ("on" if ab.endswith("on") else "off"))
    elif st.get("ab_result"):
        lines.append("Last A/B test: " + st["ab_result"])
    return head, lines


def log_path():
    return os.path.join(cfgmod.data_dir(), "fps_log.csv")


def read_log(path=None):
    """Rows of fps_log.csv as dicts. Missing or damaged file -> []. Damaged rows are skipped."""
    path = path or log_path()
    try:
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
    except OSError:
        return []
    rows = []
    for r in csv.DictReader(io.StringIO(text)):
        if _f(r, "avg_fps") is None or _f(r, "seconds") is None:
            continue
        rows.append(r)
    return rows


def setting_label(r):
    if r.get("ffr") == "0":
        return "FFR off"
    parts = ["FFR on %d%% x %d%%" % (round((_f(r, "h", 0)) * 100), round((_f(r, "v", 0)) * 100)),
             "edge " + (r.get("edge") or "?")]
    if r.get("smooth") == "1":
        parts.append("smooth %d%%" % round(_f(r, "smooth_strength", 0) * 100))
    if r.get("auto") == "1":
        parts.append("auto")
    return ", ".join(parts)


def summarise(rows, app=None):
    """Average fps per setting, weighted by measured time. Uses the newest row's game unless `app`
    is given. Returns (app, [dict(label, fps, missed, seconds, rows)]), best fps first."""
    if not rows:
        return None, []
    app = app if app is not None else rows[-1].get("app", "")
    groups = {}
    for r in rows:
        if r.get("app", "") != app:
            continue
        secs = _f(r, "seconds", 0) or 0
        if secs <= 0:
            continue
        lab = setting_label(r)
        g = groups.setdefault(lab, dict(label=lab, fps_t=0.0, miss_t=0.0, miss_s=0.0, seconds=0.0, rows=0))
        g["fps_t"] += _f(r, "avg_fps", 0) * secs
        m = _f(r, "missed_pct")
        if m is not None:
            g["miss_t"] += m * secs
            g["miss_s"] += secs
        g["seconds"] += secs
        g["rows"] += 1
    out = []
    for g in groups.values():
        out.append(dict(label=g["label"], fps=g["fps_t"] / g["seconds"],
                        missed=(g["miss_t"] / g["miss_s"]) if g["miss_s"] else None,
                        seconds=g["seconds"], rows=g["rows"]))
    out.sort(key=lambda d: -d["fps"])
    return app, out
