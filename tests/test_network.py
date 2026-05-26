import os
import re
import pytest
import time
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DAEMON_PATH = "/media/fat/trans/a2065d_ddr3"
DIAG_CMD = "share:a2065_diag"
SERIAL_CMD = os.path.join(BASE_DIR, "serial_cmd.py")
SERIAL_LONG = os.path.join(BASE_DIR, "serial_long.py")
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260524e.rbf")


def _clean_serial(out):
    out = re.sub(r"\d+h", "", out)
    return re.sub(r"[\x9b\x0f\r]|\x1b\[[^@-~]*[@-~]", "", out)


@pytest.fixture(scope="session")
def ssh():
    with MiSTerSSH() as client:
        yield client


@pytest.fixture(scope="session", autouse=True)
def load_core(ssh):
    print(f"Forcing load of {CORE_NAME}...")
    _, err, code = ssh.load_core(CORE_NAME)
    assert code == 0, f"load_core failed: {err}"
    for attempt in range(15):
        time.sleep(3)
        out, _, _ = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        if "DHO:" in out:
            print(f"  Amiga booted after {(attempt+1)*3}s")
            return
    pytest.fail("Amiga did not boot after core load")


@pytest.fixture(scope="session", autouse=True)
def deploy_serial(ssh):
    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_cmd.py")
    sftp.put(SERIAL_LONG, "/tmp/serial_long.py")
    sftp.close()


def test_register_sanity(ssh):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null; sleep 1", timeout=5)
    ssh.execute(f"nohup {DAEMON_PATH} --iface eth1 > /tmp/a2065d.log 2>&1 &", timeout=5)
    time.sleep(5)

    out, _, _ = ssh.execute("ps -ef | grep a2065d | grep -v grep")
    assert "a2065d" in out, f"ARM daemon did not start: {out}"

    log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    assert "Running" in log, f"Daemon didn't reach Running state: {log}"

    for attempt in range(12):
        out, err, code = ssh.execute(
            f'python3 /tmp/serial_cmd.py "{DIAG_CMD}"', timeout=60
        )
        cleaned = _clean_serial(out)
        if "Results:" in cleaned:
            break
        if attempt < 11:
            time.sleep(5)
    else:
        pytest.fail("a2065_diag never completed")

    match = re.search(r"Results:\s+(\d+)\s+passed,\s+(\d+)\s+failed", cleaned)
    assert match, f"No summary in diag output: {cleaned}"
    p, f = int(match.group(1)), int(match.group(2))
    print(f"  a2065_diag: {p} passed, {f} failed")
    assert p >= 3, f"Register sanity: only {p} passes (need >=3)"


def test_daemon_log(ssh):
    out, _, _ = ssh.execute("cat /tmp/a2065d.log 2>/dev/null || echo '(no log)'")
    print(f"\n--- a2065d_ddr3 log ---\n{out}\n")
    assert "Starting" in out or "Running" in out


def test_addnetinterface(ssh):
    out, err, code = ssh.execute(
        f'python3 /tmp/serial_cmd.py "Copy SYS:Storage/NetInterfaces/A2065#? DEVS:NetInterfaces"',
        timeout=30
    )
    print(f"\n--- Copy A2065 config ---\n{_clean_serial(out)}")

    out, err, code = ssh.execute(
        'python3 /tmp/serial_long.py "AddNetInterface A2065" 60',
        timeout=120
    )
    cleaned = _clean_serial(out)
    print(f"\n--- AddNetInterface A2065 output ---\n{cleaned}\n")

    if "configured" in cleaned.lower():
        match = re.search(r"address\s*=\s*([\d.]+)", cleaned)
        if match:
            print(f"  Interface configured with IP: {match.group(1)}")

    if "timed out" in cleaned.lower() or "could not" in cleaned.lower():
        print("  DHCP configuration timed out (expected without bridge network)")
    elif "configured" in cleaned.lower():
        print("  Interface configured successfully")
    elif "added" in cleaned.lower():
        print("  Interface added (config pending)")
    elif "error" in cleaned.lower() or "fail" in cleaned.lower():
        print(f"  Error detected in output")
    else:
        print(f"  Check output above for details")


def test_showarp(ssh):
    out, err, code = ssh.execute(
        'python3 /tmp/serial_cmd.py "arp -a"',
        timeout=30
    )
    cleaned = _clean_serial(out)
    print(f"\n--- arp -a output ---\n{cleaned}")

    out, err, code = ssh.execute(
        'python3 /tmp/serial_cmd.py "ShowNetStatus"',
        timeout=30
    )
    cleaned = _clean_serial(out)
    print(f"\n--- ShowNetStatus output ---\n{cleaned}")


def test_cleanup(ssh):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null", timeout=5)
