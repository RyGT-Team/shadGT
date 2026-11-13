#!/usr/bin/env python3
# DIAG-SHC-001: summarises logs/pipeline_compiles.csv written with RYGT_PIPELINE_COMPILE_LOG=1 or 2.
# Usage: python pipeline_compile_report.py <game>/logs/pipeline_compiles.csv [--top 25]
import argparse
import csv
import re
from collections import defaultdict


def stat_values(text):
    """'exe[name=v;name=v] exe[...]' -> {stat name: max value over the executables}."""
    out = {}
    for body in re.findall(r"\[(.*?)\]", text or ""):
        for pair in body.split(";"):
            if "=" not in pair:
                continue
            k, v = pair.split("=", 1)
            try:
                out[k] = max(out.get(k, float("-inf")), float(v))
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    names, rows = {}, []
    with open(args.csv, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r["kind"] == "name":
                names[r["handle"]] = r["stages"]
            else:
                rows.append(r)
    if not rows:
        print("No compiles logged.")
        return

    by_kind = defaultdict(lambda: [0, 0.0, 0, 0])
    for r in rows:
        k = by_kind[r["kind"]]
        k[0] += 1
        k[1] += float(r["call_ms"])
        k[2] += r["cache"] == "hit"
        k[3] += r["cache"] == "miss"
    span = (float(rows[-1]["ms"]) - float(rows[0]["ms"])) / 1000
    print(f"{len(rows)} driver builds over {span:.1f} s\n")
    print(f"{'kind':16}{'count':>7}{'total s':>10}{'avg ms':>9}{'hits':>7}{'misses':>8}")
    for kind, (n, ms, hit, miss) in sorted(by_kind.items(), key=lambda kv: -kv[1][1]):
        print(f"{kind:16}{n:>7}{ms / 1000:>10.2f}{ms / n:>9.1f}{hit:>7}{miss:>8}")

    def name(r):
        return names.get(r["handle"], "")[:90]

    print(f"\nSlowest {args.top} builds:")
    for r in sorted(rows, key=lambda r: -float(r["call_ms"]))[: args.top]:
        print(f"  {float(r['call_ms']):8.1f} ms  {r['kind']:14} {r['cache']:7} t{r['thread']:<6} "
              f"{r['stages']:30} {name(r)}")

    # Bursts: seconds in which the driver spent the most time compiling (stutter candidates).
    per_second = defaultdict(float)
    for r in rows:
        per_second[int(float(r["ms"]) // 1000)] += float(r["call_ms"])
    print("\nBusiest seconds (driver ms compiled in that second, summed over threads):")
    for sec, ms in sorted(per_second.items(), key=lambda kv: -kv[1])[:10]:
        print(f"  t={sec:6d} s  {ms:8.1f} ms")

    stats = [(r, stat_values(r["statistics"])) for r in rows if r["statistics"]]
    if stats:
        keys = sorted({k for _, s in stats for k in s})
        print("\nDriver statistics seen:", ", ".join(keys))
        for key in [k for k in keys if re.search(r"vgpr|sgpr|scratch|lds|spill|occup|wave|instr", k, re.I)]:
            print(f"\nHighest {key}:")
            for r, s in sorted(stats, key=lambda rs: -rs[1].get(key, float("-inf")))[:8]:
                if key in s:
                    print(f"  {s[key]:10g}  {float(r['call_ms']):7.1f} ms  {r['kind']:10} {name(r)}")


if __name__ == "__main__":
    main()
