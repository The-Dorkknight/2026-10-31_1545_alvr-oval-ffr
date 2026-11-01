"""Config shared with the C++ layer (key=value file, hot-reloaded by the layer)."""
import math
import os
import sys
from dataclasses import dataclass, asdict

DEFAULTS = dict(enabled=True, h=0.70, v=0.70, shape=2.0, band=0.50, outer=2, centre_on_lens=True,
                auto_fov=False, auto_floor=0.60, smooth=False, smooth_strength=0.60,
                overlay=False)


def _fin(v, default):
    try:
        v = float(v)
    except (TypeError, ValueError):
        return default
    return v if math.isfinite(v) else default


@dataclass
class Config:
    enabled: bool = True
    h: float = 0.70       # full-quality oval width,  fraction of view width
    v: float = 0.70       # full-quality oval height, fraction of view height
    shape: float = 2.0    # superellipse exponent: 2 = oval, 4 = slot
    band: float = 0.50    # middle ring thickness, fraction of the inner oval
    outer: int = 2        # log2 of outer rate: 1 = 2x2, 2 = 4x4
    centre_on_lens: bool = True
    auto_fov: bool = False    # Auto foveation: shrink the sharp area only while frames are being missed
    auto_floor: float = 0.60  # ...but never below this fraction of the sliders (0.40..1.00)
    smooth: bool = False      # Smooth periphery: blend the blocky low-res pixels (experimental, off by default)
    smooth_strength: float = 0.60  # 0..1
    overlay: bool = False     # small fps box in the headset (Ctrl+Alt+O while playing)

    def clamped(self):
        """Same rules as the C++ clamp(): NaN / infinity fall back to the defaults (not to a limit)."""
        c = Config(**asdict(self))
        c.h = min(1.0, max(0.20, _fin(c.h, 0.70)))
        c.v = min(1.0, max(0.20, _fin(c.v, 0.70)))
        c.shape = min(6.0, max(2.0, _fin(c.shape, 2.0)))
        c.band = min(2.0, max(0.0, _fin(c.band, 0.50)))
        try:
            o = int(c.outer)
        except (TypeError, ValueError, OverflowError):
            o = 2
        c.outer = 1 if o < 1 else (2 if o > 2 else o)
        c.auto_floor = min(1.0, max(0.40, _fin(c.auto_floor, 0.60)))
        c.smooth_strength = min(1.0, max(0.0, _fin(c.smooth_strength, 0.60)))
        return c


def data_dir(create=True):
    """Same folder the C++ layer uses. OVALFFR_DIR overrides it (used by the tests)."""
    override = os.environ.get("OVALFFR_DIR")
    if override:
        d = override
    elif sys.platform.startswith("win"):
        d = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "OvalFFR")
    elif sys.platform == "darwin":
        d = os.path.expanduser("~/Library/Application Support/OvalFFR")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
        d = os.path.join(base, "ovalffr")
    if create:
        os.makedirs(d, exist_ok=True)
    return d


def config_path():
    return os.path.join(data_dir(), "config.ini")


def to_text(c: Config) -> str:
    c = c.clamped()
    return ("# OvalFFR config (written by the UI, hot-reloaded by the layer)\n"
            "enabled=%d\nh=%.4f\nv=%.4f\nshape=%.3f\nband=%.4f\nouter=%d\ncentre_on_lens=%d\n"
            "auto=%d\nauto_floor=%.3f\nsmooth=%d\nsmooth_strength=%.3f\noverlay=%d\n"
            % (c.enabled, c.h, c.v, c.shape, c.band, c.outer, c.centre_on_lens, c.auto_fov, c.auto_floor,
               c.smooth, c.smooth_strength, c.overlay))


def parse(text: str) -> Config:
    c = Config()
    for line in text.lstrip("\ufeff").splitlines():     # tolerate a BOM from Notepad
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        try:
            if k == "enabled": c.enabled = int(v) != 0
            elif k == "h": c.h = float(v)
            elif k == "v": c.v = float(v)
            elif k == "shape": c.shape = float(v)
            elif k == "band": c.band = float(v)
            elif k == "outer": c.outer = int(v)
            elif k == "centre_on_lens": c.centre_on_lens = int(v) != 0
            elif k == "auto": c.auto_fov = int(v) != 0
            elif k == "auto_floor": c.auto_floor = float(v)
            elif k == "smooth": c.smooth = int(v) != 0
            elif k == "smooth_strength": c.smooth_strength = float(v)
            elif k == "overlay": c.overlay = int(v) != 0
        except (ValueError, OverflowError):
            pass
    return c.clamped()


def load() -> Config:
    try:
        with open(config_path(), "r", encoding="utf-8", errors="replace") as f:
            return parse(f.read())
    except OSError:
        return Config()


def save(c: Config) -> str:
    """Write atomically so the layer never reads a half-written file."""
    path = config_path()
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(to_text(c))
    os.replace(tmp, path)
    return path
