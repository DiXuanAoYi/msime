import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("build_domain_input", ROOT / "scripts/build_domain_input.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DomainInputBuildTest(unittest.TestCase):
    def test_pinned_assets_counts_and_read_only_compatible_tables(self):
        source = ROOT / "assets/domain-input"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "domain-input.db"
            catalog = MODULE.build(source, output)
            self.assertEqual(len(catalog["books"]), 11)
            self.assertEqual(sum(x["entries"] for x in catalog["books"]), 132916)
            with sqlite3.connect(f"file:{output.as_posix()}?mode=ro", uri=True) as db:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM entries").fetchone()[0], 132916)
                for book in catalog["books"]:
                    self.assertEqual(db.execute("SELECT COUNT(*) FROM entries WHERE pack=?", (book["id"],)).fetchone()[0], book["entries"])
                self.assertEqual(db.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                tables = {x[0] for x in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                self.assertFalse(any(x.startswith("tbl_8_") or x.startswith("tbl_9_") for x in tables))
                self.assertGreater(db.execute("SELECT COUNT(*) FROM entries WHERE LENGTH(jp)>=8").fetchone()[0], 0)
                for key, word in db.execute("SELECT key,value FROM entries WHERE LENGTH(jp)>=8 LIMIT 20"):
                    table = "tbl_others_" + key[0]
                    self.assertIsNotNone(db.execute(f'SELECT 1 FROM "{table}" WHERE key=? AND value=?', (key, word)).fetchone())
            db.close()
            # A failed source hash must leave a previously built asset byte-for-byte intact.
            original = output.read_bytes()
            with tempfile.TemporaryDirectory() as altered:
                broken = Path(altered)
                broken_catalog = json.loads((source / "catalog.json").read_text(encoding="utf-8"))
                (broken / "catalog.json").write_text(json.dumps(broken_catalog), encoding="utf-8")
                (broken / (broken_catalog["books"][0]["id"] + ".tsv")).write_text("篡改\tcuan gai\t1\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "bytes changed"):
                    MODULE.build(broken, output)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
