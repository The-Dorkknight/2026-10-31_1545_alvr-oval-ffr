"""Live status, commands to the game, and the fps-per-setting summary (v0.5.0)."""
import os
import tempfile
import time
import unittest

import _paths  # noqa: F401  (puts src/ on the path)

LOG = """start,seconds,app,ffr,h,v,shape,ring,edge,smooth,smooth_strength,auto,auto_scale,avg_fps,worst_fps,hz,missed_pct,spare_ms,vrs_seen,note
2026-11-01 20:00:00,120,Game A,0,0.700,0.700,2.0,0.500,4x4,0,0.60,0,1.000,60.0,55.0,90.0,30.0,0.50,,
2026-11-01 20:02:00,120,Game A,1,0.700,0.700,2.0,0.500,4x4,0,0.60,0,1.000,72.0,66.0,90.0,10.0,1.20,93.0,
2026-11-01 20:04:00,60,Game A,1,0.700,0.700,2.0,0.500,4x4,0,0.60,0,1.000,78.0,70.0,90.0,6.0,1.40,94.0,setting changed before 2 minutes
2026-11-01 20:05:00,30,Game A,1,0.650,0.650,2.0,0.500,4x4,1,0.60,0,1.000,80.0,75.0,90.0,,1.50,,A/B test: FFR on phases
this,is,a,broken,row
2026-11-01 20:06:00,120,Old Game,1,0.700,0.700,2.0,0.500,2x2,0,0.60,1,0.800,40.0,35.0,72.0,50.0,0.10,,
2026-11-01 20:08:00,120,Game A,0,0.700,0.700,2.0,0.500,4x4,0,0.60,0,1.000,62.0,58.0,90.0,28.0,0.60,,
"""


class LiveTests(unittest.TestCase):
    def setUp(self):
        self.data = tempfile.mkdtemp()
        os.environ["OVALFFR_DIR"] = self.data
        os.environ["HOME"] = tempfile.mkdtemp()
        from ovalffr import live, config
        self.live, self.cfg = live, config

    def test_send_command_writes_a_line_the_layer_understands(self):
        self.live.send_command("zones", now=1700000000)
        self.live.send_command("abtest", now=1700000001)
        with open(self.live.commands_path()) as f:
            self.assertEqual(f.read(), "1700000000 zones\n1700000001 abtest\n")   # same format the C++ side parses
        with self.assertRaises(ValueError):
            self.live.send_command("rm -rf")

    def test_every_command_has_a_key(self):
        for cmd, (key, what) in self.live.COMMANDS.items():
            self.assertTrue(key.startswith("Ctrl+Alt+"), cmd)
            self.assertTrue(what)

    def test_summary_weights_by_time_and_uses_the_newest_game(self):
        with open(self.live.log_path(), "w") as f:
            f.write(LOG)
        rows = self.live.read_log()
        self.assertEqual(len(rows), 6)                       # broken row skipped
        app, groups = self.live.summarise(rows)
        self.assertEqual(app, "Game A")
        by = {g["label"]: g for g in groups}
        self.assertAlmostEqual(by["FFR off"]["fps"], 61.0)    # (60*120 + 62*120) / 240
        on = by["FFR on 70% x 70%, edge 4x4"]
        self.assertAlmostEqual(on["fps"], (72 * 120 + 78 * 60) / 180.0)
        self.assertAlmostEqual(on["seconds"], 180)
        sm = by["FFR on 65% x 65%, edge 4x4, smooth 60%"]
        self.assertIsNone(sm["missed"])                      # A/B rows have no late-frame figure
        self.assertEqual(groups[0]["label"], sm["label"])     # best fps first
        self.assertNotIn("FFR on 70% x 70%, edge 2x2, auto", by)   # other game not mixed in
        app2, g2 = self.live.summarise(rows, app="Old Game")
        self.assertEqual(g2[0]["label"], "FFR on 70% x 70%, edge 2x2, auto")

    def test_missing_or_empty_log(self):
        self.assertEqual(self.live.read_log(), [])
        self.assertEqual(self.live.summarise([]), (None, []))

    def _status(self, text, age_s=0):
        p = os.path.join(self.data, "status-4242.txt")
        with open(p, "w") as f:
            f.write(text)
        t = time.time() - age_s
        os.utime(p, (t, t))

    def test_live_status_and_words(self):
        self.assertIsNone(self.live.live_status())
        head, lines = self.live.describe_live(None)
        self.assertIn("No VR game", head)
        self._status("app=G\nstate=active\nfps=71.4\nhz=90.0\nmissed_pct=3.0\nspare_ms=2.10\nvrs_seen=93.0\n"
                     "ffr_now=1\nsaving=0.490\nab=idle\nab_result=FFR off 64.1 fps, on 71.9 fps (+12.2%)\n")
        st = self.live.live_status()
        self.assertIsNotNone(st)
        head, lines = self.live.describe_live(st)
        self.assertEqual(head, "71 fps / 90 Hz  FFR on")
        text = "\n".join(lines)
        self.assertIn("93%", text); self.assertIn("working", text); self.assertIn("49%", text); self.assertIn("+12.2%", text)
        self._status("app=G\nstate=active\nfps=71\nhz=90\nvrs_seen=4.0\nffr_now=1\n")
        self.assertIn("NOT reaching", "\n".join(self.live.describe_live(self.live.live_status())[1]))
        self._status("app=G\nstate=active\nfps=71\nhz=90\nvrs_seen=-1.0\nffr_now=0\n")
        self.assertIn("FFR off", self.live.describe_live(self.live.live_status())[0])
        self._status("app=G\nstate=active\nfps=71\n", age_s=60)      # old file: game not running now
        self.assertIsNone(self.live.live_status())
        self._status("app=G\nstate=active\nfps=junk\nhz=\nmissed_pct=x\n")   # junk never crashes
        self.live.describe_live(self.live.live_status())

    def test_overlay_config_key(self):
        c = self.cfg.parse(self.cfg.to_text(self.cfg.Config(overlay=True)))
        self.assertTrue(c.overlay)
        self.assertFalse(self.cfg.Config().overlay)
        self.assertIn("overlay=0\n", self.cfg.to_text(self.cfg.Config()))


if __name__ == "__main__":
    unittest.main()
