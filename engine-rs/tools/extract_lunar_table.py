"""从 C++ 源码抽出农历年表，生成 msime-core/src/lunar_table.rs。

表在 engine/local_modes/date_time_query.cpp 的 kLunarYearInfo 里（1900–2100，每年一项）。
用法：python engine-rs/tools/extract_lunar_table.py
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / "engine/local_modes/date_time_query.cpp").read_text(encoding="utf-8")
body = source.split("kLunarYearInfo = {", 1)[1].split("};", 1)[0]
values = re.findall(r"0x[0-9a-fA-F]+", body)
assert len(values) == 201, len(values)
lines = ["// 由 engine-rs/tools/extract_lunar_table.py 从 engine/local_modes/date_time_query.cpp 抽出，不要手改。", "", "pub const LUNAR_YEAR_INFO: [u32; 201] = ["]
for start in range(0, len(values), 8):
    lines.append("    " + ", ".join(values[start:start + 8]) + ",")
lines.append("];")
(root / "engine-rs/msime-core/src/lunar_table.rs").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(len(values))
