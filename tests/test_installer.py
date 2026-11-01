"""Real install/check/uninstall round trip in a throw-away HOME (Linux/Mac only)."""
import json, os, sys, tempfile, unittest
import _paths

@unittest.skipIf(sys.platform.startswith("win"), "Windows registry path is not tested here")
class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp()
        self.data = tempfile.mkdtemp()
        os.environ["HOME"] = self.home
        os.environ["OVALFFR_DIR"] = self.data
        from ovalffr import installer
        self.inst = installer

    def test_manifest_shape(self):
        m = self.inst.make_manifest("/x/lib.so")["api_layer"]
        self.assertEqual(m["name"], "XR_APILAYER_OVALFFR_oval_ffr")
        self.assertIn("disable_environment", m)       # required for implicit layers
        self.assertTrue(os.path.isabs(m["library_path"]))

    def test_install_check_uninstall(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        before = self.inst.check()
        self.assertFalse(before["installed"])
        after = self.inst.install()
        self.assertTrue(after["installed"], after["rows"])
        with open(self.inst.manifest_path()) as f:
            data = json.load(f)
        self.assertTrue(os.path.isfile(data["api_layer"]["library_path"]))
        self.assertTrue(self.inst.manifest_path().startswith(self.home))   # never touches the real HOME
        # no VR app has run yet, so "loaded" must be unknown, not green
        self.assertIsNone([r for r in after["rows"] if r["key"] == "loaded"][0]["ok"])
        # simulate the layer reporting in
        with open(os.path.join(self.data, "status.txt"), "w") as f:
            f.write("time=2026-10-31 16:00:00\napp=TestGame\nversion=0.3.0\nstate=active\nbackend=d3d12-vrs\ngpu=vrs-tier2 tile=16\n")
        loaded = [r for r in self.inst.check()["rows"] if r["key"] == "loaded"][0]
        self.assertTrue(loaded["ok"]); self.assertIn("TestGame", loaded["text"])
        with open(os.path.join(self.data, "status.txt"), "w") as f:
            f.write("time=2026-10-31 16:00:00\napp=TestGame\nstate=passthrough\nreason=no vrs\n")
        notact = [r for r in self.inst.check()["rows"] if r["key"] == "loaded"][0]
        self.assertFalse(notact["ok"]); self.assertIn("no vrs", notact["text"])
        gone = self.inst.uninstall()
        self.assertFalse(gone["installed"])
        self.assertEqual(gone["errors"], [])
        self.assertFalse(os.path.exists(self.inst.manifest_path()))

    def _put_status(self, name, text, age_s=0):
        p = os.path.join(self.data, name)
        with open(p, "w") as f:
            f.write(text)
        if age_s:
            t = __import__("time").time() - age_s
            os.utime(p, (t, t))

    def test_per_process_status_files(self):
        # a game, then an overlay app that loaded the layer later: the game must stay visible
        self._put_status("status-100.txt", "app=Game\nstate=active\nbackend=d3d12-vrs\ngpu=vrs-tier2\n", age_s=30)
        self._put_status("status-200.txt", "app=Overlay\nstate=passthrough\nreason=app does not use D3D12\nended=1\n")
        st = self.inst.read_status()
        self.assertEqual(st["app"], "Game")                      # ended overlay is skipped even though newer
        self.assertEqual(len(self.inst.read_all_status()), 2)
        # when everything has ended the newest one is still reported
        self._put_status("status-100.txt", "app=Game\nstate=active\nended=1\n", age_s=30)
        self.assertEqual(self.inst.read_status()["app"], "Overlay")

    def test_old_status_files_are_pruned_and_legacy_file_still_read(self):
        self._put_status("status-1.txt", "app=Old\nstate=active\n", age_s=9 * 86400)
        self._put_status("status.txt", "app=Legacy\nstate=active\n")
        apps = [d["app"] for d in self.inst.read_all_status()]
        self.assertEqual(apps, ["Legacy"])
        self.assertFalse(os.path.exists(os.path.join(self.data, "status-1.txt")))

    def test_gpu_row_only_when_the_layer_reported_a_verdict(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        self.inst.install()
        self._put_status("status-5.txt", "app=G\nstate=active\ngpu=x\ngpu_verdict=runtime_on_weak\n"
                                         "gpu_hint=SteamVR is on Intel; your fastest card is NVIDIA\n")
        rows = {r["key"]: r for r in self.inst.check()["rows"]}
        self.assertFalse(rows["gpu"]["ok"]); self.assertIn("NVIDIA", rows["gpu"]["text"])
        self._put_status("status-5.txt", "app=G\nstate=active\ngpu=x\n")
        self.assertNotIn("gpu", {r["key"] for r in self.inst.check()["rows"]})

    def test_auto_foveation_row(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        self.inst.install()
        self._put_status("status-7.txt", "app=G\nstate=active\ngpu=x\nauto=1\nauto_scale=0.650\nauto_state=tightening\n")
        row = {r["key"]: r for r in self.inst.check()["rows"]}["auto"]
        self.assertIn("65%", row["text"]); self.assertIn("missed", row["text"])
        self._put_status("status-7.txt", "app=G\nstate=active\ngpu=x\nauto=1\nauto_scale=oops\nauto_state=weird\n")
        self.assertIn("100%", {r["key"]: r for r in self.inst.check()["rows"]}["auto"]["text"])   # junk does not crash
        self._put_status("status-7.txt", "app=G\nstate=active\ngpu=x\nauto=0\n")
        self.assertNotIn("auto", {r["key"] for r in self.inst.check()["rows"]})

    def test_smooth_row(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        self.inst.install()
        rows = lambda: {r["key"]: r for r in self.inst.check()["rows"]}
        self._put_status("status-8.txt", "app=G\nstate=active\ngpu=x\nsmooth=0\nsmooth_state=off\n")
        self.assertNotIn("smooth", rows())                                      # off: no row at all
        self._put_status("status-8.txt", "app=G\nstate=active\ngpu=x\nsmooth=1\nsmooth_state=running\n")
        self.assertTrue(rows()["smooth"]["ok"]); self.assertIn("running", rows()["smooth"]["text"])
        self._put_status("status-8.txt", "app=G\nstate=active\ngpu=x\nsmooth=1\nsmooth_state=unavailable: d3dcompiler_47.dll is missing\n")
        self.assertFalse(rows()["smooth"]["ok"]); self.assertIn("d3dcompiler", rows()["smooth"]["text"])
        self._put_status("status-8.txt", "app=G\nstate=active\ngpu=x\nsmooth=1\n")   # older layer: no state line
        self.assertIsNone(rows()["smooth"]["ok"])

    def test_live_and_ab_rows(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        self.inst.install()
        rows = lambda: {r["key"]: r for r in self.inst.check()["rows"]}
        self._put_status("status-9.txt", "app=G\nstate=active\ngpu=x\nfps=71.4\nhz=90.0\nmissed_pct=3.0\nvrs_seen=93.0\nffr_now=1\n"
                         "ab_result=FFR off 64.1 fps, on 71.9 fps (+12.2%)\n")
        r = rows()
        self.assertIn("71 fps at 90 Hz", r["live"]["text"]); self.assertIn("VRS check 93%", r["live"]["text"])
        self.assertIn("+12.2%", r["ab"]["text"])
        self._put_status("status-9.txt", "app=G\nstate=active\ngpu=x\nfps=71.4\nhz=90.0\n", age_s=60)
        self.assertNotIn("live", rows())                                         # not running now
        self._put_status("status-9.txt", "app=G\nstate=active\ngpu=x\nfps=bad\n")
        self.assertNotIn("live", rows())                                         # junk does not crash

    def test_xdg_data_home_is_honoured(self):
        alt = tempfile.mkdtemp()
        os.environ["XDG_DATA_HOME"] = alt
        try:
            self.assertTrue(self.inst.manifest_path().startswith(alt))
            self.assertTrue(self.inst.install_dir().startswith(alt))
        finally:
            del os.environ["XDG_DATA_HOME"]

    def test_install_reports_a_locked_file_in_plain_words(self):
        if not self.inst.bundled_library():
            self.skipTest("no prebuilt layer for this OS in prebuilt/")
        real = os.replace
        def locked(*a, **k):
            raise PermissionError(13, "Permission denied")
        os.replace = locked
        try:
            with self.assertRaises(self.inst.InstallError) as cm:
                self.inst.install()
            self.assertIn("close SteamVR", str(cm.exception))
        finally:
            os.replace = real

if __name__ == "__main__":
    unittest.main()
