#!/usr/bin/env python3
import os
import re
import sys
import time
from datetime import datetime
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
CORE_NAME = os.environ.get("A2065_CORE", "/media/fat/trans/Minimig_20260609a.rbf")
DAEMON_PATH = os.environ.get("A2065_DAEMON", "/media/fat/trans/a2065d_doorbell")
DAEMON_NAME = os.path.basename(DAEMON_PATH)
DAEMON_IFACE = "eth1"
SERIAL_CMD = os.path.join(BASE_DIR, "serial_cmd.py")
SERIAL_LONG = os.path.join(BASE_DIR, "serial_long.py")
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 100
LOG_DIR = os.path.join(BASE_DIR, "lance_logs")

TEST_NAMES = ["Buffer memory", "LANCE config", "Interrupt", "Collision logic", "Internal loopback"]


def clean_serial(out):
    out = re.sub(r"\d+h", "", out)
    return re.sub(r"[\x9b\x0f\r]|\x1b\[[^@-~]*[@-~]", "", out)


def load_core(ssh):
    _, err, code = ssh.load_core(CORE_NAME)
    if code != 0:
        print(f"  ERROR: load_core failed: {err}")
        return False
    time.sleep(3)
    out, _, _ = ssh.execute("ps -ef | grep MiStor | grep -v grep")
    if CORE_NAME not in out:
        out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    return True


def wait_for_boot(ssh, timeout=60):
    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_cmd.py")
    sftp.put(SERIAL_LONG, "/tmp/serial_long.py")
    sftp.close()
    for attempt in range(15):
        out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        if code == 0 and "DHO:" in out:
            return True
        time.sleep(5)
    print(f"  ERROR: Amiga did not boot")
    return False


def start_daemon(ssh):
    ssh.execute(f"killall -9 {DAEMON_NAME} minimig_netd 2>/dev/null; sleep 1", timeout=5)
    ssh.execute(f"rm -f /tmp/a2065d.log", timeout=5)
    ssh.execute(
        f"nohup {DAEMON_PATH} --iface {DAEMON_IFACE} > /tmp/a2065d.log 2>&1 &",
        timeout=5,
    )
    time.sleep(5)
    out, _, _ = ssh.execute("ps -ef | grep a2065d | grep -v grep")
    if "a2065d" not in out:
        print(f"  ERROR: Daemon did not start")
        return False
    log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    if "Running" not in log:
        print(f"  ERROR: Daemon not in Running state")
        return False
    return True


def stop_daemon(ssh):
    ssh.execute(f"killall -9 {DAEMON_NAME} 2>/dev/null", timeout=5)


def safe_execute(ssh, cmd, timeout=10):
    try:
        return ssh.execute(cmd, timeout=timeout)
    except Exception:
        time.sleep(2)
        try:
            ssh.disconnect()
        except Exception:
            pass
        try:
            ssh.connect()
            return ssh.execute(cmd, timeout=timeout)
        except Exception:
            return ("", "", -1)


def run_lance_diag(ssh):
    shared_log = "/media/usb0/games/Amiga/shared/lance-test.log"
    safe_execute(ssh, f'rm -f {shared_log}', timeout=5)
    try:
        ssh.execute(
            'timeout 10 python3 /tmp/serial_long.py "cd share:" 5', timeout=15
        )
    except Exception:
        pass
    try:
        ssh.execute(
            'timeout 15 python3 /tmp/serial_long.py "lance-test diags > lance-test.log" 10', timeout=20
        )
    except Exception:
        pass
    for _ in range(240):
        time.sleep(1)
        out, _, _ = safe_execute(ssh, f'cat {shared_log} 2>/dev/null', timeout=5)
        if "loopback test" in out.lower() and ("PASS" in out or "FAIL" in out or "WARN" in out):
            return out
    out, _, _ = safe_execute(ssh, f'cat {shared_log} 2>/dev/null', timeout=5)
    return out


def parse_results(output):
    results = {}
    for test_name in TEST_NAMES:
        short = test_name.split()[0].lower()
        if test_name == "LANCE config":
            pattern = r"LANCE configuration test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        elif test_name == "Collision logic":
            pattern = r"LANCE collision logic test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
        else:
            pattern = rf"{re.escape(test_name)} test[^.]*\.\{{2,\}}\s*(PASS|FAIL|WARN)"

        m = re.search(pattern, output, re.IGNORECASE)
        if m:
            results[test_name] = m.group(1)
        else:
            alt = test_name.replace("LANCE ", "").replace("logic", "").strip()
            if "buffer" in short:
                pat2 = r"Buffer memory test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
            elif "config" in short:
                pat2 = r"configuration test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
            elif "interrupt" in short:
                pat2 = r"Interrupt test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
            elif "collision" in short:
                pat2 = r"collision logic test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
            elif "internal" in short:
                pat2 = r"Internal loopback test[^.]*\.{2,}\s*(PASS|FAIL|WARN)"
            else:
                pat2 = None
            if pat2:
                m2 = re.search(pat2, output, re.IGNORECASE)
                results[test_name] = m2.group(1) if m2 else "MISSING"
            else:
                results[test_name] = "MISSING"

    if "Ethernet Controller Diagnostics" not in output and "lance" not in output.lower():
        results["_valid"] = False
    else:
        results["_valid"] = True
    return results


def recovery(ssh):
    print(f"  [recovery] Full restart...")
    stop_daemon(ssh)
    time.sleep(1)
    if not load_core(ssh):
        return False
    if not wait_for_boot(ssh):
        return False
    if not start_daemon(ssh):
        return False
    return True


def write_report(results_list, filename):
    total = len(results_list)
    valid = sum(1 for r in results_list if r.get("_valid", False))

    counts = {}
    for test_name in TEST_NAMES:
        p = sum(1 for r in results_list if r.get(test_name) == "PASS")
        f = sum(1 for r in results_list if r.get(test_name) == "FAIL")
        w = sum(1 for r in results_list if r.get(test_name) == "WARN")
        m = sum(1 for r in results_list if r.get(test_name) == "MISSING")
        counts[test_name] = (p, f, w, m)

    lines = []
    lines.append(f"# lance-test 100x Results")
    lines.append(f"")
    lines.append(f"**Date:** {datetime.now().strftime('%Y-%m-%d %H:%M')}")
    lines.append(f"**Core:** {CORE_NAME}")
    lines.append(f"**Total runs:** {total}")
    lines.append(f"**Valid runs:** {valid}")
    lines.append(f"**Note:** Hardware loopback plug on eth1")
    lines.append(f"")
    lines.append(f"## Summary")
    lines.append(f"")
    lines.append(f"| Test | PASS | WARN | FAIL | MISSING | Pass Rate |")
    lines.append(f"|------|------|------|------|---------|-----------|")
    for test_name in TEST_NAMES:
        p, f, w, m = counts[test_name]
        rate = f"{100*p/valid:.0f}%" if valid > 0 else "N/A"
        lines.append(f"| {test_name} | {p} | {w} | {f} | {m} | {rate} |")
    lines.append(f"")

    fail_runs = []
    for i, r in enumerate(results_list):
        if not r.get("_valid", False):
            fail_runs.append((i + 1, "NO OUTPUT", ""))
            continue
        failed_tests = [t for t in TEST_NAMES if r.get(t) == "FAIL"]
        missing = [t for t in TEST_NAMES if r.get(t) == "MISSING"]
        if failed_tests or missing:
            detail = ", ".join(failed_tests + [f"MISSING: {t}" for t in missing])
            fail_runs.append((i + 1, detail, ""))

    if fail_runs:
        lines.append(f"## Failure Details")
        lines.append(f"")
        lines.append(f"| Run | Failed Tests |")
        lines.append(f"|-----|-------------|")
        for run_num, detail, _ in fail_runs:
            lines.append(f"| {run_num} | {detail} |")
        lines.append(f"")
    else:
        lines.append(f"**All tests passed all runs.**")
        lines.append(f"")

    with open(filename, "w") as f:
        f.write("\n".join(lines))
    print(f"\nReport written to {filename}")


def main():
    os.makedirs(LOG_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_file = os.path.join(LOG_DIR, f"results_100x_{timestamp}.md")

    results_list = []
    consecutive_fails = 0

    with MiSTerSSH() as ssh:
        print(f"=== lance-test {RUNS}x runner ===")
        print(f"Core: {CORE_NAME}")
        print(f"Daemon: {DAEMON_PATH} --iface {DAEMON_IFACE}")
        print()

        print(f"[setup] Loading core...")
        if not load_core(ssh):
            sys.exit(1)
        print(f"[setup] Waiting for boot...")
        if not wait_for_boot(ssh):
            sys.exit(1)
        print(f"[setup] Starting daemon...")
        if not start_daemon(ssh):
            sys.exit(1)
        print(f"[setup] Ready")
        print()

        for i in range(1, RUNS + 1):
            stop_daemon(ssh)
            time.sleep(0.5)
            if not start_daemon(ssh):
                consecutive_fails += 1
                if consecutive_fails >= 3:
                    print(f"  [recovery] Daemon won't start, full restart...")
                    if not recovery(ssh):
                        print(f"  FATAL: recovery failed")
                        break
                continue

            ts = datetime.now().strftime("%H:%M:%S")
            print(f"  [{ts}] Run {i}/{RUNS}...", end=" ", flush=True)

            output = run_lance_diag(ssh)

            if not output.strip():
                print("NO OUTPUT (bridge dead?)")
                results_list.append({"_valid": False})
                print(f"  [recovery] Immediate restart...")
                if not recovery(ssh):
                    print(f"  FATAL: recovery failed")
                    break
                consecutive_fails = 0
                continue

            results = parse_results(output)

            if not results.get("_valid", False):
                print("NO OUTPUT")
                results_list.append(results)
                consecutive_fails += 1
                if consecutive_fails >= 3:
                    print(f"  [recovery] 3 consecutive failures, restarting...")
                    if not recovery(ssh):
                        print(f"  FATAL: recovery failed")
                        break
                    consecutive_fails = 0
                continue

            consecutive_fails = 0
            fails = [t for t in TEST_NAMES if results.get(t) == "FAIL"]
            misses = [t for t in TEST_NAMES if results.get(t) == "MISSING"]
            warns = [t for t in TEST_NAMES if results.get(t) == "WARN"]

            if not fails and not misses and not warns:
                print("ALL PASS")
            else:
                parts = []
                for t in TEST_NAMES:
                    s = results.get(t, "?")
                    if s != "PASS":
                        parts.append(f"{t}={s}")
                print("FAIL: " + ", ".join(parts))

            # Save daemon log for all runs
            daemon_log, _, _ = safe_execute(ssh, "tail -2000 /tmp/a2065d.log", timeout=5)
            if daemon_log.strip():
                daemon_log_file = os.path.join(LOG_DIR, f"daemon_{i:03d}_{timestamp}.log")
                with open(daemon_log_file, "w") as f:
                    f.write(daemon_log)

            results_list.append(results)

            if i % 10 == 0:
                p10 = sum(
                    1
                    for r in results_list[-10:]
                    if all(r.get(t) in ("PASS", "WARN") for t in TEST_NAMES)
                )
                print(f"  --- last 10: {p10}/10 pass ---")

            log_path = os.path.join(LOG_DIR, f"run{i:03d}_{timestamp}.log")
            with open(log_path, "w") as f:
                f.write(output)

    write_report(results_list, report_file)


if __name__ == "__main__":
    main()
