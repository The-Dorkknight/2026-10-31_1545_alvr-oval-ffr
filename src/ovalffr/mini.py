"""Mini fps window: a small always-on-top box with the live fps. Click it to open quick settings,
the A/B test, the key list and the fps-per-setting table from the log. Click the top line again to
make it small.

Settings buttons change config.ini directly (the layer picks it up within half a second, and they
work without a game running). Zones and the A/B test are sent to the running game as commands.
"""
from . import __version__
from . import config as cfgmod
from . import live


def open_mini(parent=None):
    import tkinter as tk
    from tkinter import ttk

    win = tk.Toplevel(parent) if parent is not None else tk.Tk()
    win.title("OvalFFR fps %s" % __version__)
    try:
        win.attributes("-topmost", True)
    except tk.TclError:
        pass
    win.resizable(False, False)

    state = {"open": False}
    top = tk.Frame(win, bg="#14202a", cursor="hand2")
    top.pack(fill="x")
    chip = tk.Label(top, text="  ", bg="#7a6a00", width=2)
    chip.pack(side="left", padx=(6, 6), pady=6)
    head = tk.StringVar(value="-- fps")
    head_lbl = tk.Label(top, textvariable=head, fg="white", bg="#14202a", font=("TkDefaultFont", 14, "bold"))
    head_lbl.pack(side="left", pady=4)
    hint = tk.Label(top, text="  click for more", fg="#9fb3c0", bg="#14202a", font=("TkDefaultFont", 8))
    hint.pack(side="left", padx=(4, 8))

    body = ttk.Frame(win, padding=8)
    details = tk.StringVar(value="")
    ttk.Label(body, textvariable=details, justify="left", wraplength=460).pack(anchor="w")

    feedback = tk.StringVar(value="")

    def set_cfg(fn, what):
        c = cfgmod.load()
        fn(c)
        cfgmod.save(c)
        feedback.set("Saved: %s. The game picks it up within a second." % what)

    def send(cmd):
        try:
            live.send_command(cmd)
        except (OSError, ValueError) as e:
            feedback.set("Could not send: %s" % e)
            return
        if live.live_status() is None:
            feedback.set("Sent, but no VR game is running right now: it is used if a game starts within 15 s.")
        else:
            feedback.set("Sent to the game (%s)." % live.COMMANDS[cmd][0])

    def size(d):
        def f(c):
            c.h = round((c.h + d) * 20) / 20.0
            c.v = round((c.v + d) * 20) / 20.0
        return f

    btns = ttk.Frame(body)
    btns.pack(anchor="w", pady=(8, 0))
    buttons = [
        ("FFR on/off", lambda: set_cfg(lambda c: setattr(c, "enabled", not c.enabled), "FFR on/off")),
        ("Sharp area -5%", lambda: set_cfg(size(-0.05), "sharp area smaller")),
        ("Sharp area +5%", lambda: set_cfg(size(0.05), "sharp area bigger")),
        ("Edge 2x2 / 4x4", lambda: set_cfg(lambda c: setattr(c, "outer", 1 if c.outer == 2 else 2), "edge strength")),
        ("Smooth on/off", lambda: set_cfg(lambda c: setattr(c, "smooth", not c.smooth), "smooth low-res edge")),
        ("Auto on/off", lambda: set_cfg(lambda c: setattr(c, "auto_fov", not c.auto_fov), "auto foveation")),
        ("Fps box in headset", lambda: set_cfg(lambda c: setattr(c, "overlay", not c.overlay), "fps box in headset")),
        ("Show zones in headset", lambda: send("zones")),
        ("Run A/B test (40 s)", lambda: send("abtest")),
    ]
    for i, (text, fn) in enumerate(buttons):
        ttk.Button(btns, text=text, command=fn).grid(row=i // 3, column=i % 3, sticky="ew", padx=2, pady=2)
    ttk.Label(body, textvariable=feedback, foreground="#555", wraplength=460).pack(anchor="w", pady=(4, 0))

    ttk.Label(body, text="Average fps per setting (2-minute averages from fps_log.csv)").pack(anchor="w", pady=(8, 2))
    table = ttk.Treeview(body, columns=("fps", "late", "time"), height=6)
    table.heading("#0", text="Setting")
    table.heading("fps", text="fps")
    table.heading("late", text="late")
    table.heading("time", text="measured")
    table.column("#0", width=300)
    for col, w in (("fps", 60), ("late", 60), ("time", 80)):
        table.column(col, width=w, anchor="e")
    table.pack(anchor="w", fill="x")
    table_note = tk.StringVar(value="")
    ttk.Label(body, textvariable=table_note, foreground="#555", wraplength=460).pack(anchor="w")

    keys = "Keys while playing (any window):\n" + "\n".join(
        "  %-11s %s" % (k, what) for k, what in (live.COMMANDS[c] for c in
                                                ("overlay", "zones", "ffr", "smaller", "bigger", "edge", "smooth", "auto", "abtest")))
    ttk.Label(body, text=keys, justify="left", font=("TkFixedFont", 8)).pack(anchor="w", pady=(8, 0))

    def toggle(_=None):
        state["open"] = not state["open"]
        if state["open"]:
            body.pack(fill="both", expand=True)
            hint.config(text="  click to make small")
            refresh_table()
        else:
            body.pack_forget()
            hint.config(text="  click for more")

    for w in (top, chip, head_lbl, hint):
        w.bind("<Button-1>", toggle)

    def refresh_table():
        for item in table.get_children():
            table.delete(item)
        app, groups = live.summarise(live.read_log())
        for g in groups:
            mins = g["seconds"] / 60.0
            table.insert("", "end", text=g["label"],
                         values=("%.1f" % g["fps"], "-" if g["missed"] is None else "%.0f%%" % g["missed"],
                                 "%.0f min" % mins if mins >= 1 else "%.0f s" % g["seconds"]))
        table_note.set(("Game: %s. Rows are written every 2 minutes per setting, so give each setting a couple of "
                        "minutes. If every setting shows the same fps and it equals the headset's refresh rate, the "
                        "game is already at full speed - raise the render resolution to see a difference." % app)
                       if groups else "No log yet: rows appear after a setting has run for 20 s to 2 minutes.")

    def refresh():
        try:
            st = live.live_status()
            h, lines = live.describe_live(st)
            head.set(h)
            details.set("\n".join(lines))
            if st is None:
                chip.config(bg="#5a6670")
            else:
                chip.config(bg="#2e8b57" if st.get("ffr_now", "1") == "1" else "#b03a48")
        except Exception as e:   # never let one bad read stop the window
            head.set("status error")
            details.set(str(e))
        win.after(1000, refresh)

    def refresh_table_loop():
        if state["open"]:
            try:
                refresh_table()
            except Exception as e:
                table_note.set("could not read the log: %s" % e)
        win.after(10000, refresh_table_loop)

    refresh()
    win.after(10000, refresh_table_loop)
    return win, toggle
