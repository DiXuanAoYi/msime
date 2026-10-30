"""Build the pinned, read-only phonetic domain input asset. No network or models."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import re
import sqlite3
import tempfile


def build(source: Path, output: Path):
    catalog = json.loads((source / "catalog.json").read_text(encoding="utf-8"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".", delete=False) as tmp:
        temporary = Path(tmp.name)
    try:
        with closing(sqlite3.connect(temporary)) as db:
            db.execute("PRAGMA page_size=4096")
            db.execute("CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)")
            db.execute("CREATE TABLE entries(pack TEXT NOT NULL,key TEXT NOT NULL,jp TEXT NOT NULL,"
                       "value TEXT NOT NULL,weight INTEGER NOT NULL,PRIMARY KEY(pack,key,value))")
            db.execute("CREATE INDEX entries_membership ON entries(key,value,pack)")
            db.execute("INSERT INTO metadata VALUES('catalog',?)", (json.dumps(catalog, ensure_ascii=False),))
            tables = set()
            for pack in catalog["books"]:
                data = (source / (pack["id"] + ".tsv")).read_bytes()
                if hashlib.sha256(data).hexdigest() != pack["sha256"]:
                    raise ValueError("Pinned domain bytes changed: " + pack["id"])
                count = 0
                for line in data.decode("utf-8").splitlines():
                    if not line or line.startswith("#"):
                        continue
                    word, spelling, raw_weight = line.split("\t")
                    syllables = spelling.split(" ")
                    if not word or not all(re.fullmatch(r"[a-z]+", s) for s in syllables):
                        raise ValueError("Invalid domain phonetic entry: " + pack["id"])
                    key = "'".join(syllables)
                    jp = "".join(s[0] for s in syllables)
                    weight = int(raw_weight)
                    if not 0 < weight <= 2147483647:
                        raise ValueError("Invalid domain frequency")
                    # Same 1..7 / others convention as contracts/dictionary/format.json.
                    length = str(len(syllables)) if len(syllables) < 8 else "others"
                    table = "tbl_" + length + "_" + syllables[0][0]
                    if table not in tables:
                        db.execute(f'CREATE TABLE "{table}"(pack TEXT NOT NULL,key TEXT NOT NULL,jp TEXT NOT NULL,'
                                   'value TEXT NOT NULL,weight INTEGER NOT NULL,PRIMARY KEY(pack,key,value))')
                        db.execute(f'CREATE INDEX "{table}_key" ON "{table}"(key,pack,weight DESC)')
                        db.execute(f'CREATE INDEX "{table}_jp" ON "{table}"(jp,pack,weight DESC)')
                        tables.add(table)
                    row = (pack["id"], key, jp, word, weight)
                    db.execute("INSERT INTO entries VALUES(?,?,?,?,?)", row)
                    db.execute(f'INSERT INTO "{table}" VALUES(?,?,?,?,?)', row)
                    count += 1
                if count != pack["entries"]:
                    raise ValueError("Pinned domain count changed: " + pack["id"])
            # Empty tables keep lookup failures separate from missing syllables.
            for length in [*map(str, range(1, 8)), "others"]:
                for initial in "abcdefghijklmnopqrstuvwxyz":
                    table = f"tbl_{length}_{initial}"
                    if table not in tables:
                        db.execute(f'CREATE TABLE "{table}"(pack TEXT,key TEXT,jp TEXT,value TEXT,weight INTEGER)')
            db.commit()
            if db.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                raise ValueError("Domain asset failed integrity check")
            db.execute("VACUUM")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
    return catalog


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "assets/domain-input")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = build(args.source, args.output)
    print(json.dumps({"books": len(result["books"]), "entries": sum(x["entries"] for x in result["books"]),
                      "bytes": args.output.stat().st_size, "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest()}))
