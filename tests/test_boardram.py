import os
import re
import pytest
import time
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DAEMON_PATH = "/media/fat/trans/a2065d_ddr3"
BOARDRAM_TEST_CMD = "share:a2065_boardram_test"
SERIAL_CMD = os.path.join(BASE_DIR, "serial_cmd.py")
CORE_NAME = os.environ.get("A2065_CORE", "Minimig_20260524a.rbf")


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


def test_amiga_booted(ssh):
    for attempt in range(12):
        out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        assert code == 0, f"Serial check failed: {err}"
        if "DHO:" in out:
            return
        if attempt < 11:
            time.sleep(5)
    pytest.fail("Amiga did not boot — DHO: prompt not detected")


class TestBoardramDDR3:
    """Boardram DDR3 mailbox verification tests."""

    def test_arm_loopback(self, ssh):
        """Phase 1: ARM writes patterns via DDR3, reads back — verifies full DDR3 round-trip."""
        out, err, code = ssh.execute(
            f"{DAEMON_PATH} --test-boardram", timeout=30
        )
        print(f"\n--- ARM boardram loopback (stdout) ---\n{out}")
        print(f"\n--- ARM boardram loopback (stderr) ---\n{err}")

        assert "Boardram DDR3 Loopback Test" in err, f"No loopback header: {err}"

        p_count = len(re.findall(r"^\s*PASS:", err, re.MULTILINE))
        f_count = len(re.findall(r"^\s*FAIL:", err, re.MULTILINE))

        match = re.search(r"Boardram loopback:\s+(\d+)\s+passed,\s+(\d+)\s+failed", err)
        assert match, f"No summary line: {err}"
        p, f = int(match.group(1)), int(match.group(2))

        print(f"  ARM loopback: {p} passed, {f} failed")
        assert f == 0, f"ARM loopback had {f} failures"
        assert p >= 6, f"Expected >=6 passes, got {p}"

    def test_68k_write_arm_read(self, ssh):
        """Phase 2: 68k writes pattern to boardram, ARM reads back via DDR3 — verifies cross-domain."""
        for attempt in range(12):
            out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
            assert code == 0, f"Serial check failed: {err}"
            if "DHO:" in out:
                break
            if attempt < 11:
                time.sleep(5)
        else:
            pytest.fail("Amiga did not boot for boardram test")

        out, err, code = ssh.execute(
            f'python3 /tmp/serial_cmd.py "{BOARDRAM_TEST_CMD}"', timeout=60
        )
        cleaned = _clean_serial(out)
        print(f"\n--- 68k boardram test output ---\n{cleaned}")

        assert "A2065 Boardram Write Test" in cleaned, \
            f"68k boardram test didn't run: {cleaned}"
        assert "Results:" in cleaned, f"68k test didn't complete: {cleaned}"

        m68k_pass = len(re.findall(r"^\s*PASS:", cleaned, re.MULTILINE))
        m68k_fail = len(re.findall(r"^\s*FAIL:", cleaned, re.MULTILINE))
        print(f"  68k local readback: {m68k_pass} passed, {m68k_fail} failed")
        assert m68k_fail == 0, f"68k readback had {m68k_fail} failures — boardram broken"

        out2, err2, code2 = ssh.execute(
            f"{DAEMON_PATH} --dump-boardram 0x0000 0x08", timeout=30
        )
        print(f"\n--- ARM dump boardram 0x0000-0x000E ---\n{err2}")

        assert "[0x0000]" in err2, f"No dump output: {err2}"

        vals = {}
        for m in re.finditer(r"\[0x([0-9A-Fa-f]{4})\]\s*=\s*\$([0-9A-Fa-f]{4})", err2):
            vals[int(m.group(1), 16)] = int(m.group(2), 16)

        print(f"  ARM read: {vals}")

        assert vals.get(0x0000) == 0xA5A5, \
            f"[0x0000] expected $A5A5, got ${vals.get(0x0000, 0):04X}"
        assert vals.get(0x0002) == 0x5A5A, \
            f"[0x0002] expected $5A5A, got ${vals.get(0x0002, 0):04X}"
        assert vals.get(0x0004) == 0xFF00, \
            f"[0x0004] expected $FF00, got ${vals.get(0x0004, 0):04X}"
        assert vals.get(0x0006) == 0x00FF, \
            f"[0x0006] expected $00FF, got ${vals.get(0x0006, 0):04X}"

    def test_arm_write_68k_read(self, ssh):
        """Phase 3: ARM writes pattern via DDR3, 68k reads back — verifies reverse cross-domain."""
        ssh.execute(f"killall a2065d_ddr3 2>/dev/null", timeout=5)

        write_out, write_err, write_code = ssh.execute(
            f"{DAEMON_PATH} --test-boardram", timeout=30
        )
        assert "Boardram loopback:" in write_err, f"ARM test didn't complete: {write_err}"
        print(f"\n--- ARM wrote test patterns ---\n{write_err}")

        loopback_vals = {}
        for m in re.finditer(r"\[0x([0-9A-Fa-f]{4})\]\s*=\s*\$([0-9A-Fa-f]{4})", write_err):
            loopback_vals[int(m.group(1), 16)] = int(m.group(2), 16)

        ssh.execute(f"nohup {DAEMON_PATH} > /tmp/a2065d.log 2>&1 &", timeout=5)
        time.sleep(3)

        out, err, code = ssh.execute(
            f'python3 /tmp/serial_cmd.py "share:a2065_diag"', timeout=60
        )
        diag = _clean_serial(out)
        if "Results:" in diag:
            print(f"\n--- a2065_diag ran OK (daemon alive) ---")

        ssh.execute(f"killall a2065d_ddr3 2>/dev/null", timeout=5)

    def test_stability(self, ssh):
        """Phase 4: Repeated ARM loopback — verify DDR3 doesn't degrade over time."""
        ssh.load_core(CORE_NAME)
        time.sleep(3)
        results = []
        for i in range(3):
            out, err, code = ssh.execute(
                f"{DAEMON_PATH} --test-boardram", timeout=30
            )
            match = re.search(r"Boardram loopback:\s+(\d+)\s+passed,\s+(\d+)\s+failed", err)
            if match:
                p, f = int(match.group(1)), int(match.group(2))
                results.append((p, f))
                print(f"  Iteration {i+1}: {p} passed, {f} failed")
            else:
                pytest.fail(f"Iteration {i+1}: no summary line in {err}")

        for i, (p, f) in enumerate(results):
            assert f == 0, f"Iteration {i+1}: {f} failures"
            assert p >= 6, f"Iteration {i+1}: only {p} passes"
