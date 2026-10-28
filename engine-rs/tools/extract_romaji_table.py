"""从 C++ 源码抽出罗马字 → 假名的映射表，生成 msime-core/src/romaji_table.rs。

表在 engine/japanese/romaji_converter.cpp 的 RomajiTable() 里；这里照源码顺序原样抽出，不手抄。
用法：python engine-rs/tools/extract_romaji_table.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / "engine/japanese/romaji_converter.cpp").read_text(encoding="utf-8")
body = source.split("RomajiTable()", 1)[1].split("return table;", 1)[0]
pairs = re.findall(r'\{"([^"]+)",\s*"([^"]+)"\}', body)
assert len(pairs) > 150, len(pairs)
keys = [k for k, _ in pairs]
assert len(keys) == len(set(keys)), "表里有重复的罗马字键"

lines = ["// 由 engine-rs/tools/extract_romaji_table.py 从 engine/japanese/romaji_converter.cpp 抽出，不要手改。", "", "pub const ROMAJI_TABLE: &[(&str, &str)] = &["]
for key, kana in pairs:
    lines.append(f'    ("{key}", "{kana}"),')
lines.append("];")
(root / "engine-rs/msime-core/src/romaji_table.rs").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(f"{len(pairs)} 项")
