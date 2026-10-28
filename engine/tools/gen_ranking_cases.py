#!/usr/bin/env python3
"""生成调频黄金用例（engine/contracts/golden/ranking_cases.tsv），固定种子，重复运行输出相同。

列的含义见 ranking_probe.cpp。手写的边界用例在前，随机用例在后。
"""
import random
import sys

CEILING = 100_000_000
INT64_MAX = 9_223_372_036_854_775_807


def case(rows, selected, mode="halve", step=1, trigger=1, force=0, calls=1):
    cells = ",".join(f"{w}:{k}" for w, k in rows)
    return f"{cells}\t{selected}\t{mode}\t{step}\t{trigger}\t{force}\t{calls}"


def descending(*weights, key="a"):
    return [(w, key) for w in weights]


def main():
    out = ["# 调频黄金用例，由 engine/tools/gen_ranking_cases.py 生成。列见 engine/tools/ranking_probe.cpp。"]
    base = descending(500_000, 400_000, 300_000, 200_000, 100_000, 50_000, 10_000)

    # 三种模式 × 不同名次
    for mode in ("halve", "linear", "promote"):
        for selected in range(len(base)):
            out.append(case(base, selected, mode, step=2))
    out.append(case(base, 5, "linear", step=1))
    out.append(case(base, 5, "linear", step=3))
    out.append(case(base, 5, "linear", step=0))
    out.append(case(base, 5, "disabled"))
    out.append(case(base, 5, "unknown-mode"))
    for selected in (1, 4, 6):
        out.append(case(base, selected, "halve", force=1))
        out.append(case(base, selected, "disabled", force=1))

    # 触发次数：连选几次才挪
    for trigger in (1, 2, 3, 10, 99):
        for calls in (1, 2, 3, 4):
            out.append(case(base, 4, "halve", trigger=trigger, calls=calls))
    out.append(case(base, 0, "halve", trigger=3, calls=3))

    # 相邻权重：没有整数中点，要跳过已被占用的权重
    out.append(case(descending(100, 99, 98, 97, 96), 4, "promote"))
    out.append(case(descending(101, 100, 100, 99), 3, "halve"))
    out.append(case(descending(100, 99), 1, "halve"))
    out.append(case(descending(20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1), 19, "promote"))
    out.append(case(descending(20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1), 10, "halve"))
    # 权重相等或过低：整排重排的台阶放不下
    out.append(case(descending(1, 1, 1, 1, 1, 1), 5, "halve"))
    out.append(case(descending(5, 5, 5, 5), 3, "promote"))
    out.append(case(descending(3, 2, 1, 1, 1), 4, "halve"))
    out.append(case(descending(0, 0, 0), 2, "halve"))
    out.append(case(descending(1, 1), 1, "halve"))
    out.append(case(descending(7, 1, 1, 1, 1), 4, "linear", step=1))
    # 放在最顶上
    out.append(case(descending(10, 9, 8), 2, "halve", force=1))
    out.append(case(descending(CEILING - 5, 10, 9), 2, "halve", force=1))
    out.append(case(descending(CEILING, 10, 9), 2, "halve", force=1))
    out.append(case(descending(CEILING + 7, CEILING, 9), 2, "halve", force=1))
    out.append(case(descending(INT64_MAX - 10, 10, 9), 2, "halve", force=1))
    out.append(case(descending(INT64_MAX, 5, 4, 3), 3, "halve", force=1))
    # 超过上限的权重
    out.append(case(descending(CEILING * 3, CEILING * 2, CEILING + 5, 100, 50), 3, "halve"))
    out.append(case(descending(CEILING * 3, CEILING * 2, CEILING + 5, 100, 50), 4, "promote"))
    out.append(case(descending(INT64_MAX, INT64_MAX, 100, 50), 3, "halve"))
    # 显示顺序和权重顺序不一致（整句占着前面、置顶的被提到前面）
    out.append(case([(10, "a"), (900, "a"), (500, "a"), (30, "a")], 0, "halve"))
    out.append(case([(10, "a"), (900, "a"), (500, "a"), (30, "a")], 3, "promote"))
    out.append(case([(10, "a"), (900, "a"), (500, "a"), (30, "a")], 2, "halve", force=1))
    # 同一列表里混着别的读音：只有 a 的行会被写
    mixed = [(900, "b"), (800, "a"), (700, "b"), (600, "a"), (500, "b"), (400, "a"), (300, "a")]
    for selected in (1, 3, 5, 6):
        for mode in ("halve", "promote", "linear"):
            out.append(case(mixed, selected, mode, step=2))
    out.append(case([(3460998, "b"), (1, "a"), (1, "a"), (1, "a")], 3, "halve"))
    out.append(case([(3460998, "b"), (1, "a"), (1, "a"), (1, "a")], 3, "halve", force=1))
    out.append(case([(50, "b"), (50, "b"), (50, "a")], 2, "halve"))
    # 单行
    out.append(case([(100, "a")], 0, "halve"))
    out.append(case([(100, "a")], 0, "halve", force=1))

    # 随机：多种权重分布
    rng = random.Random(20261028)
    for index in range(160):
        count = rng.randint(2, 24)
        shape = rng.choice(["wide", "tight", "ties", "tiny", "big", "mixed-keys"])
        if shape == "wide":
            weights = sorted((rng.randint(1, 80_000_000) for _ in range(count)), reverse=True)
        elif shape == "tight":
            top = rng.randint(100, 5_000)
            weights = sorted((top - rng.randint(0, 8) for _ in range(count)), reverse=True)
        elif shape == "ties":
            weights = sorted((rng.choice([1, 2, 3, 100, 100, 100]) for _ in range(count)), reverse=True)
        elif shape == "tiny":
            weights = sorted((rng.randint(0, 4) for _ in range(count)), reverse=True)
        elif shape == "big":
            weights = sorted((rng.choice([CEILING - 1, CEILING, CEILING + 1, CEILING * 5, rng.randint(1, 99)]) for _ in range(count)), reverse=True)
        else:
            weights = sorted((rng.randint(1, 1_000_000) for _ in range(count)), reverse=True)
        rows = [(w, "b" if (shape == "mixed-keys" and rng.random() < 0.4) else "a") for w in weights]
        # 偶尔打乱显示顺序
        if rng.random() < 0.25:
            rng.shuffle(rows)
        owned = [i for i, (_, k) in enumerate(rows) if k == "a"]
        if not owned:
            rows[0] = (rows[0][0], "a")
            owned = [0]
        selected = rng.choice(owned)
        mode = rng.choice(["halve", "linear", "promote", "halve", "promote"])
        out.append(
            case(
                rows,
                selected,
                mode,
                step=rng.randint(1, 4),
                trigger=rng.choice([1, 1, 1, 2, 3]),
                force=1 if rng.random() < 0.15 else 0,
                calls=rng.choice([1, 1, 1, 2, 3]),
            )
        )
    text = "\n".join(out) + "\n"
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
