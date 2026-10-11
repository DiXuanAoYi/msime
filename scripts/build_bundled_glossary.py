"""Build the pinned Qingjian gloss tables without changing the base dictionary."""
import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "assets" / "qingjian-glossary"


def build(output: Path, source: Path = SOURCE) -> dict[str, int]:
    manifest = json.loads((source / "source.json").read_text(encoding="utf-8"))
    output.parent.mkdir(parents=True, exist_ok=True)
    counts = {}
    # Build atomically so a failed checksum/parse cannot replace a good database.
    fd, name = tempfile.mkstemp(prefix="glossary-", suffix=".db", dir=output.parent)
    os.close(fd)
    try:
        with closing(sqlite3.connect(name)) as db, db:
            db.execute("CREATE TABLE glosses(language TEXT, word TEXT, gloss TEXT NOT NULL, "
                       "PRIMARY KEY(language,word)) WITHOUT ROWID")
            for filename, expected in manifest["files"].items():
                raw = (source / filename).read_bytes()
                if hashlib.sha256(raw).hexdigest() != expected["sha256"]:
                    raise ValueError(f"Checksum mismatch: {filename}")
                language = filename.removeprefix("glossary-").removesuffix(".tsv")
                rows = []
                for line in raw.decode("utf-8").splitlines():
                    if not line.strip() or line.startswith("#"):
                        continue
                    fields = [s.strip() for s in line.split("\t")]
                    if len(fields) < 2 or not fields[0] or not fields[1]:
                        raise ValueError(f"Invalid glossary row in {filename}")
                    senses = []
                    for field in fields[1:3]:
                        if field:
                            text, sep, reading = field.partition("|")
                            senses.append(text + (f" ({reading})" if sep and reading else ""))
                    rows.append((language, fields[0], "; ".join(senses)))
                if len(rows) != expected["entries"]:
                    raise ValueError(f"Entry count mismatch: {filename}")
                db.executemany("INSERT INTO glosses VALUES(?,?,?)", rows)
                counts[language] = len(rows)
            db.execute("PRAGMA user_version=1")
            if db.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                raise ValueError("Glossary integrity check failed")
        os.replace(name, output)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    return counts


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output), ensure_ascii=False))
