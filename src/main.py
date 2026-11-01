#!/usr/bin/env python3
"""OvalFFR - oval/slot fixed foveated rendering for OpenXR VR (ALVR friendly).

Window: horizontal + vertical sliders with a live picture of the low-res area,
plus install / check / uninstall of the OpenXR layer.
"""
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ovalffr import __version__, __build_date__, __app_name__   # noqa: E402
from ovalffr import config as cfgmod                           # noqa: E402
from ovalffr import installer                                  # noqa: E402
from ovalffr import gpu as gpumod                              # noqa: E402
from ovalffr.mask import make_mask, estimated_saving           # noqa: E402

# import errcode; errcode.install(app="OvalFFR")   # optional ErrCode hook (see skill notes)

GRID = 40            # preview resolution in tiles
CELL = 8             # pixels per preview tile
VIEW = GRID * CELL   # 320 px square preview of one eye
COL_SHARP, COL_MID, COL_LOW = "#dfeee8", "#f2c14e", "#d1495b"
COL_BG, COL_EDGE = "#1b2a32", "#0b141a"


def title():
    return "%s %s (Built %s)" % (__app_name__, __version__, __build_date__)


def run_gui(selftest=False):
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog

    root = tk.Tk()
    root.title(title())
    root.minsize(740, 420)
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass

    cfg = cfgmod.load()
    v_enabled = tk.BooleanVar(value=cfg.enabled)
    v_h = tk.DoubleVar(value=round(cfg.h * 100))
    v_v = tk.DoubleVar(value=round(cfg.v * 100))
    v_shape = tk.DoubleVar(value=cfg.shape)
    v_band = tk.DoubleVar(value=round(cfg.band * 100))
    v_outer = tk.IntVar(value=cfg.outer)
    v_centre = tk.BooleanVar(value=cfg.centre_on_lens)
    v_auto = tk.BooleanVar(value=cfg.auto_fov)
    v_floor = tk.DoubleVar(value=round(cfg.auto_floor * 100))
    v_smooth = tk.BooleanVar(value=cfg.smooth)
    v_sstr = tk.DoubleVar(value=round(cfg.smooth_strength * 100))
    v_overlay = tk.BooleanVar(value=cfg.overlay)

    # Status bar first, so it stays visible however small the window is.
    status_bar = tk.StringVar(value="v%s - %s" % (__version__, __build_date__))
    ttk.Label(root, textvariable=status_bar, relief="sunken", anchor="w").pack(fill="x", side="bottom")

    # The panel scrolls when the screen is too short for it (many low-end laptops are 1366x768).
    holder = ttk.Frame(root)
    holder.pack(fill="both", expand=True)
    holder.columnconfigure(0, weight=1)
    holder.rowconfigure(0, weight=1)
    scroller = tk.Canvas(holder, highlightthickness=0, bd=0)
    vsb = ttk.Scrollbar(holder, orient="vertical", command=scroller.yview)
    scroller.configure(yscrollcommand=vsb.set)
    scroller.grid(row=0, column=0, sticky="nsew")
    main = ttk.Frame(scroller, padding=12)
    main_win = scroller.create_window((0, 0), window=main, anchor="nw")
    main.columnconfigure(1, weight=1)

    def fit_scroll(_=None):
        scroller.itemconfigure(main_win, width=max(1, scroller.winfo_width()))
        scroller.configure(scrollregion=scroller.bbox("all"))
        if main.winfo_reqheight() > scroller.winfo_height() + 1:
            vsb.grid(row=0, column=1, sticky="ns")
        else:
            vsb.grid_remove()
            scroller.yview_moveto(0)

    def wheel(ev):
        if vsb.winfo_ismapped():
            step = -1 if (getattr(ev, "num", 0) == 4 or getattr(ev, "delta", 0) > 0) else 1
            scroller.yview_scroll(step * 2, "units")

    scroller.bind("<Configure>", fit_scroll)
    main.bind("<Configure>", fit_scroll)
    for ev in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
        root.bind_all(ev, wheel)

    # ---- preview -------------------------------------------------------------
    left = ttk.Frame(main)
    left.grid(row=0, column=0, sticky="n", padx=(0, 14))
    ttk.Label(left, text="One eye, as the GPU sees it", font=("TkDefaultFont", 10, "bold")).pack(anchor="w")
    canvas = tk.Canvas(left, width=VIEW + 2, height=VIEW + 2, bg=COL_EDGE, highlightthickness=0)
    canvas.pack(pady=(4, 6))
    cells = []
    for ty in range(GRID):
        for tx in range(GRID):
            cells.append(canvas.create_rectangle(1 + tx * CELL, 1 + ty * CELL, 1 + (tx + 1) * CELL,
                                                 1 + (ty + 1) * CELL, fill=COL_SHARP, outline=""))
    outline = canvas.create_line(0, 0, 0, 0, fill="#0b141a", width=2, smooth=False)
    canvas.create_line(1 + VIEW / 2 - 6, 1 + VIEW / 2, 1 + VIEW / 2 + 6, 1 + VIEW / 2, fill="#0b141a")
    canvas.create_line(1 + VIEW / 2, 1 + VIEW / 2 - 6, 1 + VIEW / 2, 1 + VIEW / 2 + 6, fill="#0b141a")
    legend = ttk.Frame(left)
    legend.pack(anchor="w")
    legend_labels = {}
    for col, key in ((COL_SHARP, "sharp"), (COL_MID, "mid"), (COL_LOW, "low")):
        f = ttk.Frame(legend)
        f.pack(anchor="w")
        tk.Label(f, bg=col, width=2, relief="solid", bd=1).pack(side="left", padx=(0, 6))
        legend_labels[key] = ttk.Label(f, text="")
        legend_labels[key].pack(side="left")
    saving_var = tk.StringVar()
    ttk.Label(left, textvariable=saving_var, font=("TkDefaultFont", 12, "bold")).pack(anchor="w", pady=(10, 0))
    ttk.Label(left, text="Estimate of pixel-shading work saved. Real fps gain is smaller\n"
                         "(geometry and memory work are not reduced).",
              foreground="#555").pack(anchor="w")

    # ---- controls ------------------------------------------------------------
    right = ttk.Frame(main)
    right.grid(row=0, column=1, sticky="new")
    right.columnconfigure(0, weight=1)

    ttk.Checkbutton(right, text="Foveated rendering on", variable=v_enabled,
                    command=lambda: changed()).grid(row=0, column=0, sticky="w", pady=(0, 8))

    def slider(row, label, var, lo, hi, fmt, note=""):
        head = ttk.Frame(right)
        head.grid(row=row, column=0, sticky="ew")
        ttk.Label(head, text=label).pack(side="left")
        val = ttk.Label(head, text=fmt(var.get()), width=10, anchor="e")
        val.pack(side="right")
        s = ttk.Scale(right, from_=lo, to=hi, variable=var, orient="horizontal",
                      command=lambda _=None: (val.config(text=fmt(var.get())), changed()))
        s.grid(row=row + 1, column=0, sticky="ew", pady=(0, 2))
        if note:
            ttk.Label(right, text=note, foreground="#555", wraplength=330, justify="left").grid(row=row + 2, column=0, sticky="w")
        return s, val

    s_h, _ = slider(1, "Horizontal: sharp area width", v_h, 20, 100, lambda x: "%d %%" % round(x),
                    "Full detail across this much of the view width.")
    s_v, _ = slider(4, "Vertical: sharp area height", v_v, 20, 100, lambda x: "%d %%" % round(x),
                    "Full detail across this much of the view height.")
    slider(7, "Shape", v_shape, 2.0, 4.5, lambda x: "oval" if x < 2.4 else ("slot" if x > 3.6 else "in between"),
           "Oval = lowest cost. Slot keeps more of the frame sharp (rounded rectangle).")
    slider(10, "Middle ring (2x2 shading)", v_band, 0, 100, lambda x: "%d %%" % round(x),
           "Gentle step between the sharp area and the low-res edge.")

    outer_box = ttk.Frame(right)
    outer_box.grid(row=13, column=0, sticky="w", pady=(8, 0))
    ttk.Label(outer_box, text="Low-res edge strength").pack(anchor="w")
    ttk.Radiobutton(outer_box, text="2x2 (gentle, works on every supported GPU)", variable=v_outer, value=1,
                    command=lambda: changed()).pack(anchor="w")
    ttk.Radiobutton(outer_box, text="4x4 (strong, needs a newer GPU)", variable=v_outer, value=2,
                    command=lambda: changed()).pack(anchor="w")
    ttk.Checkbutton(right, text="Centre on each lens (from the headset FOV)", variable=v_centre,
                    command=lambda: changed()).grid(row=14, column=0, sticky="w", pady=(6, 0))
    ttk.Checkbutton(right, text="Auto foveation (experimental)", variable=v_auto,
                    command=lambda: changed()).grid(row=15, column=0, sticky="w", pady=(6, 0))
    auto_box = ttk.Frame(right)
    auto_box.grid(row=16, column=0, sticky="ew")
    auto_box.columnconfigure(1, weight=1)
    ttk.Label(auto_box, text="   Never below").grid(row=0, column=0, sticky="w")
    floor_val = ttk.Label(auto_box, text="%d %%" % round(v_floor.get()), width=6, anchor="e")
    floor_val.grid(row=0, column=2, sticky="e")
    ttk.Scale(auto_box, from_=40, to=100, variable=v_floor, orient="horizontal",
              command=lambda _=None: (floor_val.config(text="%d %%" % round(v_floor.get())), changed())
              ).grid(row=0, column=1, sticky="ew", padx=6)
    ttk.Label(right, text="Shrinks the sharp area only while frames are being missed. Your sliders become the sharpest it will go.",
              foreground="#555", wraplength=330, justify="left").grid(row=17, column=0, sticky="w")
    ttk.Checkbutton(right, text="Smooth low-res edge (experimental)", variable=v_smooth,
                    command=lambda: changed()).grid(row=18, column=0, sticky="w", pady=(6, 0))
    smooth_box = ttk.Frame(right)
    smooth_box.grid(row=19, column=0, sticky="ew")
    smooth_box.columnconfigure(1, weight=1)
    ttk.Label(smooth_box, text="   Strength").grid(row=0, column=0, sticky="w")
    sstr_val = ttk.Label(smooth_box, text="%d %%" % round(v_sstr.get()), width=6, anchor="e")
    sstr_val.grid(row=0, column=2, sticky="e")
    ttk.Scale(smooth_box, from_=0, to=100, variable=v_sstr, orient="horizontal",
              command=lambda _=None: (sstr_val.config(text="%d %%" % round(v_sstr.get())), changed())
              ).grid(row=0, column=1, sticky="ew", padx=6)
    ttk.Label(right, text="Blends the blocky low-res pixels into a smooth picture, so a smaller sharp area can look the same. Costs a little GPU time.",
              foreground="#555", wraplength=330, justify="left").grid(row=20, column=0, sticky="w")
    ttk.Checkbutton(right, text="Show fps in the headset", variable=v_overlay,
                    command=lambda: changed()).grid(row=21, column=0, sticky="w", pady=(6, 0))
    ttk.Label(right, text="While playing: Ctrl+Alt+O fps box, Z zones, F FFR on/off, [ ] sharp area, T A/B test. "
                          "All keys and the fps log: Mini fps window.",
              foreground="#555", wraplength=330, justify="left").grid(row=22, column=0, sticky="w")
    ttk.Button(right, text="Reset to defaults", command=lambda: reset()).grid(row=23, column=0, sticky="w", pady=(8, 0))

    # ---- install & check -----------------------------------------------------
    box = ttk.LabelFrame(main, text="OpenXR layer: install and check", padding=8)
    box.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(12, 0))
    box.columnconfigure(0, weight=1)
    rows_frame = ttk.Frame(box)
    rows_frame.grid(row=0, column=0, sticky="ew")
    row_labels = []
    btns = ttk.Frame(box)
    btns.grid(row=1, column=0, sticky="w", pady=(8, 0))

    # ---- laptops with two graphics cards -----------------------------------
    gbox = ttk.LabelFrame(main, text="Laptop with two graphics cards", padding=8)
    gbox.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(10, 0))
    gbox.columnconfigure(0, weight=1)
    gpu_row = ttk.Frame(gbox)
    gpu_row.grid(row=0, column=0, sticky="ew")
    gpu_chip = tk.Label(gpu_row, text=" .. ", fg="white", bg="#7a6a00", font=("TkDefaultFont", 8, "bold"))
    gpu_chip.pack(side="left", padx=(0, 8), anchor="n")
    gpu_text = tk.StringVar(value="")
    ttk.Label(gpu_row, textvariable=gpu_text, wraplength=640, justify="left").pack(side="left", fill="x")
    gbtns = ttk.Frame(gbox)
    gbtns.grid(row=1, column=0, sticky="w", pady=(8, 0))

    # ---- logic ---------------------------------------------------------------
    pending = {"id": None}

    def current():
        c = cfgmod.Config(enabled=bool(v_enabled.get()), h=v_h.get() / 100.0, v=v_v.get() / 100.0,
                          shape=float(v_shape.get()), band=v_band.get() / 100.0, outer=int(v_outer.get()),
                          centre_on_lens=bool(v_centre.get()), auto_fov=bool(v_auto.get()),
                          auto_floor=v_floor.get() / 100.0, smooth=bool(v_smooth.get()),
                          smooth_strength=v_sstr.get() / 100.0, overlay=bool(v_overlay.get()))
        return c.clamped()

    def draw():
        c = current()
        # Colour by zone (so the ring and the edge stay distinguishable even when both are 2x2),
        # but estimate the saving from the mask with the strength the user actually chose.
        zc = cfgmod.Config(**{**c.__dict__, "outer": 2})
        zones = make_mask(zc, 2000, 2000, 2000 // GRID, 0.5, 0.5, 2)
        m = make_mask(c, 2000, 2000, 2000 // GRID, 0.5, 0.5, 2)
        n = {0: 0, 5: 0, 10: 0}
        i = 0
        for row in zones:
            for code in row:
                color = COL_SHARP if code == 0 else (COL_MID if code == 5 else COL_LOW)
                if not c.enabled:
                    color = COL_SHARP
                canvas.itemconfigure(cells[i], fill=color)
                n[code] += 1
                i += 1
        # outline of the full-detail shape
        pts = []
        a, b = c.h / 2.0, c.v / 2.0
        for k in range(160):
            t = 2 * math.pi * k / 160
            ct, st = math.cos(t), math.sin(t)
            x = a * math.copysign(abs(ct) ** (2.0 / c.shape), ct)
            y = b * math.copysign(abs(st) ** (2.0 / c.shape), st)
            pts += [1 + (0.5 + x) * VIEW, 1 + (0.5 + y) * VIEW]
        pts += pts[:2]
        canvas.coords(outline, *pts)
        canvas.itemconfigure(outline, state="normal" if c.enabled else "hidden")
        total = float(sum(n.values())) or 1.0
        edge = "%dx%d" % (1 << c.outer, 1 << c.outer)
        legend_labels["sharp"].config(text="Full detail (1x1): %d %% of the view" % round(100 * n.get(0, 0) / total))
        legend_labels["mid"].config(text="Middle ring (2x2): %d %%" % round(100 * n.get(5, 0) / total))
        legend_labels["low"].config(text="Low-res edge (%s): %d %%" % (edge, round(100 * (n.get(10, 0)) / total)))
        saving_var.set("About %d %% less shading work" % round(100 * estimated_saving(m)) if c.enabled
                       else "Off: no change to rendering")

    known = {"text": cfgmod.to_text(cfg)}   # config text last loaded or saved by this window

    def flush():
        pending["id"] = None
        try:
            cfgmod.save(current())
            known["text"] = cfgmod.to_text(current())
            status_bar.set("v%s - %s    saved; the layer reloads it within a second" % (__version__, __build_date__))
        except OSError as e:
            status_bar.set("could not save config: %s" % e)

    def changed():
        draw()
        if pending["id"]:
            root.after_cancel(pending["id"])
        pending["id"] = root.after(150, flush)

    def apply_cfg(d):
        """Show a config in the controls without saving it (used for changes made elsewhere)."""
        v_enabled.set(d.enabled); v_h.set(round(d.h * 100)); v_v.set(round(d.v * 100))
        v_shape.set(d.shape); v_band.set(round(d.band * 100)); v_outer.set(d.outer); v_centre.set(d.centre_on_lens)
        v_auto.set(d.auto_fov); v_floor.set(round(d.auto_floor * 100)); floor_val.config(text="%d %%" % round(d.auto_floor * 100))
        v_smooth.set(d.smooth); v_sstr.set(round(d.smooth_strength * 100)); sstr_val.config(text="%d %%" % round(d.smooth_strength * 100))
        v_overlay.set(d.overlay)
        draw()

    def follow_config_file():
        """Ctrl+Alt keys in the game and the mini window change config.ini: show those changes here."""
        try:
            if not pending["id"]:
                with open(cfgmod.config_path(), "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
                if text and text != known["text"]:
                    known["text"] = text
                    apply_cfg(cfgmod.parse(text))
                    status_bar.set("v%s - %s    settings changed outside this window (keys or mini window)" % (__version__, __build_date__))
        except OSError:
            pass
        root.after(1000, follow_config_file)

    def reset():
        d = cfgmod.Config()
        v_enabled.set(d.enabled); v_h.set(round(d.h * 100)); v_v.set(round(d.v * 100))
        v_shape.set(d.shape); v_band.set(round(d.band * 100)); v_outer.set(d.outer); v_centre.set(d.centre_on_lens)
        v_auto.set(d.auto_fov); v_floor.set(round(d.auto_floor * 100)); floor_val.config(text="%d %%" % round(d.auto_floor * 100))
        v_smooth.set(d.smooth); v_sstr.set(round(d.smooth_strength * 100)); sstr_val.config(text="%d %%" % round(d.smooth_strength * 100))
        v_overlay.set(d.overlay)
        changed()

    shown = {"sig": None}

    def shorten(t, n=88):
        """Keep long file paths on one line: ...middle of the path removed."""
        if len(t) <= n:
            return t
        keep = (n - 3) // 2
        return t[:keep] + "..." + t[-keep:]

    def refresh_gpu(res):
        gr = [r for r in res["rows"] if r["key"] == "gpu"]
        if gr:
            ok, text = gr[0]["ok"], gr[0]["text"]
        else:
            ok, text = None, gpumod.VERDICT_TEXT["unknown"][1]
        gpu_chip.config(text={True: " OK ", False: " !! ", None: " .. "}[ok],
                        bg={True: "#1b7a43", False: "#b3261e", None: "#7a6a00"}[ok])
        gpu_text.set(text)

    def refresh_status(_=None):
        res = installer.check()
        refresh_gpu(res)
        sig = repr([(r["key"], r["ok"], r["text"]) for r in res["rows"]])
        if sig == shown["sig"]:
            return                      # nothing changed: do not rebuild the widgets (no flicker)
        shown["sig"] = sig
        for w in row_labels:
            w.destroy()
        del row_labels[:]
        for r in res["rows"]:
            if r["key"] == "gpu":
                continue                # shown in its own box below
            col = {True: "#1b7a43", False: "#b3261e", None: "#7a6a00"}[r["ok"]]
            sym = {True: "OK", False: "!!", None: ".."}[r["ok"]]
            f = ttk.Frame(rows_frame)
            f.pack(anchor="w", fill="x")
            tk.Label(f, text=" %s " % sym, fg="white", bg=col, font=("TkDefaultFont", 8, "bold")).pack(side="left", padx=(0, 8))
            ttk.Label(f, text=(shorten(r["text"]) if r["key"] in ("library", "manifest") else r["text"]), wraplength=640, justify="left").pack(side="left")
            row_labels.append(f)

    def do_install():
        try:
            installer.install()
        except installer.InstallError as e:
            messagebox.showerror(title(), str(e))
        except OSError as e:
            messagebox.showerror(title(), "Install failed: %s" % e)
        refresh_status()

    def do_uninstall():
        if messagebox.askyesno(title(), "Remove the OvalFFR layer from the OpenXR loader?"):
            res = installer.uninstall()
            if res.get("errors"):
                messagebox.showwarning(title(), "\n".join(res["errors"]))
            refresh_status()

    # ---- GPU actions ---------------------------------------------------------
    def do_force_gpu():
        progs = gpumod.find_vr_programs()
        if not progs:
            messagebox.showinfo(title(), "No SteamVR or ALVR program was found.\n"
                                         "Use 'Add program...' and pick vrserver.exe (SteamVR > bin > win64).")
            return
        names = "\n".join("  " + p for p in progs)
        if not messagebox.askyesno(title(), "Tell Windows to run these programs on the high-performance "
                                            "graphics card?\n\n%s\n\nThis is the same switch as Settings > "
                                            "System > Display > Graphics. 'Undo GPU changes' puts everything "
                                            "back. Restart SteamVR afterwards." % names):
            return
        show_gpu_results(gpumod.force_high(progs))

    def do_add_program():
        path = filedialog.askopenfilename(title="Pick a program to run on the gaming GPU (for example the game)",
                                          filetypes=[("Programs", "*.exe")])
        if path:
            show_gpu_results(gpumod.force_high([path]))

    def do_undo_gpu():
        if not gpumod.has_changes():
            messagebox.showinfo(title(), "Nothing to undo: this tool has not changed any GPU preference.")
            return
        show_gpu_results(gpumod.undo())

    def show_gpu_results(results):
        bad = [r for r in results if not r["ok"]]
        lines = ["%s  %s\n    %s" % ("OK " if r["ok"] else "FAIL", r["path"], r["text"]) for r in results]
        (messagebox.showwarning if bad else messagebox.showinfo)(title(), "\n".join(lines) or "Nothing changed.")
        status_bar.set("v%s - %s    GPU preference %s" % (__version__, __build_date__, "FAILED" if bad else "updated"))

    def copy_linux_cmd():
        cmd = gpumod.linux_script_path() + " alvr_dashboard"
        root.clipboard_clear()
        root.clipboard_append(cmd)
        status_bar.set("v%s - %s    copied: %s" % (__version__, __build_date__, cmd))

    def start_alvr():
        remembered = os.path.join(cfgmod.data_dir(), "alvr_path.txt")
        path = None
        try:
            with open(remembered, "r", encoding="utf-8") as f:
                path = f.read().strip()
        except OSError:
            pass
        if not path or not os.path.isfile(path):
            path = installer.find_alvr()
        if not path:
            path = filedialog.askopenfilename(title="Find the ALVR Dashboard program")
        if path:
            try:
                subprocess.Popen([path], cwd=os.path.dirname(path))
                with open(remembered, "w", encoding="utf-8") as f:
                    f.write(path)
            except OSError as e:
                messagebox.showerror(title(), "Could not start ALVR: %s" % e)

    def open_folder():
        d = cfgmod.data_dir()
        try:
            if sys.platform.startswith("win"):
                os.startfile(d)  # noqa
            elif sys.platform == "darwin":
                subprocess.Popen(["open", d])
            else:
                subprocess.Popen(["xdg-open", d])
        except OSError as e:
            messagebox.showinfo(title(), "Folder: %s\n(%s)" % (d, e))

    ttk.Button(btns, text="Install", command=do_install).pack(side="left", padx=(0, 6))
    ttk.Button(btns, text="Check", command=refresh_status).pack(side="left", padx=(0, 6))
    ttk.Button(btns, text="Uninstall", command=do_uninstall).pack(side="left", padx=(0, 6))
    ttk.Button(btns, text="Start ALVR", command=start_alvr).pack(side="left", padx=(0, 6))
    ttk.Button(btns, text="Open log folder", command=open_folder).pack(side="left", padx=(0, 6))

    def open_mini():
        from ovalffr import mini
        mini.open_mini(root)
    ttk.Button(btns, text="Mini fps window", command=open_mini).pack(side="left")

    if gpumod.IS_WIN:
        ttk.Button(gbtns, text="Use gaming GPU for SteamVR", command=do_force_gpu).pack(side="left", padx=(0, 6))
        ttk.Button(gbtns, text="Add program...", command=do_add_program).pack(side="left", padx=(0, 6))
        ttk.Button(gbtns, text="Undo GPU changes", command=do_undo_gpu).pack(side="left")
    else:
        ttk.Button(gbtns, text="Copy launch command", command=copy_linux_cmd).pack(side="left", padx=(0, 8))
        ttk.Label(gbtns, text="Start ALVR / SteamVR through tools/ovalffr-gpu-linux.sh to use the discrete GPU.").pack(side="left")

    draw()
    refresh_status()
    root.update_idletasks()
    scroller.configure(width=main.winfo_reqwidth(),
                       height=min(main.winfo_reqheight(), max(300, root.winfo_screenheight() - 110)))

    def poll():
        try:
            refresh_status()
        except Exception as e:   # one bad read must not stop the live status for good
            status_bar.set("v%s - %s    status refresh failed: %s" % (__version__, __build_date__, e))
        root.after(3000, poll)
    root.after(3000, poll)
    root.after(1000, follow_config_file)

    if selftest:
        v_h.set(50); v_v.set(40); v_shape.set(3.0); v_outer.set(1)
        root.update()
        changed()
        root.update()
        root.after_cancel(pending["id"])
        flush()
        c = cfgmod.load()
        ok = abs(c.h - 0.5) < 1e-3 and abs(c.v - 0.4) < 1e-3 and abs(c.shape - 3.0) < 1e-3 and c.outer == 1
        print("selftest:", "saved config matches sliders" if ok else "MISMATCH %r" % (c,))
        # a change made elsewhere (Ctrl+Alt key in the game, mini window) must show up in the panel
        cfgmod.save(cfgmod.Config(h=0.55, v=0.45, overlay=True, smooth=True))
        follow_config_file()
        root.update()
        ok2 = round(v_h.get()) == 55 and round(v_v.get()) == 45 and v_overlay.get() and v_smooth.get()
        print("selftest:", "panel follows outside changes" if ok2 else "panel did NOT follow outside change")
        ok = ok and ok2
        root.destroy()
        return 0 if ok else 1

    root.mainloop()
    return 0


def main(argv):
    if "--version" in argv:
        print("%s %s (built %s)" % (__app_name__, __version__, __build_date__))
        return 0
    if "--cli" in argv:
        from ovalffr import cli
        return cli.run([a for a in argv if a != "--cli"])
    try:
        import importlib
        importlib.import_module("tkinter")
    except ImportError:
        print("tkinter is not installed, so the window cannot open.")
        print("Fix: Linux  sudo apt install python3-tk   |   Mac  install Python from python.org")
        print("Starting text mode instead.\n")
        from ovalffr import cli
        return cli.run([])
    if "--mini" in argv:
        from ovalffr import mini
        win, _ = mini.open_mini()
        win.mainloop()
        return 0
    return run_gui(selftest="--selftest" in argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
