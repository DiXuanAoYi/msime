"""Source-builder failure/rollback tests. All writes stay in disposable fixtures."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from dictionary_build import build, inputs


class BuilderFailureTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        with tempfile.TemporaryDirectory(prefix="msime-source-builder-") as directory:
            root = Path(directory)
            old = root / "out"
            old.mkdir()
            (old / "valid-product").write_text("previous", encoding="utf8")
            staging = root / "staging"
            staging.mkdir()
            (staging / "valid-product").write_text("new", encoding="utf8")
            (staging / "mozc_dictionary_oss_README.txt").write_text("notice", encoding="utf8")
            with patch.object(build, "DICT_ROOT", root), patch.object(build, "OUT", old), \
                 patch.object(build, "NOTICE", root / "source/notice.txt"):
                yield root, old, staging

    def test_success_keeps_previous_product_and_notice(self):
        with self.fixture() as (root, old, staging):
            build.install(staging)
            self.assertEqual((old / "valid-product").read_text(), "new")
            previous = list(root.glob("out.previous-*"))
            self.assertEqual(len(previous), 1)
            self.assertEqual((previous[0] / "valid-product").read_text(), "previous")
            self.assertEqual(build.NOTICE.read_text(), "notice")

    def test_failed_final_rename_restores_previous_product(self):
        with self.fixture() as (_, old, staging):
            rename = Path.rename
            def fail_incoming(path, target):
                if path.parent.name.startswith("validated-"):
                    raise PermissionError("fixture: locked destination")
                return rename(path, target)
            with patch.object(Path, "rename", fail_incoming):
                with self.assertRaises(PermissionError):
                    build.install(staging)
            self.assertEqual((old / "valid-product").read_text(), "previous")

    def test_failed_copy_never_moves_previous_product(self):
        with self.fixture() as (_, old, staging):
            with patch.object(build.shutil, "copytree", side_effect=OSError("fixture: full disk")):
                with self.assertRaises(OSError):
                    build.install(staging)
            self.assertEqual((old / "valid-product").read_text(), "previous")

    def test_root_cannot_be_output(self):
        with self.fixture() as (root, old, staging), patch.object(build, "OUT", root):
            with self.assertRaises(ValueError):
                build.install(staging)
            self.assertEqual((old / "valid-product").read_text(), "previous")

    def test_tampered_output_is_not_accepted_as_cached_success(self):
        with self.fixture() as (root, old, _):
            build.NOTICE.parent.mkdir()
            build.NOTICE.write_text("notice")
            digest = hashlib.sha256(b"previous").hexdigest()
            (old / build.MARKER).write_text(json.dumps({"files": {"valid-product": digest}}))
            build.verify_out()
            (old / "valid-product").write_text("tampered")
            with self.assertRaises(ValueError):
                build.verify_out()

    def test_dirty_source_forces_rebuild(self):
        with self.fixture() as (_, old, _):
            build.NOTICE.parent.mkdir()
            build.NOTICE.write_text("notice")
            (old / build.MARKER).write_text(json.dumps({"files": {}, "custom_dictionary_commit": "fixed", "builder": build.builder_digest()}))
            self.assertIsNone(build.stale_reason("fixed", False))
            self.assertIsNotNone(build.stale_reason("fixed", True))

    def test_bad_external_bytes_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "fixed-input"
            target.write_bytes(b"bad")
            with patch.object(inputs, "download", lambda url, path: path.write_bytes(b"bad")):
                with self.assertRaises(ValueError):
                    inputs._ensure(target, "https://example.invalid/fixture", 4, hashlib.sha256(b"good").hexdigest(), "fixture")


if __name__ == "__main__":
    unittest.main()
