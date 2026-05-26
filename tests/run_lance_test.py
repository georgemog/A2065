#!/usr/bin/env python3
"""
A2065 Lance-Test automated runner.

Workflow:
  1. Load Minimig core on MiSTer via SSH
  2. Wait for Amiga boot
  3. Start ARM daemon
  4. Run Lance-Test on the Amiga via serial
  5. Capture test output and daemon log

Usage:
  python3 run_lance_test.py                    # full run
  python3 run_lance_test.py --skip-load        # skip core reload
  python3 run_lance_test.py --skip-test        # daemon management only
  python3 run_lance_test.py --manual           # print manual instructions
"""

import subprocess
import time
import sys
import argparse

MISTER = "mister.broadband"
RBF_DEFAULT = "/media/fat/Minimig_20260525a.rbf"
DAEMON = "/media/fat/trans/a2065d_ddr3"
DAEMON_LOG = "/tmp/a2065d.log"
SERIAL = "/dev/ttyS1"
SHARED_DIR = "/media/usb0/games/Amiga/shared"
BOOT_WAIT = 30
DAEMON_WAIT = 10
TEST_TIMEOUT = 60

LANCE_RUNNER_SCRIPT = """\
echo "=== Lance-Test Start ===" >serial:
cd shared:
Lance-Test >serial:
echo "=== Lance-Test End ===" >serial:
"""


def ssh(cmd, timeout=30):
    try:
        r = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=5", "-o", "StrictHostKeyChecking=no",
             f"root@{MISTER}", cmd],
            capture_output=True, text=True, timeout=timeout,
        )
        return r.stdout.strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "", -1


def step(n, msg):
    print(f"\n[{n}] {msg}")


def check_mister():
    out, rc = ssh("echo ok", timeout=8)
    if rc != 0 or "ok" not in out:
        print("ERROR: MiSTer not reachable")
        sys.exit(1)
    print(f"  MiSTer reachable at {MISTER}")


def load_core(rbf):
    step(1, f"Loading core: {rbf}")
    ssh(f"echo 'load_core {rbf}' > /dev/MiSTer_cmd", timeout=10)
    print(f"  Core load command sent")
    print(f"  Waiting {BOOT_WAIT}s for Amiga boot...")
    time.sleep(BOOT_WAIT)


def check_serial():
    step(2, "Checking Amiga boot via serial")
    ssh(f"stty -F {SERIAL} 115200 raw -echo", timeout=5)
    out, _ = ssh(f"timeout 2 cat {SERIAL} 2>&1 | head -3", timeout=10)
    if out:
        print(f"  Serial activity: {out[:100]}")
    else:
        print("  No serial output (assuming booted)")
    return bool(out)


def start_daemon(iface):
    step(3, "Starting ARM daemon")
    ssh("killall -9 a2065d_ddr3 2>/dev/null", timeout=5)
    time.sleep(1)
    ssh(f"rm -f {DAEMON_LOG}", timeout=5)

    runner = f"/tmp/a2065_start.sh"
    ssh(f"cat > {runner} << 'EOF'\n#!/bin/sh\ncd /media/fat/trans && ./a2065d_ddr3 --iface {iface} > {DAEMON_LOG} 2>&1 &\nEOF\nchmod +x {runner}", timeout=5)
    ssh(f"sh {runner}", timeout=5)
    time.sleep(2)

    out, _ = ssh("ps | grep a2065d_ddr3 | grep -v grep", timeout=5)
    if "a2065d" in out:
        print(f"  Daemon running (PID {out.split()[0]})")
    else:
        print("  WARNING: Daemon may not have started")

    print(f"  Waiting {DAEMON_WAIT}s for stabilization...")
    time.sleep(DAEMON_WAIT)

    header, _ = ssh(f"head -8 {DAEMON_LOG}", timeout=5)
    if header:
        for line in header.split("\n"):
            print(f"  | {line}")


def deploy_runner():
    step(4, "Deploying serial runner to shared drive")
    runner_path = f"{SHARED_DIR}/RunLanceTest"
    ssh(f"printf '{LANCE_RUNNER_SCRIPT}' > {runner_path}", timeout=5)
    out, _ = ssh(f"cat {runner_path}", timeout=5)
    if "Lance-Test" in out:
        print(f"  Runner deployed: shared:RunLanceTest")
    else:
        print(f"  WARNING: Runner deployment may have failed")


def run_lance_test_serial():
    step(5, f"Running Lance-Test via serial (timeout {TEST_TIMEOUT}s)")

    ssh(f"stty -F {SERIAL} 115200 raw -echo", timeout=5)

    print("  Sending command to Amiga serial port...")
    ssh(f"echo 'Execute shared:RunLanceTest' > {SERIAL}", timeout=5)

    print(f"  Capturing serial output...")
    out, _ = ssh(f"timeout {TEST_TIMEOUT} cat {SERIAL} 2>&1", timeout=TEST_TIMEOUT + 15)
    return out


def capture_daemon_log():
    step(6, "Capturing daemon log")
    out, _ = ssh(f"cat {DAEMON_LOG} 2>&1", timeout=10)
    return out


def print_manual():
    print("""
=== Manual Lance-Test Instructions ===

The Amiga serial console is not responding to remote commands.
Run Lance-Test manually:

  1. On the Amiga, open AmigaShell (Right-Amiga + W or WB → Shell)
  2. Type:
       cd shared:
       Lance-Test

  To redirect output to serial (captured by this script):
       cd shared:
       Lance-Test >serial:

  Or run the deployed serial runner:
       Execute shared:RunLanceTest
""")


def analyze(serial_out, daemon_log):
    step(7, "Results")
    print()

    test_results = False

    if serial_out and len(serial_out) > 5:
        print("=== Lance-Test Output ===")
        print(serial_out)
        print("=== End ===\n")
        test_results = True
    else:
        print("  No serial output from Amiga.")
        print("  The Amiga does not have a serial console configured.")
        print_manual()

    if daemon_log:
        lines = daemon_log.split("\n")
        key = [l for l in lines if "[req" in l or "[a2065]" in l
               or "Starting" in l or "fakemac" in l or "STOP" in l]
        print(f"=== Daemon Log ({len(key)} key lines of {len(lines)} total) ===")
        for line in key[:80]:
            print(f"  {line}")
        if len(key) > 80:
            print(f"  ... ({len(key) - 80} more lines)")
        print("=== End ===\n")

    if test_results:
        passes = serial_out.count("PASS")
        fails = serial_out.count("FAIL")
        print(f"Summary: {passes} passed, {fails} failed")
        return 0 if fails == 0 else 1

    print("Summary: Manual run required (no serial results)")
    return 2


def main():
    p = argparse.ArgumentParser(description="Run Lance-Test on MiSTer A2065")
    p.add_argument("--rbf", default=RBF_DEFAULT)
    p.add_argument("--iface", default="eth1")
    p.add_argument("--boot-wait", type=int, default=BOOT_WAIT)
    p.add_argument("--test-timeout", type=int, default=TEST_TIMEOUT)
    p.add_argument("--skip-load", action="store_true")
    p.add_argument("--skip-daemon", action="store_true")
    p.add_argument("--skip-test", action="store_true")
    p.add_argument("--manual", action="store_true", help="Print manual instructions only")
    args = p.parse_args()

    print("=== A2065 Lance-Test Runner ===")

    if args.manual:
        print_manual()
        sys.exit(0)

    try:
        check_mister()

        if not args.skip_load:
            load_core(args.rbf)
        else:
            step(1, "Skipping core load")

        if not args.skip_daemon:
            check_serial()
            start_daemon(args.iface)
        else:
            step("2-3", "Skipping daemon management")

        serial_out = ""
        if not args.skip_test:
            deploy_runner()
            serial_out = run_lance_test_serial()
        else:
            step("4-5", "Skipping Lance-Test")

        daemon_log = capture_daemon_log()
        rc = analyze(serial_out, daemon_log)
        sys.exit(rc)

    except KeyboardInterrupt:
        print("\nInterrupted")
        sys.exit(1)
    except Exception as e:
        print(f"\nERROR: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
