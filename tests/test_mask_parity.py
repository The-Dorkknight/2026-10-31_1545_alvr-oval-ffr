"""The picture in the UI must match the mask the C++ layer builds."""
import subprocess, unittest
import _paths
from ovalffr.config import Config
from ovalffr.mask import make_mask, estimated_saving

CASES = [
    dict(h=0.70, v=0.70, shape=2.0, band=0.50, outer=2, w=1600, hh=1600, tile=16, cx=0.5, cy=0.5, ml=2),
    dict(h=0.55, v=0.40, shape=3.2, band=0.30, outer=2, w=2064, hh=2208, tile=16, cx=0.62, cy=0.48, ml=2),
    dict(h=0.90, v=0.35, shape=4.0, band=1.00, outer=1, w=1832, hh=1920, tile=8, cx=0.38, cy=0.5, ml=2),
    dict(h=0.40, v=0.40, shape=2.0, band=0.00, outer=2, w=1000, hh=600, tile=32, cx=0.5, cy=0.5, ml=1),
    # edge cases: tile size 0 (both sides use 8) and a NaN centre (both sides use the middle)
    dict(h=0.70, v=0.70, shape=2.0, band=0.50, outer=2, w=640, hh=480, tile=0, cx=0.5, cy=0.5, ml=2),
    dict(h=0.70, v=0.60, shape=2.0, band=0.50, outer=2, w=640, hh=480, tile=16, cx="nan", cy="nan", ml=2),
]

class Parity(unittest.TestCase):
    def test_cases(self):
        tool = _paths.find_tool("mask_dump")
        if not tool:
            self.skipTest("mask_dump not built (cmake --build layer first)")
        for k in CASES:
            out = subprocess.run([tool, str(k["h"]), str(k["v"]), str(k["shape"]), str(k["band"]), str(k["outer"]),
                                  str(k["w"]), str(k["hh"]), str(k["tile"]), str(k["cx"]), str(k["cy"]), str(k["ml"])],
                                 capture_output=True, text=True, check=True).stdout.splitlines()
            tw, th, saving = out[0].split()
            tw, th, saving = int(tw), int(th), float(saving)
            cpp = [[int(x) for x in ln.split()] for ln in out[1:]]
            py = make_mask(Config(h=k["h"], v=k["v"], shape=k["shape"], band=k["band"], outer=k["outer"]),
                           k["w"], k["hh"], k["tile"], float(k["cx"]), float(k["cy"]), k["ml"])
            self.assertEqual((len(py[0]), len(py)), (tw, th))
            diff = sum(1 for y in range(th) for x in range(tw) if cpp[y][x] != py[y][x])
            # float32 (C++) vs float64 (Python) can disagree on a tile whose centre sits exactly on a boundary
            self.assertLessEqual(diff / float(tw * th), 0.002, "mask mismatch in %r: %d tiles" % (k, diff))
            self.assertAlmostEqual(saving, estimated_saving(py), delta=0.005)

if __name__ == "__main__":
    unittest.main()
