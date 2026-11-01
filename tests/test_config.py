import os, tempfile, unittest
import _paths
from ovalffr import config as cfg

class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.environ["OVALFFR_DIR"] = self.d

    def test_round_trip_and_atomic_file(self):
        c = cfg.Config(h=0.55, v=0.45, shape=3.0, band=0.2, outer=1, enabled=False, centre_on_lens=False)
        path = cfg.save(c)
        self.assertTrue(os.path.isfile(path))
        self.assertFalse(os.path.exists(path + ".tmp"))
        r = cfg.load()
        self.assertEqual((r.h, r.v, r.shape, r.band, r.outer, r.enabled, r.centre_on_lens),
                         (0.55, 0.45, 3.0, 0.2, 1, False, False))

    def test_clamping_and_garbage(self):
        r = cfg.parse("h=9\nv=-4\nshape=abc\nouter=7\nband=x\nnonsense\n=\n")
        self.assertEqual((r.h, r.v, r.outer), (1.0, 0.20, 2))
        self.assertEqual(r.shape, 2.0)

    def test_nan_and_inf_fall_back_to_defaults_like_the_layer(self):
        r = cfg.parse("h=nan\nv=inf\nshape=-inf\nband=nan\n")
        self.assertEqual((r.h, r.v, r.shape, r.band), (0.70, 0.70, 2.0, 0.50))
        # a dataclass built by hand with NaN is cleaned the same way
        c = cfg.Config(h=float("nan"), v=float("inf"), outer=10**30).clamped()
        self.assertEqual((c.h, c.v, c.outer), (0.70, 0.70, 2))

    def test_bom_crlf_and_binary_junk_do_not_crash(self):
        self.assertEqual(cfg.parse("\ufeffh=0.5\r\nv=0.4\r\n").h, 0.5)
        with open(cfg.config_path(), "wb") as f:
            f.write(b"h=0.6\n\xff\xfe\x00garbage\n")
        self.assertEqual(cfg.load().h, 0.6)        # used to raise UnicodeDecodeError

    def test_auto_foveation_keys(self):
        c = cfg.Config(auto_fov=True, auto_floor=0.5)
        cfg.save(c)
        r = cfg.load()
        self.assertTrue(r.auto_fov); self.assertEqual(r.auto_floor, 0.5)
        self.assertIn("auto=0\nauto_floor=0.600\n", cfg.to_text(cfg.Config()))   # same text the C++ side writes
        self.assertEqual(cfg.parse("auto_floor=0.1").auto_floor, 0.40)           # clamped
        self.assertEqual(cfg.parse("auto_floor=9").auto_floor, 1.0)
        self.assertEqual(cfg.parse("auto_floor=nan").auto_floor, 0.60)           # NaN -> default, like the layer
        self.assertFalse(cfg.parse("auto=abc").auto_fov)
        self.assertFalse(cfg.Config().auto_fov)                                  # off unless asked for

    def test_smooth_periphery_keys(self):
        self.assertFalse(cfg.Config().smooth)                                    # off unless asked for
        self.assertIn("smooth=0\nsmooth_strength=0.600\n", cfg.to_text(cfg.Config()))   # same text as the C++ side
        cfg.save(cfg.Config(smooth=True, smooth_strength=0.35))
        r = cfg.load()
        self.assertTrue(r.smooth); self.assertAlmostEqual(r.smooth_strength, 0.35)
        self.assertEqual(cfg.parse("smooth_strength=7").smooth_strength, 1.0)
        self.assertEqual(cfg.parse("smooth_strength=-2").smooth_strength, 0.0)
        self.assertEqual(cfg.parse("smooth_strength=nan").smooth_strength, 0.60)
        self.assertFalse(cfg.parse("smooth=maybe").smooth)

    def test_missing_file_gives_defaults(self):
        self.assertEqual(cfg.load().h, 0.70)

if __name__ == "__main__":
    unittest.main()
