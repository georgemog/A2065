#!/usr/bin/env python3
"""Classify lance-test failures from run/daemon logs into failure signatures."""

import re
import os
import sys
from collections import defaultdict
from pathlib import Path

TESTS = ["Buffer memory", "LANCE config", "Interrupt", "Collision logic", "Internal loopback"]


def read_file(path):
    try:
        return Path(path).read_text()
    except FileNotFoundError:
        return ""


def get_test_statuses(run_text):
    results = {}
    for test in TESTS:
        if test == "LANCE config":
            pat = r"LANCE configuration test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        elif test == "Collision logic":
            pat = r"LANCE collision logic test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        else:
            pat = rf"{re.escape(test)} test[^.]*\.{{2,}}\s*(PASS|FAIL|WARN)"
        m = re.search(pat, run_text, re.IGNORECASE)
        results[test] = m.group(1) if m else "MISSING"
    return results


def analyze_daemon(text):
    if not text:
        return {"has_log": False}
    reqs = re.findall(r'\[req (\d+)\]', text)
    safe_reqs = re.findall(r'\(safe\)\]', text)
    health = len(re.findall(r'bridge health', text))
    inits_00 = len(re.findall(r'chip_init: mode=0000', text))
    inits_54 = len(re.findall(r'chip_init: mode=0054', text))
    inits_44 = len(re.findall(r'chip_init: mode=0044', text))
    tx_coll = len(re.findall(r'TX LOOP\+COLL', text))
    tx_loop = len(re.findall(r'TX LOOP \d', text))
    rx_loop = len(re.findall(r'RX LOOP OK', text))
    stops = len(re.findall(r'\[a2065\] STOP', text))
    starts = len(re.findall(r'\[a2065\] START', text))

    phantom = False
    phantom_rsp = ""
    phantom_init = bool(re.search(r'mode=FFFF', text))
    all_lines = text.strip().split('\n')
    last_lines = all_lines[-10:]
    health_flood = False
    last_reqs = [l for l in all_lines if '[req' in l][-20:]
    if len(last_reqs) >= 10:
        rsps = re.findall(r'rsp=([0-9A-Fa-f]+)', '\n'.join(last_reqs))
        if rsps and rsps.count(rsps[-1]) >= 8:
            phantom = True
            phantom_rsp = rsps[-1]
    if not phantom:
        last_raws = re.findall(r'raw=(0x[0-9A-Fa-f]+)', '\n'.join(last_reqs[-2:])) if len(last_reqs) >= 2 else []
        if len(last_raws) == 2 and last_raws[0] == last_raws[1]:
            phantom = True
            phantom_rsp = re.findall(r'rsp=([0-9A-Fa-f]+)', last_reqs[-1])
            phantom_rsp = phantom_rsp[0] if phantom_rsp else ""

    rw_reads = len(re.findall(r'rw=0 addr=00', text))
    total_req_count = int(reqs[-1]) + 1 if reqs else 0
    read_ratio = rw_reads / total_req_count if total_req_count > 0 else 0
    poll_saturated = total_req_count > 200 and read_ratio > 0.7

    bridge_dead = False
    if total_req_count > 0:
        last_status = re.findall(r'\[a2065\] (STOP|START|INIT)', '\n'.join(all_lines[-5:]))
        trailing_reads = re.findall(r'rw=0 addr=00.*rsp=000[14]', '\n'.join(all_lines[-15:]))
        if len(trailing_reads) >= 10 and len(last_status) == 0:
            bridge_dead = True

    return {
        "has_log": True,
        "reqs": total_req_count,
        "safe_reqs": len(safe_reqs),
        "health": health,
        "inits": inits_00 + inits_54 + inits_44,
        "inits_44": inits_44,
        "inits_54": inits_54,
        "tx_coll": tx_coll,
        "tx_loop": tx_loop,
        "rx_loop": rx_loop,
        "stops": stops,
        "starts": starts,
        "phantom": phantom,
        "phantom_rsp": phantom_rsp,
        "phantom_init": phantom_init,
        "health_flood": health_flood,
        "poll_saturated": poll_saturated,
        "read_ratio": read_ratio,
        "bridge_dead": bridge_dead,
    }


def classify(test, status, d):
    if not d["has_log"]:
        return "C1:NO_DAEMON_LOG"

    if test == "LANCE config" and status in ("FAIL", "MISSING"):
        if d["reqs"] == 0:
            return "A12:NO_REQUESTS_EVER"
        if d["phantom"] and d["inits"] == 0:
            return "A13:PHANTOM_READ_NO_INIT"
        if d["phantom"] and d["bridge_dead"]:
            return "A14:CSR0_POLL_STUCK"
        if d["poll_saturated"] and d["inits"] == 0:
            return "A14:CSR0_POLL_STUCK"
        if d["bridge_dead"] and d["inits"] == 0:
            return "A15:BRIDGE_DEAD_NO_INIT"
        return "A?:CONFIG_FAIL_UNK"

    if test == "Interrupt" and status == "FAIL":
        if d["phantom"] and d["inits"] == 0:
            return "A13:PHANTOM_READ_NO_INIT"
        if d["phantom"]:
            return "A3:PHANTOM_READ_MID"
        if d["bridge_dead"]:
            return "A15:BRIDGE_DEAD_NO_INIT"
        if d["poll_saturated"] and d["inits"] >= 2:
            return "A14:CSR0_POLL_STUCK"
        return "A?:INT_FAIL_UNK"

    if test == "Interrupt" and status == "MISSING":
        if d["phantom"]:
            return "A5:PHANTOM_CASCADE"
        if d["bridge_dead"]:
            return "A15:BRIDGE_DEAD_CASCADE"
        return "A5:PHANTOM_CASCADE"

    if test == "Collision logic" and status == "FAIL":
        if d["phantom"] and d["tx_coll"] == 0:
            return "A6:PHANTOM_NO_COLL"
        if d["bridge_dead"] and d["tx_coll"] == 0:
            return "A15:BRIDGE_DEAD_NO_COLL"
        if d["tx_coll"] > 0 and d["tx_coll"] < 10:
            return "A8:COLL_PARTIAL"
        if d["poll_saturated"] and d["tx_coll"] == 0:
            return "A14:CSR0_POLL_STUCK"
        return "A?:COLL_FAIL_UNK"

    if test == "Collision logic" and status == "MISSING":
        if d["phantom"]:
            return "A5:PHANTOM_CASCADE"
        if d["bridge_dead"]:
            return "A15:BRIDGE_DEAD_CASCADE"
        if d["reqs"] == 0:
            return "A12:NO_REQUESTS_EVER"
        return "A?:COLL_MISS_UNK"

    if test == "Internal loopback" and status == "FAIL":
        if d["inits_44"] > 0 and d["tx_loop"] == 0 and d["health"] < 500 and not d["phantom"]:
            return "B1:LOOP_INIT_NO_TX"
        if d["inits_44"] == 0 and d["tx_coll"] > 0:
            return "A6:PHANTOM_AFTER_COLL_NO_LOOP"
        if d["phantom"]:
            return "A6:PHANTOM_NO_LOOP"
        if d["bridge_dead"]:
            return "A15:BRIDGE_DEAD_NO_LOOP"
        if d["poll_saturated"] and d["inits_44"] > 0:
            return "A14:CSR0_POLL_STUCK"
        return "B?:LOOP_FAIL_UNK"

    if test == "Internal loopback" and status == "WARN":
        if d["inits_44"] > 0 and d["tx_loop"] == 0 and d["phantom"]:
            return "A10:PHANTOM_AFTER_LOOP_INIT"
        if d["inits_44"] == 0 and d["tx_coll"] > 0 and d["phantom"] and d["tx_loop"] == 0:
            return "A6:PHANTOM_AFTER_COLL_NO_LOOP"
        if d["tx_loop"] > 0 and d["rx_loop"] > 0:
            return "B2:LOOP_ALL_RX_OK_WARN"
        if d["tx_loop"] > 0 and d["rx_loop"] == 0:
            return "B5:LOOP_TX_NO_RX_WARN"
        return "B?:LOOP_WARN_UNK"

    if test == "Internal loopback" and status == "MISSING":
        if d["inits_44"] > 0 and d["tx_loop"] > 0 and d["tx_loop"] < 50:
            return "B3:LOOP_PARTIAL_CAPTURE"
        if d["inits_44"] > 0 and d["tx_loop"] == 0:
            return "B4:LOOP_INIT_NO_TX_CAPTURE"
        if d["phantom"]:
            return "A5:PHANTOM_CASCADE_NO_LOOP"
        if d["bridge_dead"]:
            return "A15:BRIDGE_DEAD_NO_LOOP"
        return "B?:LOOP_MISS_UNK"

    return "?:UNCLASSIFIED"


SIGNATURE_DESCRIPTIONS = {
    "A3:PHANTOM_READ_MID": "FPGA stuck in phantom CSR0 read loop after interrupt test INIT. Phantom reads in tail of daemon log. Interrupt test fails because ISR clears status bits but MBX_INT timing race leaves INT2 asserted.",
    "A5:PHANTOM_CASCADE": "Bridge stuck (phantom reads or bridge dead) causes earlier test failure, preventing collision/loopback from running. Cascading failure from Interrupt or Config test.",
    "A5:PHANTOM_CASCADE_NO_LOOP": "Earlier test failure (Config, Interrupt, or Collision) prevents loopback test from running. Loopback MISSING because preceding test already failed and test sequence stopped.",
    "A6:PHANTOM_NO_COLL": "Phantom read loop prevents collision test TX packets. 0 TX_COLL seen by daemon. The Amiga writes TDMD but the register bridge path is stuck.",
    "A6:PHANTOM_AFTER_COLL_NO_LOOP": "Collision test completes (10 TX_COLL) but phantom reads prevent loopback INIT. Daemon never sees mode=0x0044. Bridge dies between collision and loopback.",
    "A6:PHANTOM_NO_LOOP": "Phantom read loop prevents loopback test from running. Loopback FAIL because bridge is stuck.",
    "A8:COLL_PARTIAL": "Collision test partially works (1-9 of 10 TX_COLL) but not all packets get through.",
    "A10:PHANTOM_AFTER_LOOP_INIT": "Phantom reads start after loopback INIT completes. mode=0x0044 INIT seen but 0 TX LOOP packets. Bridge dies between INIT and first TDMD.",
    "A12:NO_REQUESTS_EVER": "Daemon starts but sees 0 register requests. DDR3 init race — daemon polling before FPGA mailbox adapter is ready.",
    "A13:PHANTOM_READ_NO_INIT": "Phantom read loop with no inits. Daemon sees repeated CSR0 reads (all returning same rsp). The Amiga driver is stuck polling before sending INIT. Bridge health is low/zero.",
    "A14:CSR0_POLL_STUCK": "Amiga polls CSR0 in tight loop generating many identical reads. >70% reads with >200 total requests. The Amiga's state machine is stuck — wrote a value and keeps reading back, never seeing expected flags change.",
    "A15:BRIDGE_DEAD_NO_INIT": "FPGA bridge path is dead — daemon sees trailing CSR0 reads returning STOP (0x0004) or stale data with no STOP/START/INIT in tail. No inits completed.",
    "A15:BRIDGE_DEAD_CASCADE": "FPGA bridge path died during an earlier test, causing cascading MISSING results for all subsequent tests.",
    "A15:BRIDGE_DEAD_NO_COLL": "FPGA bridge path died before collision test could send any TX packets. 0 TX_COLL.",
    "A15:BRIDGE_DEAD_NO_LOOP": "FPGA bridge path died before loopback test could run. inits_44=0 or tx_loop=0.",
    "B1:LOOP_INIT_NO_TX": "Loopback INIT (mode=0x0044) completes but 0 TX LOOP packets sent. Bridge healthy. Daemon-side issue.",
    "B2:LOOP_ALL_RX_OK_WARN": "All loopback packets TX/RX successfully (tx_loop==rx_loop) but test reports WARN. Test binary data mismatch or serial capture truncation — daemon log shows clean loopback.",
    "B3:LOOP_PARTIAL_CAPTURE": "Loopback test starts (some TX LOOP) but log capture times out before completion. Test output truncated.",
    "B4:LOOP_INIT_NO_TX_CAPTURE": "Loopback INIT completes but 0 TX packets, and test output times out. Similar to B1 but captured as MISSING.",
    "B5:LOOP_TX_NO_RX_WARN": "Loopback TX packets sent but 0 RX LOOP OK received. Daemon transmits but loopback receive path fails.",
    "C1:NO_DAEMON_LOG": "No daemon log saved for this run (runs 61+ had no daemon logs due to test timeout).",
}


def main():
    results_file = sys.argv[1] if len(sys.argv) > 1 else None
    if not results_file:
        print("Usage: classify_failures.py <results_file> [run_dir] [daemon_dir]")
        sys.exit(1)

    log_dir = os.path.dirname(results_file)
    run_dir = sys.argv[2] if len(sys.argv) > 2 else log_dir
    daemon_dir = sys.argv[3] if len(sys.argv) > 3 else log_dir

    ts_match = re.search(r'(\d{8}_\d{6})', os.path.basename(results_file))
    ts = ts_match.group(1) if ts_match else "unknown"

    print(f"=== lance-test Failure Classifier ===")
    print(f"Timestamp: {ts}\n")

    sig_count = defaultdict(int)
    sig_runs = defaultdict(list)
    run_data = {}

    for r in range(1, 101):
        rn = f"{r:03d}"
        run_text = read_file(os.path.join(run_dir, f"run{rn}_{ts}.log"))
        daemon_text = read_file(os.path.join(daemon_dir, f"daemon_{rn}_{ts}.log"))
        d = analyze_daemon(daemon_text)
        statuses = get_test_statuses(run_text)

        for test in TESTS:
            st = statuses[test]
            if st == "PASS":
                continue
            sig = classify(test, st, d)
            sig_count[sig] += 1
            sig_runs[sig].append(r)
            run_data[(r, test)] = {"status": st, "sig": sig, "d": d}

    # Signature summary
    print("## Signature Summary\n")
    print(f"{'SIGNATURE':<45} {'COUNT':>5}  RUNS")
    print(f"{'-'*45} {'-----':>5}  {'-'*4}")
    for sig in sorted(sig_count.keys()):
        runs_str = ",".join(str(r) for r in sig_runs[sig])
        print(f"{sig:<45} {sig_count[sig]:>5}  {runs_str}")
    print(f"\nTotal failures: {sum(sig_count.values())}\n")

    # Root cause categories
    cat_a = sum(c for s, c in sig_count.items() if s.startswith("A"))
    cat_b = sum(c for s, c in sig_count.items() if s.startswith("B"))
    cat_c = sum(c for s, c in sig_count.items() if s.startswith("C") or s.startswith("?"))
    print("## Root Cause Categories\n")
    print(f"A: DDR3 Phantom Read / Bridge Stuck:  {cat_a:3d} failures ({100*cat_a//(cat_a+cat_b+cat_c or 1):3d}%)")
    print(f"B: Loopback Test Logic / Timing:      {cat_b:3d} failures ({100*cat_b//(cat_a+cat_b+cat_c or 1):3d}%)")
    print(f"C: Other / Unclassified:               {cat_c:3d} failures\n")

    # Signature descriptions
    print("## Signature Descriptions\n")
    for sig in sorted(sig_count.keys()):
        desc = SIGNATURE_DESCRIPTIONS.get(sig, "Unknown signature")
        print(f"**{sig}** ({sig_count[sig]} occurrences)")
        print(f"  {desc}\n")

    # Detailed per-run listing
    print("## Per-Run Details\n")
    hdr = f"{'RUN':>4} {'TEST':<20} {'ST':<8} {'SIGNATURE':<45} {'REQS':>5} {'INIT':>5} {'TXC':>4} {'TXL':>4} {'RXL':>4} {'PH':>2} {'BD':>2} {'SAT':>3}"
    print(hdr)
    print("-" * len(hdr))
    for r in range(1, 101):
        for test in TESTS:
            key = (r, test)
            if key not in run_data:
                continue
            rd = run_data[key]
            d = rd["d"]
            ph = "Y" if d.get("phantom") else ""
            bd = "Y" if d.get("bridge_dead") else ""
            sat = "Y" if d.get("poll_saturated") else ""
            print(f"{r:>4} {test:<20} {rd['status']:<8} {rd['sig']:<45} {d.get('reqs',0):>5} {d.get('inits',0):>5} {d.get('tx_coll',0):>4} {d.get('tx_loop',0):>4} {d.get('rx_loop',0):>4} {ph:>2} {bd:>2} {sat:>3}")


if __name__ == "__main__":
    main()
