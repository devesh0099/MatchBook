#!/usr/bin/env python3
"""analyze.py — turn a noise-floor run into a hardware decision.

    ops/noise-floor/analyze.py noise-floor.jsonl [soak.jsonl]

Reports the spread of per-run medians, and — if a soak file is given — the drift
between its first and last twenty runs. Then maps both onto the decision table
from the plan, because the point of the measurement is to make the choice
mechanical rather than a matter of taste.

What this can and cannot tell you: it quantifies how much the machine moves
under a FIXED workload. It says nothing about whether the absolute number is
right. A node that is uniformly 8 percent slow produces a beautifully tight
spread around a wrong value; that is what the end-of-event rejudge block and the
reference spot checks are for.
"""

import json
import statistics
import sys


def load(path):
    p50s, stamps = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            r = row.get("result") or {}
            v = r.get("p50_ns")
            if isinstance(v, (int, float)) and v > 0:
                p50s.append(float(v))
                stamps.append(row.get("at"))
    return p50s, stamps


def describe(name, xs):
    if not xs:
        print(f"{name}: no usable samples")
        return None
    lo, hi = min(xs), max(xs)
    med = statistics.median(xs)
    spread = (hi - lo) / med * 100.0
    # The interquartile range is the honest headline: min-max is one bad run
    # away from being a story about a single interrupt.
    q = statistics.quantiles(xs, n=4) if len(xs) >= 4 else [med, med, med]
    iqr_pct = (q[2] - q[0]) / med * 100.0
    print(f"{name}")
    print(f"  n          {len(xs)}")
    print(f"  median     {med:.2f} ns")
    print(f"  min / max  {lo:.2f} / {hi:.2f} ns")
    print(f"  spread     {spread:.2f}%  (max-min over median)")
    print(f"  IQR        {iqr_pct:.2f}%")
    if len(xs) >= 2:
        print(f"  stdev      {statistics.stdev(xs) / med * 100.0:.2f}% of median")
    return {"median": med, "spread": spread, "iqr": iqr_pct}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    p50s, _ = load(sys.argv[1])
    print("=" * 60)
    spread = describe("spread test", p50s)
    if spread is None:
        return 1

    drift_pct = None
    if len(sys.argv) > 2:
        soak, _ = load(sys.argv[2])
        print()
        describe("soak", soak)
        if len(soak) >= 40:
            first = statistics.median(soak[:20])
            last = statistics.median(soak[-20:])
            drift_pct = abs(last - first) / first * 100.0
            print()
            print("drift")
            print(f"  first 20   {first:.2f} ns")
            print(f"  last 20    {last:.2f} ns")
            print(f"  drift      {drift_pct:.2f}%")
        else:
            print("\n  (need at least 40 soak samples to talk about drift)")

    print()
    print("=" * 60)
    s = spread["spread"]
    print("READ (plan section 9)")
    if s < 2.0 and (drift_pct is None or drift_pct < 1.0):
        print("  Spread under 2% and drift under 1%.")
        print("  -> A dedicated instance is fine. Save the money, skip drift")
        print("     correction, and answer section 16 q1 'no canary' and q2")
        print("     'dedicated'.")
    elif s < 5.0:
        print("  Spread 2-5%.")
        print("  -> Dedicated works, but revisit drift correction (q1). The")
        print("     cheap middle option is the reference spot check every ~20")
        print("     minutes as a MONITORING signal rather than a per-submission")
        print("     scoring divisor: ~5% throughput cost instead of ~200%.")
    else:
        print(f"  Spread {s:.1f}% — at or above the level where metal is justified.")
        print("  -> Answer q2 'metal' and re-run this on the metal instance")
        print("     before trusting anything. A leaderboard built on this much")
        print("     noise is ranking the machine, not the submissions.")

    print()
    print("Note this is the spread of a SINGLE run, and a score is the median")
    print("of nine. The median is far steadier than any one run, so compare")
    print("the IQR above against the gaps you expect to resolve rather than")
    print("max-min. Measured on a shared c6i.2xlarge: 4.5% single-run spread,")
    print("1.05% IQR, and a median-of-9 stable to +/-0.6% — which separates")
    print("engines 2% apart every time. Worth knowing before the reveal.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
