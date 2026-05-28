import os
import re
import time
import pytest
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DAEMON_PATH = "/media/fat/trans/a2065d_ddr3"
DAEMON_IFACE = "eth1"
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260528a.rbf")

SERIAL_CMD = os.path.join(BASE_DIR, "serial_cmd.py")
SERIAL_LONG = os.path.join(BASE_DIR, "serial_long.py")


def clean_serial(out):
    out = re.sub(r"\d+h", "", out)
    return re.sub(r"[\x9b\x0f\r]|\x1b\[[^@-~]*[@-~]", "", out)


def extract_section(text, test_name):
    match = re.search(
        rf"--- {re.escape(test_name)}[^\n]*---\s*\n(.*?)(?=\n---|\n===|$)",
        text, re.DOTALL
    )
    return match.group(1) if match else ""


def count_passes(text):
    return len(re.findall(r"^\s*PASS:", text, re.MULTILINE))


def count_fails(text):
    return len(re.findall(r"^\s*FAIL:", text, re.MULTILINE))


@pytest.fixture(scope="session")
def ssh():
    with MiSTerSSH() as client:
        yield client


@pytest.fixture(scope="session")
def load_core(ssh):
    _, err, code = ssh.load_core(CORE_NAME)
    assert code == 0, f"load_core failed: {err}"
    time.sleep(3)
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert CORE_NAME in out, f"Core not loaded: {out}"


@pytest.fixture(scope="session")
def deploy_serial(ssh):
    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_cmd.py")
    sftp.put(SERIAL_LONG, "/tmp/serial_long.py")
    sftp.close()


def wait_for_boot(ssh, timeout=60):
    for attempt in range(12):
        out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        assert code == 0, f"Serial check failed: {err}"
        if "DHO:" in out:
            return True
        if attempt < 11:
            time.sleep(5)
    pytest.fail("Amiga did not boot — DHO: prompt not detected")


def run_amiga_cmd(ssh, cmd, timeout=60):
    out, err, code = ssh.execute(
        f'python3 /tmp/serial_cmd.py "{cmd}"', timeout=timeout
    )
    return clean_serial(out)


def run_amiga_cmd_long(ssh, cmd, wait=30, timeout=120):
    out, err, code = ssh.execute(
        f'python3 /tmp/serial_long.py "{cmd}" {wait}', timeout=timeout
    )
    return clean_serial(out)


def start_daemon(ssh, iface=DAEMON_IFACE):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null; sleep 1", timeout=5)
    ssh.execute(
        f"nohup {DAEMON_PATH} --iface {iface} > /tmp/a2065d.log 2>&1 &", timeout=5
    )
    time.sleep(5)
    out, _, _ = ssh.execute("ps -ef | grep a2065d | grep -v grep")
    assert "a2065d" in out, f"ARM daemon did not start: {out}"
    log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    assert "Running" in log, f"Daemon didn't reach Running state: {log}"
    return log


def stop_daemon(ssh):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null", timeout=5)


def get_daemon_log(ssh):
    out, _, _ = ssh.execute("cat /tmp/a2065d.log 2>/dev/null || echo '(no log)'")
    return out
