#!/usr/bin/env python3
"""比对两份黄金样本输出（golden_probe 的 JSONL），看候选顺序是否一致。

用法：
  golden_compare.py 期望.jsonl 实际.jsonl [--top N] [--weights]
  golden_compare.py --trim 输入.jsonl 输出.jsonl [--top N]   # 只留每条的前 N 个候选，用来生成要提交的期望文件

默认只比每条前 N（20）个候选的文字顺序和预编辑；加 --weights 连权重一起比。
退出码：0 全部一致，1 有差异，2 参数或文件有问题。
"""
import argparse
import json
import sys


def load(path):
    rows = {}
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            rows[(row["scheme"], row["keys"], row.get("fuzzy", 0), row.get("autocorrect", 0))] = row
    return rows


def label(key):
    """输入的可读名：方案 按键，带选项的再补上模糊音/纠错掩码。"""
    extra = (f" fuzzy={key[2]}" if key[2] else "") + (f" autocorrect={key[3]}" if key[3] else "")
    return f"{key[0]} {key[1]}{extra}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("first")
    parser.add_argument("second")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--weights", action="store_true")
    parser.add_argument("--trim", action="store_true")
    args = parser.parse_args()

    if args.trim:
        with open(args.first, encoding="utf-8") as source, open(args.second, "w", encoding="utf-8", newline="\n") as target:
            for line in source:
                if not line.strip():
                    continue
                row = json.loads(line)
                row["snapshot"]["candidates"] = row["snapshot"]["candidates"][: args.top]
                target.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
        return 0

    expected, actual = load(args.first), load(args.second)
    different = 0
    for key in expected:
        if key not in actual:
            print(f"缺少：{label(key)}")
            different += 1
            continue
        a, b = expected[key]["snapshot"], actual[key]["snapshot"]
        problems = []
        if a["preedit"] != b["preedit"]:
            problems.append(f"预编辑 {a['preedit']!r} ≠ {b['preedit']!r}")

        def view(snapshot):
            items = snapshot["candidates"][: args.top]
            return [(c["text"], c["weight"]) if args.weights else c["text"] for c in items]

        left, right = view(a), view(b)
        if left != right:
            index = next((i for i, (x, y) in enumerate(zip(left, right)) if x != y), min(len(left), len(right)))
            problems.append(f"候选第 {index + 1} 位起不同：{left[index:index + 3]} ≠ {right[index:index + 3]}")
        if problems:
            different += 1
            print(f"{label(key)}：" + "；".join(problems))
    extra = set(actual) - set(expected)
    for key in sorted(extra):
        print(f"多出：{label(key)}")
    print(f"共 {len(expected)} 条，{different} 条有差异，{len(extra)} 条多出")
    return 1 if different or extra else 0


if __name__ == "__main__":
    sys.exit(main())
