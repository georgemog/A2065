import os
import subprocess
import sys
from datetime import datetime

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON = os.path.join(BASE_DIR, ".venv/bin/python")
TEST = os.path.join(BASE_DIR, "test_lance.py")
LOG_DIR = os.path.join(BASE_DIR, "lance_logs")
RUNS = 6

os.makedirs(LOG_DIR, exist_ok=True)

results = []
for i in range(1, RUNS + 1):
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = os.path.join(LOG_DIR, f"run{i}_{timestamp}.log")

    print(f"\n{'='*60}")
    print(f"  RUN {i}/{RUNS} — {timestamp}")
    print(f"{'='*60}")

    with open(log_file, "w") as f:
        proc = subprocess.run(
            [PYTHON, "-m", "pytest", TEST, "-v", "-s"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            cwd=BASE_DIR,
            timeout=1200,
        )
        f.write(proc.stdout)

    passed = proc.returncode == 0
    results.append((i, timestamp, passed, log_file))
    status = "PASS" if passed else "FAIL"
    print(f"  Run {i}: {status}")

print(f"\n{'='*60}")
print(f"  SUMMARY")
print(f"{'='*60}")
for i, ts, passed, log in results:
    status = "PASS" if passed else "FAIL"
    print(f"  Run {i} [{ts}]: {status} — {os.path.basename(log)}")

total_pass = sum(1 for _, _, p, _ in results if p)
print(f"\n  {total_pass}/{RUNS} passed — logs in {LOG_DIR}/")
