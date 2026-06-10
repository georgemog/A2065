#!/usr/bin/env python3
"""Categorise failures by test result, showing signature breakdown per test+status."""

import re
import sys
import os
from pathlib import Path
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
import classify_failures as cf

TESTS = ["Buffer memory", "LANCE config", "Interrupt", "Collision logic", "Internal loopback"]
ts = "20260603_104916"
log_dir = os.path.join(os.path.dirname(__file__), "lance_logs")

# Read per-run test results
run_results = {}
for r in range(1, 101):
    rn = f"{r:03d}"
    p = os.path.join(log_dir, f"run{rn}_{ts}.log")
    try:
        text = Path(p).read_text()
    except FileNotFoundError:
        continue
    run_results[r] = {}
    for t in TESTS:
        if t == "LANCE config":
            pat = r"LANCE configuration test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        elif t == "Collision logic":
            pat = r"LANCE collision logic test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        else:
            pat = rf"{re.escape(t)} test[^.]*\.{{2,}}\s*(PASS|FAIL|WARN)"
        m = re.search(pat, text, re.IGNORECASE)
        run_results[r][t] = m.group(1) if m else "MISSING"

# Overall counts
print("## Overall Test Results\n")
print(f"{'Test':<20} {'PASS':>5} {'WARN':>5} {'FAIL':>5} {'MISS':>6} {'Pass%':>6}")
print("-" * 50)
for t in TESTS:
    counts = defaultdict(int)
    for r in range(1, 101):
        counts[run_results[r][t]] += 1
    total_pass = counts["PASS"]
    print(f"{t:<20} {counts['PASS']:>5} {counts['WARN']:>5} {counts['FAIL']:>5} {counts['MISSING']:>6} {100*total_pass//100:>5}%")

# Breakdown by test + status
sig_by_test_status = defaultdict(lambda: defaultdict(int))

for r in range(1, 101):
    rn = f"{r:03d}"
    daemon_text = ""
    dp = os.path.join(log_dir, f"daemon_{rn}_{ts}.log")
    try:
        daemon_text = Path(dp).read_text()
    except FileNotFoundError:
        pass
    d = cf.analyze_daemon(daemon_text)

    for t in TESTS:
        st = run_results[r][t]
        if st == "PASS":
            continue
        sig = cf.classify(t, st, d)
        key = f"{t} = {st}"
        sig_by_test_status[key][sig] += 1

print("\n## Failure Breakdown by Test Result\n")

for t in TESTS:
    for st in ["FAIL", "WARN", "MISSING"]:
        key = f"{t} = {st}"
        if key not in sig_by_test_status:
            continue
        total = sum(sig_by_test_status[key].values())
        print(f"### {key} ({total} occurrences)\n")
        print(f"  {'Signature':<45} {'Count':>5} {'%':>6}")
        print(f"  {'-'*45} {'-----':>5} {'------':>6}")
        for sig, cnt in sorted(sig_by_test_status[key].items(), key=lambda x: -x[1]):
            pct = 100 * cnt / total
            print(f"  {sig:<45} {cnt:>5} {pct:>5.1f}%")
        print()

# Also show root cause category summary per test
print("## Root Cause Category per Test\n")
cat_by_test = defaultdict(lambda: defaultdict(int))
for t in TESTS:
    for st in ["FAIL", "WARN", "MISSING"]:
        key = f"{t} = {st}"
        for sig, cnt in sig_by_test_status[key].items():
            cat = sig[0]  # A, B, or ?
            if cat == "?":
                cat = "C"
            cat_by_test[t][cat] += cnt

print(f"{'Test':<20} {'A:DDR3':>8} {'B:Loop':>8} {'Total':>6}")
print("-" * 45)
for t in TESTS:
    a = cat_by_test[t]["A"]
    b = cat_by_test[t]["B"]
    print(f"{t:<20} {a:>8} {b:>8} {a+b:>6}")
