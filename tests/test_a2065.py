import os
import re
import pytest
import time
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DAEMON_PATH = "/media/fat/trans/a2065d_ddr3"
DIAG_CMD = "share:a2065_diag"
SERIAL_CMD = os.path.join(BASE_DIR, "serial_cmd.py")
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260510d.rbf")


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
    time.sleep(3)
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert CORE_NAME in out, f"Core not loaded: {out}"


@pytest.fixture(scope="session", autouse=True)
def deploy_serial(ssh):
    sftp = ssh._client.open_sftp()
    sftp.put(SERIAL_CMD, "/tmp/serial_cmd.py")
    sftp.close()


def test_ssh_connection(ssh):
    stdout, _, code = ssh.execute("uname -a")
    assert code == 0
    assert "Linux" in stdout


def test_core_loaded(ssh):
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert CORE_NAME in out, f"Expected {CORE_NAME}, got: {out}"


def test_amiga_booted(ssh):
    for attempt in range(12):
        out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        assert code == 0, f"Serial check failed: {err}"
        if "DHO:" in out:
            return
        if attempt < 11:
            time.sleep(5)
    pytest.fail("Amiga did not boot — DHO: prompt not detected")


def test_showconfig(ssh):
    out, err, code = ssh.execute('python3 /tmp/serial_cmd.py showconfig', timeout=30)
    assert code == 0, f"showconfig failed: {err}"
    cleaned = _clean_serial(out)
    assert "PROCESSOR" in cleaned, f"No PROCESSOR line: {cleaned}"
    assert "CUSTOM CHIPS" in cleaned, f"No CUSTOM CHIPS line: {cleaned}"
    print(f"showconfig OK")


def test_version(ssh):
    out, err, code = ssh.execute('python3 /tmp/serial_cmd.py version', timeout=30)
    assert code == 0, f"version failed: {err}"
    cleaned = _clean_serial(out)
    assert "Kickstart" in cleaned, f"No Kickstart: {cleaned}"
    print(f"version OK")


def test_info(ssh):
    out, err, code = ssh.execute('python3 /tmp/serial_cmd.py info', timeout=30)
    assert code == 0, f"info failed: {err}"
    cleaned = _clean_serial(out)
    assert "DH0" in cleaned, f"No DH0: {cleaned}"
    print(f"info OK")


@pytest.fixture(scope="module")
def daemon_log(ssh):
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null; sleep 1", timeout=5)
    ssh.execute(f"nohup {DAEMON_PATH} > /tmp/a2065d.log 2>&1 &", timeout=5)
    time.sleep(5)
    out, _, _ = ssh.execute("ps -ef | grep a2065d | grep -v grep")
    assert "a2065d" in out, f"ARM daemon did not start: {out}"
    log, _, _ = ssh.execute("cat /tmp/a2065d.log")
    print(f"\n--- a2065d startup log ---\n{log}\n")
    assert "Running" in log, f"Daemon didn't reach Running state: {log}"
    yield
    ssh.execute(f"killall a2065d_ddr3 2>/dev/null", timeout=5)


@pytest.fixture(scope="module")
def diag_output(ssh, daemon_log):
    for attempt in range(12):
        out, _, code = ssh.execute(
            f'python3 /tmp/serial_cmd.py "{DIAG_CMD}"', timeout=60
        )
        cleaned = _clean_serial(out)
        if "Results:" in cleaned:
            return cleaned
        if attempt < 11:
            time.sleep(5)
    pytest.fail("a2065_diag never completed after 12 attempts")


def test_daemon_started(ssh, daemon_log):
    out, _, _ = ssh.execute("cat /tmp/a2065d.log 2>/dev/null || echo '(no log)'")
    print(f"\n--- a2065d_ddr3 log ---\n{out}\n")
    assert "Starting" in out or "Running" in out


def test_a2065_diag_ran(diag_output):
    print(f"\n--- a2065_diag output ---\n{diag_output}\n")
    assert "A2065 RAP Diagnostic" in diag_output
    assert "Results:" in diag_output


def test_csr0_stop(diag_output):
    assert "CSR0 = $0004" in diag_output, \
        "CSR0 not $0004 (STOP) — daemon not responding"


def test_a_write_rap_readback(diag_output):
    section = _extract_section(diag_output, "Test A")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test A: {p} passed, {f} failed")
    assert p >= 2, f"Test A needs >=2 passes, got {p}"


def test_b_rap_persistence_rdp(diag_output):
    section = _extract_section(diag_output, "Test B")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test B: {p} passed, {f} failed")
    assert p >= 2, f"Test B needs >=2 passes, got {p}"


def test_c_rap_persistence_time(diag_output):
    section = _extract_section(diag_output, "Test C")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test C: {p} passed, {f} failed")


def test_d_repeated_reads(diag_output):
    section = _extract_section(diag_output, "Test D")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test D: {p} passed, {f} failed")


def test_e_sequential(diag_output):
    section = _extract_section(diag_output, "Test E")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test E: {p} passed, {f} failed")


def test_f_raw_probe(diag_output):
    section = _extract_section(diag_output, "Test F")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test F: {p} passed, {f} failed")


def test_g_alternating(diag_output):
    section = _extract_section(diag_output, "Test G")
    p, f = _count_passes(section), _count_fails(section)
    print(f"  Test G: {p} passed, {f} failed")


def test_summary(diag_output):
    match = re.search(r"Results:\s+(\d+)\s+passed,\s+(\d+)\s+failed", diag_output)
    assert match, "No summary line found"
    p, f = int(match.group(1)), int(match.group(2))
    print(f"\n  TOTAL: {p} passed, {f} failed")
    assert p >= 3, f"Expected >=3 passes, got {p}"


def _extract_section(text, test_name):
    match = re.search(
        rf"--- {re.escape(test_name)}[^\n]*---\s*\n(.*?)(?=\n---|\n===|$)",
        text, re.DOTALL
    )
    return match.group(1) if match else ""


def _count_passes(text):
    return len(re.findall(r"^\s*PASS:", text, re.MULTILINE))


def _count_fails(text):
    return len(re.findall(r"^\s*FAIL:", text, re.MULTILINE))
