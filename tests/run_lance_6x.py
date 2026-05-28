import os
import subprocess
import sys
import time
import threading
from datetime import datetime

from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON = os.path.join(BASE_DIR, ".venv/bin/python")
TEST = os.path.join(BASE_DIR, "test_lance.py")
LOG_DIR = os.path.join(BASE_DIR, "lance_logs")
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260525b.rbf")
RUNS = 6
STALL_TIMEOUT = 180
MAX_RETRIES = 3
MAX_TOTAL_TIMEOUT = 900

os.makedirs(LOG_DIR, exist_ok=True)


def reload_core(ssh):
    print(f"  [recovery] Reloading core {CORE_NAME}...")
    ssh.load_core(CORE_NAME)
    time.sleep(5)
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    if CORE_NAME in out:
        print(f"  [recovery] Core loaded OK")
        return True
    else:
        print(f"  [recovery] WARNING: Core may not be loaded: {out.strip()}")
        return False


def run_single_test(log_file):
    last_output_time = [time.time()]
    output_lines = []
    failed = [False]

    def read_output(proc):
        for line in iter(proc.stdout.readline, ""):
            if not line:
                break
            output_lines.append(line)
            last_output_time[0] = time.time()
            line_stripped = line.strip()
            if line_stripped:
                print(f"    {line_stripped}")
            if "Interrupt test" in line and "FAIL" in line:
                print(f"  [detect] Interrupt test FAIL detected (continuing)")
            if "Collision" in line and "FAIL" in line:
                print(f"  [detect] Collision test FAIL detected")
                failed[0] = True

    proc = subprocess.Popen(
        [PYTHON, "-m", "pytest", TEST, "-v", "-s"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        cwd=BASE_DIR,
    )

    reader = threading.Thread(target=read_output, args=(proc,), daemon=True)
    reader.start()

    stalled = False
    start_time = time.time()
    try:
        while proc.poll() is None:
            elapsed = time.time() - last_output_time[0]
            total_elapsed = time.time() - start_time

            if elapsed > STALL_TIMEOUT:
                print(f"  [detect] STALL: no output for {STALL_TIMEOUT}s — killing")
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                stalled = True
                break

            if total_elapsed > MAX_TOTAL_TIMEOUT:
                print(f"  [detect] TIMEOUT: {MAX_TOTAL_TIMEOUT}s total — killing")
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                stalled = True
                break

            if failed[0]:
                print(f"  [detect] Test failure detected — killing")
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                break

            time.sleep(1)
    except KeyboardInterrupt:
        proc.terminate()
        proc.wait()
        raise

    reader.join(timeout=3)

    with open(log_file, "w") as f:
        f.writelines(output_lines)

    if failed[0]:
        return "failed"
    elif stalled:
        return "stalled"
    elif proc.returncode == 0:
        return "pass"
    else:
        return "error"


results = []
with MiSTerSSH() as ssh:
    for i in range(1, RUNS + 1):
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_file = os.path.join(LOG_DIR, f"run{i}_{timestamp}.log")

        print(f"\n{'='*60}")
        print(f"  RUN {i}/{RUNS} — {timestamp}")
        print(f"{'='*60}")

        result = None
        for attempt in range(1, MAX_RETRIES + 1):
            if attempt > 1:
                print(f"\n  --- Retry attempt {attempt}/{MAX_RETRIES} ---")
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                log_file = os.path.join(LOG_DIR, f"run{i}_{timestamp}.log")

            result = run_single_test(log_file)

            if result == "pass":
                print(f"  Run {i}: PASS (attempt {attempt})")
                break
            elif result in ("stalled", "error"):
                print(f"  Run {i}: {result.upper()} (attempt {attempt}) — recovering...")
                reload_core(ssh)
            elif result == "failed":
                print(f"  Run {i}: FAIL (lance-test subtest failure, attempt {attempt})")
                reload_core(ssh)
            else:
                break

        status = "PASS" if result == "pass" else "FAIL"
        results.append((i, timestamp, status, log_file))

print(f"\n{'='*60}")
print(f"  SUMMARY")
print(f"{'='*60}")
for i, ts, status, log in results:
    print(f"  Run {i} [{ts}]: {status} — {os.path.basename(log)}")

total_pass = sum(1 for _, _, s, _ in results if s == "PASS")
print(f"\n  {total_pass}/{RUNS} passed — logs in {LOG_DIR}/")
