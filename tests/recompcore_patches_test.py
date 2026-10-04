"""Runtime patch application must be idempotent and preserve unrelated edits."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("runtime_patches", ROOT / "scripts/apply_recompcore_patches.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RuntimePatchTest(unittest.TestCase):
    def test_clean_repeat_and_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            checkout = Path(tmp) / "repo"
            checkout.mkdir()
            def git(*args):
                return subprocess.check_output(["git", "-C", str(checkout), *args])
            git("init", "-q")
            git("config", "user.name", "Fixture")
            git("config", "user.email", "fixture@example.invalid")
            source = checkout / "source.txt"
            source.write_text("before\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD").decode().strip()
            source.write_text("after\n")
            delta = git("diff", "--no-ext-diff", "--no-renames", "--binary", "--full-index", "HEAD", "--")
            patch = Path(tmp) / "fix.patch"
            patch.write_bytes(delta)
            digest = hashlib.sha256(delta).hexdigest()
            git("checkout", "--", "source.txt")
            self.assertEqual(module.apply(checkout, base, patch, digest, True), "ready")
            self.assertEqual(source.read_text(), "before\n")
            self.assertEqual(module.apply(checkout, base, patch, digest), "applied")
            self.assertEqual(module.apply(checkout, base, patch, digest), "already applied")
            git("add", ".")  # A staged, exact delta is accepted too.
            self.assertEqual(module.apply(checkout, base, patch, digest), "already applied")
            source.write_text("personal edit\n")
            with self.assertRaises(ValueError):
                module.apply(checkout, base, patch, digest)
            self.assertEqual(source.read_text(), "personal edit\n")
            with self.assertRaises(ValueError):
                module.apply(checkout, "0" * 40, patch, digest)
            with self.assertRaises(ValueError):
                module.apply(checkout, base, patch, "0" * 64)


if __name__ == "__main__":
    unittest.main()
