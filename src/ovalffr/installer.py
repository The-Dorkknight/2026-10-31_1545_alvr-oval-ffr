"""Install / check / uninstall the OpenXR API layer, then confirm it really loaded.

Windows : DLL + manifest in %LOCALAPPDATA%\\OvalFFR\\layer, registered under
          HKCU\\Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit (no admin needed).
Linux/Mac: .so/.dylib + manifest in ~/.local/share, manifest in
          ~/.local/share/openxr/1/api_layers/implicit.d/ (the loader's search path).

"Installed" is never reported on a guess: files are read back, the registry value is
read back, and the layer's own status.txt (written when an OpenXR app attaches it) says
whether a VR app actually loaded it.
"""
import glob
import json
import os
import shutil
import sys
import time

from . import config as cfgmod
from . import gpu as gpumod

LAYER_NAME = "XR_APILAYER_OVALFFR_oval_ffr"
MANIFEST_NAME = LAYER_NAME + ".json"
REG_KEY = r"Software\Khronos\OpenXR\1\ApiLayers\Implicit"

IS_WIN = sys.platform.startswith("win")
IS_MAC = sys.platform == "darwin"


def project_root():
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def lib_filename():
    if IS_WIN:
        return LAYER_NAME + ".dll"
    if IS_MAC:
        return "lib" + LAYER_NAME + ".dylib"
    return "lib" + LAYER_NAME + ".so"


def bundled_library():
    """Prebuilt layer shipped in the zip (prebuilt/<os>/). None if there is none."""
    sub = "windows-x64" if IS_WIN else ("macos" if IS_MAC else "linux-x64")
    p = os.path.join(project_root(), "prebuilt", sub, lib_filename())
    return p if os.path.isfile(p) else None


def _xdg_data():
    """The OpenXR loader searches $XDG_DATA_HOME (default ~/.local/share) for implicit layers."""
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def install_dir():
    if IS_WIN:
        return os.path.join(cfgmod.data_dir(), "layer")
    return os.path.join(_xdg_data(), "ovalffr", "layer")


def manifest_dir():
    if IS_WIN:
        return install_dir()
    return os.path.join(_xdg_data(), "openxr", "1", "api_layers", "implicit.d")


def manifest_path():
    return os.path.join(manifest_dir(), MANIFEST_NAME)


def installed_library():
    return os.path.join(install_dir(), lib_filename())


def make_manifest(library_path):
    return {
        "file_format_version": "1.0.0",
        "api_layer": {
            "name": LAYER_NAME,
            "library_path": library_path,
            "api_version": "1.0",
            "implementation_version": "1",
            "description": "OvalFFR: oval/slot fixed foveated rendering via variable rate shading",
            "disable_environment": "DISABLE_OVALFFR_LAYER",
        },
    }


# ---- registry (Windows only) ---------------------------------------------------

def _reg_has(path):
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_KEY, 0, winreg.KEY_READ) as k:
            val, _ = winreg.QueryValueEx(k, path)
            return int(val) == 0          # 0 = enabled, 1 = disabled
    except OSError:
        return False


def _reg_add(path):
    import winreg
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, REG_KEY, 0, winreg.KEY_SET_VALUE) as k:
        winreg.SetValueEx(k, path, 0, winreg.REG_DWORD, 0)


def _reg_remove(path):
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_KEY, 0, winreg.KEY_SET_VALUE) as k:
            winreg.DeleteValue(k, path)
    except OSError:
        pass


# ---- install / uninstall -------------------------------------------------------

class InstallError(Exception):
    pass


def install():
    src = bundled_library()
    if not src:
        raise InstallError(
            "No prebuilt layer for this system is in the zip (prebuilt/). "
            "Only Windows x64 has a GPU backend so far (Linux/Vulkan is planned, see docs/LINUX_VULKAN_PLAN.md); "
            "build one from layer/ with CMake.")
    dst = installed_library()
    try:
        os.makedirs(install_dir(), exist_ok=True)
        # Copy next to the target first, then swap: a half-copied DLL is never left behind.
        tmp = dst + ".new"
        shutil.copy2(src, tmp)
        os.replace(tmp, dst)
        os.makedirs(manifest_dir(), exist_ok=True)
        with open(manifest_path(), "w", encoding="utf-8", newline="\n") as f:
            json.dump(make_manifest(dst), f, indent=2)
        if IS_WIN:
            _reg_add(manifest_path())
    except PermissionError as e:
        raise InstallError(
            "Windows would not let the layer file be replaced (%s). It is probably in use: close SteamVR, "
            "the ALVR dashboard and any VR game, then press Install again." % e)
    except OSError as e:
        raise InstallError("Could not write the layer files: %s" % e)
    # seed a config so the layer and the UI agree from the first launch
    if not os.path.exists(cfgmod.config_path()):
        cfgmod.save(cfgmod.Config())
    return check()


def uninstall():
    """Returns the same dict as check(), plus 'errors': files that could not be removed."""
    errors = []
    if IS_WIN:
        _reg_remove(manifest_path())
    for p in (manifest_path(), installed_library()):
        try:
            os.remove(p)
        except FileNotFoundError:
            pass
        except OSError as e:
            errors.append("Could not remove %s: %s (is SteamVR or a VR game still running?)" % (p, e))
    res = check()
    res["errors"] = errors
    return res


# ---- check ---------------------------------------------------------------------

STATUS_KEEP_DAYS = 7


def _parse_status_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        mtime = os.path.getmtime(path)
    except OSError:
        return None
    d = {}
    for ln in lines:
        if "=" in ln:
            k, v = ln.split("=", 1)
            d[k] = v
    d["age_s"] = max(0.0, time.time() - mtime)
    d["file"] = path
    return d


def read_all_status():
    """Every VR program that loaded the layer recently, newest first. The layer writes one file per
    process (status-<pid>.txt) so an overlay app started after the game cannot hide the game's status.
    Files older than a week are deleted. A pre-0.2 status.txt is still understood."""
    ddir = cfgmod.data_dir(create=False)
    found = []
    for path in glob.glob(os.path.join(ddir, "status-*.txt")) + [os.path.join(ddir, "status.txt")]:
        d = _parse_status_file(path)
        if not d:
            continue
        if d["age_s"] > STATUS_KEEP_DAYS * 86400:
            try:
                os.remove(path)
            except OSError:
                pass
            continue
        found.append(d)
    found.sort(key=lambda d: d["age_s"])
    return found


def read_status():
    """The most relevant status: the newest one that is still running, else the newest of all."""
    allst = read_all_status()
    for d in allst:
        if d.get("ended") != "1":
            return d
    return allst[0] if allst else None


def check():
    """Return a dict of rows: each {ok: True/False/None, text: str}. Never guesses."""
    rows = []
    lib = installed_library()
    man = manifest_path()
    lib_ok = os.path.isfile(lib) and os.path.getsize(lib) > 0
    rows.append(dict(key="library", ok=lib_ok,
                     text=("Layer file present: " + lib) if lib_ok else "Layer file not installed"))

    man_ok = False
    if os.path.isfile(man):
        try:
            with open(man, "r", encoding="utf-8") as f:
                data = json.load(f)
            man_ok = (data["api_layer"]["name"] == LAYER_NAME and
                      os.path.isfile(data["api_layer"]["library_path"]))
        except (OSError, ValueError, KeyError):
            man_ok = False
    rows.append(dict(key="manifest", ok=man_ok,
                     text=("Manifest read back OK: " + man) if man_ok else "Manifest missing or points at a missing file"))

    if IS_WIN:
        reg_ok = _reg_has(man)
        rows.append(dict(key="registry", ok=reg_ok,
                         text="Registered with the OpenXR loader (HKCU, enabled)" if reg_ok
                         else "Not registered with the OpenXR loader"))
    installed = lib_ok and man_ok and (not IS_WIN or rows[-1]["ok"])

    st = read_status()
    if not installed:
        rows.append(dict(key="loaded", ok=None, text="Install first, then start a VR game"))
    elif not st:
        rows.append(dict(key="loaded", ok=None,
                         text="Installed, but no VR app has loaded it yet. Start a game through ALVR, then press Check."))
    else:
        age = st.get("age_s")
        when = ("%d min ago" % (age // 60)) if age is not None and age < 3600 else st.get("time", "earlier")
        closed = " - that program has since closed" if st.get("ended") == "1" else ""
        if st.get("state") == "active":
            rows.append(dict(key="loaded", ok=True,
                             text="Loaded by '%s' (%s%s). Foveation active: %s." %
                                  (st.get("app", "?"), when, closed, st.get("gpu", st.get("backend", "?")))))
        else:
            rows.append(dict(key="loaded", ok=False,
                             text="Loaded by '%s' (%s%s) but NOT active: %s" %
                                  (st.get("app", "?"), when, closed, st.get("reason", "unknown reason"))))
        if st.get("auto") == "1":
            try:
                pct = int(round(float(st.get("auto_scale", "1")) * 100))
            except ValueError:
                pct = 100
            names = {"warmup": "starting up", "steady": "frames are on time, full size", "tightening": "shrinking because frames were missed",
                     "relaxing": "growing back", "at_floor": "at your floor and still missing frames",
                     "locked": "shrinking did not help (not GPU-bound?), paused", "off": "off"}
            rows.append(dict(key="auto", ok=None if pct == 100 else True,
                             text="Auto foveation: sharp area at %d%% of your sliders - %s" % (pct, names.get(st.get("auto_state"), st.get("auto_state", "?")))))
        if st.get("smooth") == "1":
            sst = st.get("smooth_state", "")
            sok = True if sst == "running" else (False if sst.startswith("unavailable") else None)
            words = {"running": "running", "off": "off", "waiting for the first frame": "waiting for the first frame"}
            rows.append(dict(key="smooth", ok=sok,
                             text="Smooth low-res edge: %s" % (words.get(sst) or sst or "no report from the layer yet")))
        live = st.get("ended") != "1" and st.get("age_s", 1e9) <= 10 and st.get("state") == "active"
        try:
            fps = float(st.get("fps", "-1"))
        except ValueError:
            fps = -1.0
        if live and fps >= 0:
            extra = ""
            try:
                vrs = float(st.get("vrs_seen", "-1"))
                if vrs >= 0:
                    extra = "; VRS check %.0f%%" % vrs
            except ValueError:
                pass
            rows.append(dict(key="live", ok=None, text="Live: %.0f fps at %s Hz, FFR %s, %s%% late frames%s" % (
                fps, st.get("hz", "?").split(".")[0], "on" if st.get("ffr_now", "1") == "1" else "off",
                st.get("missed_pct", "?").split(".")[0], extra)))
        if st.get("ab_result"):
            rows.append(dict(key="ab", ok=None, text="Last A/B test: " + st["ab_result"]))
        gok, gtext, gdetail = gpumod.describe(st)
        if st.get("gpu_verdict") not in (None, "", "unknown"):
            rows.append(dict(key="gpu", ok=gok, text=gtext + ((" " + gdetail) if gdetail else "")))
    return dict(installed=installed, rows=rows, status=st)


# ---- finding ALVR --------------------------------------------------------------

def alvr_candidates():
    home = os.path.expanduser("~")
    c = []
    if IS_WIN:
        for env in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
            b = os.environ.get(env)
            if b:
                c += [os.path.join(b, "ALVR", "ALVR Dashboard.exe"),
                      os.path.join(b, "ALVR", "alvr_dashboard.exe")]
    else:
        c += [os.path.join(home, "alvr_streamer_linux", "bin", "alvr_dashboard"),
              os.path.join(home, "Applications", "alvr_dashboard"),
              "/usr/bin/alvr_dashboard", "/opt/alvr/bin/alvr_dashboard"]
    return c


def find_alvr():
    for p in alvr_candidates():
        if os.path.isfile(p):
            return p
    return shutil.which("alvr_dashboard")
