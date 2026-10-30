#!/usr/bin/env python3
"""从 engine/shuangpin/shuangpin_profile.cpp 抽取九套内置双拼方案，输出成 Rust 函数。

用法：extract_shuangpin_profiles.py [C++ 源文件] > 生成的函数
输出贴在 msime-core/src/shuangpin.rs 末尾那条“由脚本抽取”的分隔线之后。
抽完用 `cargo test -p msime-core --test shuangpin_helpcode_golden` 对着 C++ 导出的基线核对。
"""
import re
import sys

DEFAULT_SOURCE = "engine/shuangpin/shuangpin_profile.cpp"


def top_level_groups(text):
    groups, depth, current = [], 0, ""
    for ch in text:
        if ch == "{":
            depth += 1
            if depth == 1:
                current = ""
                continue
        if ch == "}":
            depth -= 1
            if depth == 0:
                groups.append(current)
                continue
        if depth >= 1:
            current += ch
    return groups


def main():
    source = open(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOURCE, encoding="utf-8").read()
    source = re.sub(r"//[^\n]*", "", source)
    functions = re.findall(
        r"const ShuangpinProfile &Get(\w+)ShuangpinProfile\(\)\s*\{\s*static const ShuangpinProfile profile\{(.*?)\n    \};",
        source,
        re.S,
    )
    if len(functions) != 9:
        sys.exit(f"期望 9 套方案，抽到 {len(functions)} 套")
    out = []
    for _, body in functions:
        match = re.match(r'\s*"(\w+)",(.*)', body, re.S)
        name, rest = match.group(1), match.group(2)
        groups = top_level_groups(rest)
        if len(groups) != 3:
            sys.exit(f"{name}：期望声母、零声母、韵母三张表，抽到 {len(groups)} 张")
        tables = [re.findall(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}', g) for g in groups]

        def lines(pairs):
            return ",\n".join(f'            ("{a}", "{b}")' for a, b in pairs)

        out.append(
            f"fn {name}() -> Profile {{\n    Profile::new(\n        \"{name}\",\n        &[\n{lines(tables[0])},\n        ],\n"
            f"        &[\n{lines(tables[1])},\n        ],\n        &[\n{lines(tables[2])},\n        ],\n    )\n}}\n"
        )
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
