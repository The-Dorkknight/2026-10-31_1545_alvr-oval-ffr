"""Dual-GPU hook: registry logic with an in-memory store (the real WinRegStore is Windows-only and
could not be run here), the undo journal, verdict text and the Linux environment."""
import json, os, tempfile, unittest
import _paths
from ovalffr import gpu

EXE = os.path.abspath(os.sep + os.path.join("steam", "vrserver.exe"))
if os.name != "nt":
    EXE = "/steam/vrserver.exe"


class PrefString(unittest.TestCase):
    def test_set_keeps_other_settings(self):
        self.assertEqual(gpu.with_gpu_pref(None, 2), "GpuPreference=2;")
        self.assertEqual(gpu.with_gpu_pref("SwapEffectUpgradeEnable=1;", 2), "SwapEffectUpgradeEnable=1;GpuPreference=2;")
        self.assertEqual(gpu.with_gpu_pref("GpuPreference=1;SwapEffectUpgradeEnable=1;", 2),
                         "SwapEffectUpgradeEnable=1;GpuPreference=2;")
        self.assertEqual(gpu.with_gpu_pref("  junk ;; =;GpuPreference = 0 ;", 2), "GpuPreference=2;")

    def test_level(self):
        self.assertEqual(gpu.pref_level("GpuPreference=2;"), 2)
        self.assertIsNone(gpu.pref_level(""))
        self.assertIsNone(gpu.pref_level(None))
        self.assertIsNone(gpu.pref_level("GpuPreference=abc;"))


class ForceAndUndo(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.environ["OVALFFR_DIR"] = self.d

    def test_force_reads_back_and_undo_restores_exactly(self):
        other = "/steam/vrcompositor.exe"
        store = gpu.MemoryStore({EXE: "GpuPreference=1;SwapEffectUpgradeEnable=1;"})
        res = gpu.force_high([EXE, other], store)
        self.assertTrue(all(r["ok"] for r in res), res)
        self.assertEqual(store.get(EXE), "SwapEffectUpgradeEnable=1;GpuPreference=2;")
        self.assertEqual(store.get(other), "GpuPreference=2;")
        self.assertTrue(gpu.has_changes())
        undo = gpu.undo(store)
        self.assertTrue(all(r["ok"] for r in undo), undo)
        self.assertEqual(store.get(EXE), "GpuPreference=1;SwapEffectUpgradeEnable=1;")   # exactly what it was
        self.assertIsNone(store.get(other))                                              # there was none before
        self.assertFalse(gpu.has_changes())

    def test_second_click_does_not_lose_the_original_value(self):
        store = gpu.MemoryStore({EXE: "GpuPreference=1;"})
        gpu.force_high([EXE], store)
        gpu.force_high([EXE], store)            # user presses the button twice
        gpu.undo(store)
        self.assertEqual(store.get(EXE), "GpuPreference=1;")

    def test_rejects_relative_and_non_exe_paths(self):
        store = gpu.MemoryStore()
        res = gpu.force_high(["vrserver.exe", "/steam/readme.txt"], store)
        self.assertFalse(any(r["ok"] for r in res))
        self.assertEqual(store.data, {})

    def test_a_store_that_ignores_writes_is_reported_not_trusted(self):
        class Stubborn(gpu.MemoryStore):
            def set(self, name, value):
                pass                           # Windows silently dropped it
        res = gpu.force_high([EXE], Stubborn())
        self.assertFalse(res[0]["ok"])
        self.assertIn("reading it back", res[0]["text"])

    def test_a_store_that_raises_is_reported(self):
        class Denied(gpu.MemoryStore):
            def set(self, name, value):
                raise PermissionError(5, "Access is denied")
        res = gpu.force_high([EXE], Denied())
        self.assertFalse(res[0]["ok"]); self.assertIn("refused", res[0]["text"])

    def test_damaged_journal_does_not_crash(self):
        with open(os.path.join(self.d, "gpu_changes.json"), "w") as f:
            f.write("{not json")
        self.assertEqual(gpu.undo(gpu.MemoryStore()), [])
        with open(os.path.join(self.d, "gpu_changes.json"), "w") as f:
            f.write("[1,2,3]")
        self.assertFalse(gpu.has_changes())

    def test_journal_is_written_before_the_registry_changes(self):
        seen = {}
        class Spy(gpu.MemoryStore):
            def set(inner, name, value):
                with open(os.path.join(self.d, "gpu_changes.json")) as f:
                    seen.update(json.load(f))
                super().set(name, value)
        gpu.force_high([EXE], Spy())
        self.assertIn(EXE, seen)


class Verdicts(unittest.TestCase):
    def test_describe(self):
        self.assertIsNone(gpu.describe(None)[0])
        self.assertTrue(gpu.describe({"gpu_verdict": "ok"})[0])
        ok, text, detail = gpu.describe({"gpu_verdict": "runtime_on_weak", "gpu_hint": "use NVIDIA"})
        self.assertFalse(ok); self.assertIn("SteamVR", text); self.assertEqual(detail, "use NVIDIA")
        self.assertIsNone(gpu.describe({"gpu_verdict": "something_new"})[0])   # unknown future value: stay neutral


class Linux(unittest.TestCase):
    def test_env(self):
        nv = gpu.linux_env("nvidia")
        self.assertEqual(nv["__NV_PRIME_RENDER_OFFLOAD"], "1")
        self.assertEqual(nv["__VK_LAYER_NV_optimus"], "NVIDIA_only")
        self.assertEqual(gpu.linux_env("mesa"), {"DRI_PRIME": "1"})

    def test_script_exists_and_is_executable(self):
        p = gpu.linux_script_path()
        self.assertTrue(os.path.isfile(p), p)
        if os.name != "nt":
            self.assertTrue(os.access(p, os.X_OK))


if __name__ == "__main__":
    unittest.main()
