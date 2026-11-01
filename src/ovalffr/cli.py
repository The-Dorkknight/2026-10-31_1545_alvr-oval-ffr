"""Text mode: used when tkinter is missing, or with --cli. Same actions as the window."""
import os

from . import __version__, __build_date__
from . import config as cfgmod
from . import gpu as gpumod
from . import installer
from .mask import make_mask, estimated_saving


def print_status():
    res = installer.check()
    for r in res["rows"]:
        mark = {True: "[ OK ]", False: "[FAIL]", None: "[ .. ]"}[r["ok"]]
        print(mark, r["text"])


def show_config():
    c = cfgmod.load()
    m = make_mask(c, 2000, 2000, 50)
    print("enabled=%s  horizontal=%d%%  vertical=%d%%  shape=%.1f  ring=%d%%  outer=%dx%d" % (
        c.enabled, round(c.h * 100), round(c.v * 100), c.shape, round(c.band * 100), 1 << c.outer, 1 << c.outer))
    print("estimated pixel-shading saved: %.0f%% (shading only; real fps gain is smaller)" % (estimated_saving(m) * 100))
    print("auto foveation: %s (floor %d%% of the sliders)" % ("ON" if c.auto_fov else "off", round(c.auto_floor * 100)))
    print("smooth low-res edge: %s (strength %d%%)" % ("ON" if c.smooth else "off", round(c.smooth_strength * 100)))
    # tiny ASCII picture: '.' sharp, '+' middle, '#' low-res
    for row in make_mask(c, 2000, 2000, 100):
        print("".join("." if v == 0 else ("+" if v == 5 else "#") for v in row))


def set_values(pairs):
    c = cfgmod.load()
    for p in pairs:
        k, _, v = p.partition("=")
        if k in ("h", "v", "shape", "band"):
            try:
                x = float(v)
            except ValueError:
                print("ignored %r: not a number" % p)
                continue
            setattr(c, k, x / (100.0 if k in ("h", "v", "band") and x > 1.5 else 1.0))
        elif k == "outer":
            c.outer = 1 if v in ("1", "2x2") else 2
        elif k == "enabled":
            c.enabled = v in ("1", "true", "yes", "on")
        elif k == "auto":
            c.auto_fov = v in ("1", "true", "yes", "on")
        elif k == "auto_floor":
            try:
                x = float(v)
            except ValueError:
                print("ignored %r: not a number" % p)
                continue
            c.auto_floor = x / 100.0 if x > 1.5 else x
        elif k == "smooth":
            c.smooth = v in ("1", "true", "yes", "on")
        elif k == "overlay":
            c.overlay = v in ("1", "true", "yes", "on")
        elif k == "smooth_strength":
            try:
                x = float(v)
            except ValueError:
                print("ignored %r: not a number" % p)
                continue
            c.smooth_strength = x / 100.0 if x > 1.0 else x
        else:
            print("ignored %r: unknown setting" % p)
            continue
    print("saved", cfgmod.save(c))


def gpu_command(args):
    """gpu status | force [exe ...] | add EXE... | undo | linux"""
    sub = args[0] if args else "status"
    rest = args[1:]
    if sub == "linux" or not gpumod.IS_WIN and sub in ("force", "add", "undo"):
        print("On Linux nothing is written to the system. Start VR through the wrapper instead:")
        print("   " + gpumod.linux_script_path() + " <your ALVR / SteamVR command>")
        print("It sets the PRIME offload variables for that one launch only.")
        return 0
    if sub == "status":
        st = installer.read_status()
        ok, text, detail = gpumod.describe(st)
        print({True: "[ OK ]", False: "[FAIL]", None: "[ .. ]"}[ok], text)
        if detail:
            print("      " + detail)
        if not gpumod.IS_WIN:
            print("      On Linux: " + gpumod.linux_script_path() + " <command>   (see 'gpu linux')")
            return 0
        progs = gpumod.find_vr_programs()
        lv = gpumod.current_levels(progs)
        names = {None: "not set", 0: "Windows decides", 1: "power saving", 2: "HIGH PERFORMANCE"}
        for p in progs:
            print("   %-18s %s" % (names.get(lv[p], "other"), p))
        if not progs:
            print("   No SteamVR / ALVR programs found. Add one with: gpu add C:\\path\\to\\program.exe")
        return 0
    if sub in ("force", "add"):
        paths = [os.path.abspath(a) for a in rest] if sub == "add" else (
            [os.path.abspath(a) for a in rest] or gpumod.find_vr_programs())
        if not paths:
            print("Nothing to change: no SteamVR / ALVR program was found. Give the full path(s) to .exe file(s).")
            return 1
        bad = 0
        for r in gpumod.force_high(paths):
            print("[ OK ]" if r["ok"] else "[FAIL]", r["path"], "-", r["text"])
            bad += 0 if r["ok"] else 1
        print("Restart SteamVR / ALVR for this to take effect. Undo any time with: gpu undo")
        return 1 if bad else 0
    if sub == "undo":
        res = gpumod.undo()
        if not res:
            print("Nothing to undo: this tool has not changed any GPU preference.")
        for r in res:
            print("[ OK ]" if r["ok"] else "[FAIL]", r["path"], "-", r["text"])
        return 0
    print("usage: gpu status | force [exe ...] | add exe ... | undo | linux")
    return 2


def fps_command(args):
    from . import live
    if args and args[0] == "send":
        if len(args) < 2:
            print("usage: fps send <%s>" % "|".join(sorted(live.COMMANDS)))
            return 2
        try:
            live.send_command(args[1])
        except ValueError as e:
            print(e)
            return 2
        print("sent '%s' (%s); the running VR game picks it up within a second" % (args[1], live.COMMANDS[args[1]][1]))
        return 0
    head, lines = live.describe_live(live.live_status())
    print(head)
    for ln in lines:
        print("  " + ln)
    app, groups = live.summarise(live.read_log())
    print("\nAverage fps per setting%s (from %s):" % (" for '%s'" % app if app else "", live.log_path()))
    if not groups:
        print("  no rows yet (a row is written after a setting has run for 20 s to 2 minutes)")
    for g in groups:
        print("  %6.1f fps  %5s late  %6.0f s  %s" % (g["fps"], "-" if g["missed"] is None else "%.0f%%" % g["missed"],
                                                   g["seconds"], g["label"]))
    print("\nKeys while playing: " + ", ".join("%s %s" % (k, c) for c, (k, _) in live.COMMANDS.items()))
    return 0


def run(argv):
    if argv and argv[0] == "fps":
        return fps_command(argv[1:])
    if argv and argv[0] == "gpu":
        return gpu_command(argv[1:])
    if argv and argv[0] in ("status", "check"):
        return print_status() or 0
    if argv and argv[0] == "install":
        try:
            installer.install()
        except installer.InstallError as e:
            print("Install failed:", e)
            return 1
        print_status()
        return 0
    if argv and argv[0] == "uninstall":
        res = installer.uninstall()
        for e in res.get("errors", []):
            print("[FAIL]", e)
        print_status()
        return 1 if res.get("errors") else 0
    if argv and argv[0] == "show":
        show_config()
        return 0
    if argv and argv[0] == "set":
        set_values(argv[1:])
        return 0
    # interactive menu
    print("OvalFFR %s (built %s) - text mode" % (__version__, __build_date__))
    while True:
        print("\n1) Install   2) Check   3) Uninstall   4) Show/set sizes   q) Quit")
        try:
            choice = input("> ").strip().lower()
        except EOFError:
            return 0
        if choice == "1":
            run(["install"])
        elif choice == "2":
            print_status()
        elif choice == "3":
            run(["uninstall"])
        elif choice == "4":
            show_config()
            h = input("Horizontal % (blank = keep): ").strip()
            v = input("Vertical % (blank = keep): ").strip()
            args = (["h=" + h] if h else []) + (["v=" + v] if v else [])
            if args:
                set_values(args)
        elif choice in ("q", "quit", "exit"):
            return 0
