"""Build a reproducible offline character-component catalog, never download models."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def build(source: Path, output: Path):
    from pypinyin import Style, pinyin
    raw = source.read_bytes()
    lock = json.loads(source.with_name('source.json').read_text(encoding='utf-8'))
    if hashlib.sha256(raw).hexdigest() != lock['sha256']:
        raise ValueError('IDS source checksum differs from the pinned catalog')
    entries = []
    for line in raw.decode('utf-8-sig').splitlines():
        if not line.startswith('U+'):
            continue
        fields = line.split('\t')
        character = fields[1]
        if len(character) != 1:
            continue
        variants = [m.groups() for cell in fields[2:] if (m := re.fullmatch(r'\^(.*?)\$\(([^)]+)\)', cell))]
        # Prefer mainland glyph forms; unsupported extensions/unknown graphical components are omitted.
        variants.sort(key=lambda row: ('G' not in row[1], row[1]))
        ids = list(dict.fromkeys(row[0] for row in variants if not re.search(r'[&;①-⑳？↔↷]', row[0])))
        if not ids:
            continue
        readings = pinyin(character, style=Style.TONE, heteronym=True, errors=lambda _: [''])
        reading = ' / '.join(dict.fromkeys(readings[0])) if readings else ''
        entries.append([character, reading, ids[:4]])
    payload = {'version': 1, 'source': lock, 'entries': entries}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, ensure_ascii=False, separators=(',', ':')) + '\n', encoding='utf-8')
    print(f'Built {len(entries)} offline character records; no model weights downloaded.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    base = Path(__file__).resolve().parents[1] / 'assets/hanzi-question'
    parser.add_argument('--source', type=Path, default=base / 'IDS.txt')
    parser.add_argument('--output', type=Path, default=base / 'catalog.json')
    args = parser.parse_args()
    build(args.source, args.output)
