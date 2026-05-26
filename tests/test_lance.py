import os
import re
import pytest
import time
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DAEMON_PATH = "/media/fat/trans/a2065d_ddr3"
DIAG_CMD = "share:lance-test diags > share:lance-test.log"
SERIAL_CMD = os.path.join(BASE_DIR, "serial_long.py")
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260525b.rbf")


def _clean_serial(out):
    out = re.sub(r"\d+h", "", out)
    return re.sub(r"[\x9b\x0f\r]|\x1b\[[^@-~]*[@-~]", "", out)


@pytest.fixture(scope="session")
def ssh():
    with MiSTerSSH() as client:
        yield client


@pytest.fixture(scope="session", autouse=True)
def load_core(ssh):
    print(f"Loading {CORE_NAME}...")
    _, err, code = ssh.load_core(CORE_NAME)
    assert code == 0, f"load_core failed: {err}"
    time.sleep(3)
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert CORE_NAME in out, f"Core not loaded: {out}"


@pytest.fixture(scope="session", autouse=True)
def deploy_serial(ssh):
    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_long.py")
    sftp.close()


def test_amiga_booted(ssh):
    for attempt in range(12):
        out, err, code = ssh.execute("python3 /tmp/serial_long.py", timeout=30)
        assert code == 0, f"Serial check failed: {err}"
        if "DHO:" in out:
            return
        if attempt < 11:
            time.sleep(5)
    pytest.fail("Amiga did not boot — DHO: prompt not detected")


@pytest.fixture(scope="module")
def diag_output(ssh):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null; sleep 1", timeout=5)
    ssh.execute(
        f"nohup {DAEMON_PATH} --iface eth1 > /tmp/a2065d.log 2>&1 &", timeout=5
    )
    time.sleep(10)

    out, _, _ = ssh.execute("ps -ef | grep a2065d_ddr3 | grep -v grep")
    assert "a2065d" in out, f"ARM daemon did not start: {out}"

    log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    print(f"\n--- a2065d_ddr3 startup log ---\n{log}\n")

    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_long.py")
    sftp.close()

    out, err, code = ssh.execute(
        f'python3 /tmp/serial_long.py "{DIAG_CMD}" 60', timeout=120
    )
    cleaned = _clean_serial(out)
    print(f"\n--- lance-test diags output ---\n{cleaned}\n")

    time.sleep(60)

    a2065d_log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    print(f"\n--- a2065d.log ---\n{a2065d_log}\n")

    lance_log, _, _ = ssh.execute("cat /media/usb0/games/Amiga/shared/lance-test.log")
    print(f"\n--- lance-test.log ---\n{lance_log}\n")

    ssh.load_core(CORE_NAME)

    yield cleaned


def test_daemon_started(diag_output):
    assert diag_output, "No serial output captured from lance-test diags"


def test_lance_diag_ran(diag_output):
    assert "lance" in diag_output.lower() or "LANCE" in diag_output, \
        f"No LANCE output found in diag output"
