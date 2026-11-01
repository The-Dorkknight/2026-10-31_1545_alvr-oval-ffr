import os, stat, subprocess, sys, shutil, tempfile, unittest
import _paths
ROOT = _paths.ROOT

class Launchers(unittest.TestCase):
    def test_files_and_modes(self):
        for n in ("OvalFFR-Linux.sh", "OvalFFR-Mac.command"):
            p = os.path.join(ROOT, n)
            self.assertTrue(os.path.isfile(p), n)
            self.assertTrue(os.stat(p).st_mode & stat.S_IXUSR, n + " must be executable")
            with open(p, "rb") as f:
                data = f.read()
            self.assertTrue(data.startswith(b"#!/bin/sh"))
            self.assertNotIn(b"\r\n", data, n + " must use LF")
        with open(os.path.join(ROOT, "OvalFFR-Windows.bat"), "rb") as f:
            bat = f.read()
        self.assertIn(b"\r\n", bat); self.assertNotIn(b"\n", bat.replace(b"\r\n", b""))

    def test_gitattributes(self):
        with open(os.path.join(ROOT, ".gitattributes")) as f:
            t = f.read()
        for line in ("*.sh text eol=lf", "*.command text eol=lf", "*.bat text eol=crlf"):
            self.assertIn(line, t)

    def test_linux_launcher_really_starts_the_app(self):
        if sys.platform.startswith("win") or not shutil.which("xvfb-run"):
            self.skipTest("needs Linux + xvfb-run")
        env = dict(os.environ, OVALFFR_DIR=tempfile.mkdtemp())
        r = subprocess.run(["xvfb-run", "-a", os.path.join(ROOT, "OvalFFR-Linux.sh"), "--selftest"],
                           capture_output=True, text=True, env=env, timeout=60)
        if "tkinter" in (r.stdout + r.stderr) and r.returncode != 0 and "selftest" not in r.stdout:
            self.skipTest("no Python with tkinter on this machine")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("saved config matches sliders", r.stdout)

if __name__ == "__main__":
    unittest.main()
