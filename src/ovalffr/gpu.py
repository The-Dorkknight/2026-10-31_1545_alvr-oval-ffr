"""Dual-GPU laptops: make VR run on the gaming GPU.

Why this is not just "force the game": in OpenXR the *runtime* (SteamVR, which hosts ALVR) tells the
game which graphics adapter to create its device on (xrGet*GraphicsRequirementsKHR returns the
runtime's adapter LUID). If SteamVR itself starts on the integrated GPU, every game is dragged onto
it. So on Windows the lever is Windows' own per-app GPU preference for the *VR programs*
(Settings > System > Display > Graphics), which this module writes, reads back and can undo.

The layer (C++) reports what it sees in status-<pid>.txt (gpu_verdict=...). This module turns that
into plain words and offers the fix.

Windows : HKCU\\Software\\Microsoft\\DirectX\\UserGpuPreferences, value name = full exe path,
          data "GpuPreference=2;" (2 = high performance, 1 = power saving, 0 = let Windows decide).
Linux   : nothing is written anywhere. tools/ovalffr-gpu-linux.sh sets the PRIME offload variables for
          one launch (NVIDIA: __NV_PRIME_RENDER_OFFLOAD=1, __VK_LAYER_NV_optimus=NVIDIA_only; Mesa:
          DRI_PRIME=1).
"""
import json
import os
import re
import sys

from . import config as cfgmod

IS_WIN = sys.platform.startswith("win")
REG_PATH = r"Software\Microsoft\DirectX\UserGpuPreferences"
HIGH = "GpuPreference=2;"

# Programs that make up "the VR runtime". Only vrserver.exe is backed by a user report that setting
# it fixed a wrong-GPU problem; the others are included because they are the same SteamVR process
# family. Say so honestly in the docs.
STEAMVR_EXES = ("vrserver.exe", "vrcompositor.exe", "vrstartup.exe", "vrmonitor.exe")

VERDICT_TEXT = {
    "ok": (True, "VR is running on your fastest graphics card."),
    "single": (None, "Only one graphics card found, nothing to switch."),
    "game_on_weak": (False, "The game is on a slower graphics card than your fastest one."),
    "runtime_on_weak": (False, "SteamVR / ALVR is on a slower graphics card, so every VR game is dragged onto it."),
    "unknown": (None, "Not enough information yet (start a VR game, then press Check)."),
}


# ---- registry stores -----------------------------------------------------------------------

class MemoryStore:
    """In-memory stand-in used by the tests and on non-Windows systems."""

    def __init__(self, initial=None):
        self.data = dict(initial or {})

    def get(self, name):
        return self.data.get(name)

    def set(self, name, value):
        self.data[name] = value

    def delete(self, name):
        self.data.pop(name, None)


class WinRegStore:
    """The real thing (HKCU, no admin needed)."""

    def __init__(self):
        import winreg
        self.w = winreg

    def get(self, name):
        try:
            with self.w.OpenKey(self.w.HKEY_CURRENT_USER, REG_PATH, 0, self.w.KEY_READ) as k:
                v, _ = self.w.QueryValueEx(k, name)
                return v
        except OSError:
            return None

    def set(self, name, value):
        with self.w.CreateKeyEx(self.w.HKEY_CURRENT_USER, REG_PATH, 0, self.w.KEY_SET_VALUE) as k:
            self.w.SetValueEx(k, name, 0, self.w.REG_SZ, value)

    def delete(self, name):
        try:
            with self.w.OpenKey(self.w.HKEY_CURRENT_USER, REG_PATH, 0, self.w.KEY_SET_VALUE) as k:
                self.w.DeleteValue(k, name)
        except OSError:
            pass


def default_store():
    return WinRegStore() if IS_WIN else MemoryStore()


# ---- preference string handling ------------------------------------------------------------

def parse_pref(value):
    """'A=1;GpuPreference=2;B=3;' -> ordered list of (key, val). Tolerates junk and spaces."""
    out = []
    for part in (value or "").split(";"):
        part = part.strip()
        if "=" in part:
            k, v = part.split("=", 1)
            if k.strip():                          # "=5;" is junk, never write it back
                out.append((k.strip(), v.strip()))
    return out


def with_gpu_pref(value, level):
    """Return `value` with GpuPreference set to `level`, keeping every other setting Windows stored."""
    items = [(k, v) for k, v in parse_pref(value) if k != "GpuPreference"]
    items.append(("GpuPreference", str(int(level))))
    return "".join("%s=%s;" % kv for kv in items)


def pref_level(value):
    for k, v in parse_pref(value):
        if k == "GpuPreference":
            try:
                return int(v)
            except ValueError:
                return None
    return None


# ---- finding the programs --------------------------------------------------------------------

def _steam_roots():
    roots = []
    if IS_WIN:
        try:
            import winreg
            for hive, sub in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                              (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
                try:
                    with winreg.OpenKey(hive, sub, 0, winreg.KEY_READ) as k:
                        v, _ = winreg.QueryValueEx(k, "SteamPath" if hive == winreg.HKEY_CURRENT_USER else "InstallPath")
                        roots.append(os.path.normpath(v))
                except OSError:
                    pass
        except ImportError:
            pass
        for env in ("ProgramFiles(x86)", "ProgramFiles"):
            b = os.environ.get(env)
            if b:
                roots.append(os.path.join(b, "Steam"))
    # extra library folders listed by Steam
    extra = []
    for r in roots:
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        try:
            with open(vdf, "r", encoding="utf-8", errors="replace") as f:
                for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                    extra.append(os.path.normpath(m.group(1).replace("\\\\", "\\")))
        except OSError:
            pass
    seen, out = set(), []
    for r in roots + extra:
        key = os.path.normcase(r)
        if key not in seen:
            seen.add(key)
            out.append(r)
    return out


def find_vr_programs(extra_dirs=()):
    """Existing SteamVR + ALVR executables on this PC (full paths). Empty list if none are found."""
    found = []
    for root in list(_steam_roots()) + list(extra_dirs):
        base = os.path.join(root, "steamapps", "common", "SteamVR", "bin", "win64") if "SteamVR" not in root else root
        for exe in STEAMVR_EXES:
            p = os.path.join(base, exe)
            if os.path.isfile(p):
                found.append(os.path.normpath(p))
    from . import installer
    for p in installer.alvr_candidates():
        if os.path.isfile(p):
            found.append(os.path.normpath(p))
    seen, out = set(), []
    for p in found:
        if os.path.normcase(p) not in seen:
            seen.add(os.path.normcase(p))
            out.append(p)
    return out


# ---- apply / undo (with read-back) -----------------------------------------------------------

def _journal_path():
    return os.path.join(cfgmod.data_dir(), "gpu_changes.json")


def _load_journal():
    try:
        with open(_journal_path(), "r", encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def _save_journal(d):
    path = _journal_path()
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, indent=2)
    os.replace(tmp, path)


def current_levels(paths, store=None):
    store = store or default_store()
    return {p: pref_level(store.get(p)) for p in paths}


def force_high(paths, store=None):
    """Set 'High performance' for each exe. Returns list of dicts {path, ok, text}. Never guesses:
    every value is read back before it is reported as set. The previous value is journalled so
    undo() can restore it exactly (including 'there was no entry')."""
    store = store or default_store()
    journal = _load_journal()
    results = []
    for p in paths:
        p = os.path.normpath(p)
        if not os.path.isabs(p) or not p.lower().endswith(".exe"):
            results.append(dict(path=p, ok=False, text="Not a full path to a .exe file"))
            continue
        old = store.get(p)
        if p not in journal:                       # keep the ORIGINAL value across repeated clicks
            journal[p] = old
        want = with_gpu_pref(old, 2)
        try:
            _save_journal(journal)                 # journal first: a crash can then still be undone
            store.set(p, want)
        except (OSError, ValueError) as e:
            results.append(dict(path=p, ok=False, text="Windows refused the change: %s" % e))
            continue
        back = store.get(p)
        if pref_level(back) == 2:
            results.append(dict(path=p, ok=True, text="Set to High performance (read back OK)"))
        else:
            results.append(dict(path=p, ok=False, text="Written, but reading it back gave %r" % (back,)))
    return results


def undo(store=None):
    """Put every entry we touched back to what it was. Returns list of dicts."""
    store = store or default_store()
    journal = _load_journal()
    results = []
    for p, old in list(journal.items()):
        try:
            if old is None:
                store.delete(p)
            else:
                store.set(p, old)
        except (OSError, ValueError) as e:
            results.append(dict(path=p, ok=False, text="Could not restore: %s" % e))
            continue
        back = store.get(p)
        if back == old:
            results.append(dict(path=p, ok=True, text="Restored" if old is not None else "Removed (there was none before)"))
            del journal[p]
        else:
            results.append(dict(path=p, ok=False, text="Restore did not stick (now %r)" % (back,)))
    try:
        _save_journal(journal)
    except OSError:
        pass
    return results


def has_changes():
    return bool(_load_journal())


# ---- reading the layer's verdict -------------------------------------------------------------

def describe(status):
    """status = dict from installer.read_status(). Returns (ok, headline, detail)."""
    if not status:
        return None, VERDICT_TEXT["unknown"][1], ""
    v = status.get("gpu_verdict", "unknown")
    ok, text = VERDICT_TEXT.get(v, VERDICT_TEXT["unknown"])
    detail = status.get("gpu_hint", "")
    return ok, text, detail


# ---- Linux -----------------------------------------------------------------------------------

def linux_env(vendor="auto"):
    """Environment that sends one launch to the discrete GPU. NVIDIA vars per NVIDIA's PRIME
    render-offload README (Vulkan needs only __NV_PRIME_RENDER_OFFLOAD; GLX also needs the vendor
    library name). Mesa (AMD/Intel discrete): DRI_PRIME=1."""
    nv = {"__NV_PRIME_RENDER_OFFLOAD": "1", "__GLX_VENDOR_LIBRARY_NAME": "nvidia",
          "__VK_LAYER_NV_optimus": "NVIDIA_only"}
    mesa = {"DRI_PRIME": "1"}
    if vendor == "nvidia":
        return nv
    if vendor == "mesa":
        return mesa
    return dict(nv, **mesa)   # 'auto' = both sets; the shell script picks one by looking at lspci instead


def linux_script_path():
    from . import installer
    return os.path.join(installer.project_root(), "tools", "ovalffr-gpu-linux.sh")
