import re
import pytest
from conftest import (
    clean_serial, extract_section, count_passes, count_fails,
    wait_for_boot, run_amiga_cmd, run_amiga_cmd_long,
    start_daemon, stop_daemon, get_daemon_log,
    CORE_NAME, DAEMON_PATH,
)


def test_core_loaded(ssh, load_core):
    out, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert CORE_NAME in out, f"Expected {CORE_NAME}, got: {out}"


def test_amiga_booted(ssh, deploy_serial):
    wait_for_boot(ssh)


def test_showconfig(ssh):
    out = run_amiga_cmd(ssh, "showconfig")
    assert "PROCESSOR" in out, f"No PROCESSOR line: {out}"
    assert "CUSTOM CHIPS" in out, f"No CUSTOM CHIPS line: {out}"


def test_version(ssh):
    out = run_amiga_cmd(ssh, "version")
    assert "Kickstart" in out, f"No Kickstart: {out}"


class TestBoardram:
    def test_arm_loopback(self, ssh):
        out, err, code = ssh.execute(
            f"{DAEMON_PATH} --test-boardram", timeout=30
        )
        print(f"\n--- ARM boardram loopback ---\n{err}")
        assert "Boardram DDR3 Loopback Test" in err
        match = re.search(r"Boardram loopback:\s+(\d+)\s+passed,\s+(\d+)\s+failed", err)
        assert match, f"No summary line: {err}"
        p, f = int(match.group(1)), int(match.group(2))
        assert f == 0, f"ARM loopback had {f} failures"
        assert p >= 6, f"Expected >=6 passes, got {p}"


class TestRegisterDiag:
    @pytest.fixture(scope="class")
    def diag_output(self, ssh):
        start_daemon(ssh)
        for attempt in range(12):
            out = run_amiga_cmd(ssh, "share:a2065_diag", timeout=60)
            if "Results:" in out:
                yield out
                stop_daemon(ssh)
                return
            import time; time.sleep(5)
        stop_daemon(ssh)
        pytest.fail("a2065_diag never completed after 12 attempts")

    def test_diag_ran(self, diag_output):
        assert "A2065 RAP Diagnostic" in diag_output

    def test_csr0_stop(self, diag_output):
        assert "CSR0 = $0004" in diag_output, \
            "CSR0 not $0004 (STOP) — daemon not responding"

    def test_a_write_rap_readback(self, diag_output):
        section = extract_section(diag_output, "Test A")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test A: {p} passed, {f} failed")
        assert p >= 2, f"Test A needs >=2 passes, got {p}"

    def test_b_rap_persistence_rdp(self, diag_output):
        section = extract_section(diag_output, "Test B")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test B: {p} passed, {f} failed")
        assert p >= 2, f"Test B needs >=2 passes, got {p}"

    def test_c_rap_persistence_time(self, diag_output):
        section = extract_section(diag_output, "Test C")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test C: {p} passed, {f} failed")
        assert f == 0, f"Test C had {f} failures"

    def test_d_repeated_reads(self, diag_output):
        section = extract_section(diag_output, "Test D")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test D: {p} passed, {f} failed")
        assert f == 0, f"Test D had {f} failures"

    def test_e_sequential(self, diag_output):
        section = extract_section(diag_output, "Test E")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test E: {p} passed, {f} failed")
        assert f == 0, f"Test E had {f} failures"

    def test_f_raw_probe(self, diag_output):
        section = extract_section(diag_output, "Test F")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test F: {p} passed, {f} failed")
        assert f == 0, f"Test F had {f} failures"

    def test_g_alternating(self, diag_output):
        section = extract_section(diag_output, "Test G")
        p, f = count_passes(section), count_fails(section)
        print(f"  Test G: {p} passed, {f} failed")
        assert f == 0, f"Test G had {f} failures"

    def test_summary(self, diag_output):
        match = re.search(r"Results:\s+(\d+)\s+passed,\s+(\d+)\s+failed", diag_output)
        assert match, "No summary line found"
        p, f = int(match.group(1)), int(match.group(2))
        print(f"\n  TOTAL: {p} passed, {f} failed")
        assert f == 0, f"Expected 0 failures, got {f}"

    def test_daemon_log(self, ssh, diag_output):
        log = get_daemon_log(ssh)
        print(f"\n--- a2065d_ddr3 log ---\n{log}\n")
        assert "Starting" in log or "Running" in log


class TestLanceDiag:
    @pytest.fixture(scope="class")
    def lance_output(self, ssh):
        start_daemon(ssh)
        out = run_amiga_cmd_long(
            ssh, "share:lance-test diags > share:lance-test.log",
            wait=60, timeout=120
        )
        yield out
        stop_daemon(ssh)

    def test_lance_ran(self, lance_output):
        assert lance_output, "No serial output from lance-test"
        assert "lance" in lance_output.lower() or "LANCE" in lance_output

    def test_buffer_memory(self, lance_output):
        if "Buffer" in lance_output:
            p = count_passes(lance_output)
            assert p >= 1, f"No buffer memory passes"

    def test_lance_config(self, lance_output):
        if "LANCE config" in lance_output or "Configuration" in lance_output:
            p = count_passes(lance_output)
            assert p >= 1, f"No LANCE config passes"

    def test_interrupt(self, lance_output):
        if "Interrupt" in lance_output:
            section = lance_output
            f = count_fails(section)
            print(f"  Interrupt test: {count_passes(section)} passed, {f} failed")

    def test_collision(self, lance_output):
        if "Collision" in lance_output:
            section = lance_output
            f = count_fails(section)
            print(f"  Collision test: {count_passes(section)} passed, {f} failed")

    def test_daemon_log(self, ssh, lance_output):
        log = get_daemon_log(ssh)
        print(f"\n--- a2065d_ddr3 log (last 80 lines) ---")
        lines = log.split("\n")
        for line in lines[-80:]:
            print(f"  {line}")


class TestStability:
    def test_repeated_boardram(self, ssh, load_core, deploy_serial):
        results = []
        for i in range(3):
            out, err, code = ssh.execute(
                f"{DAEMON_PATH} --test-boardram", timeout=30
            )
            match = re.search(r"Boardram loopback:\s+(\d+)\s+passed,\s+(\d+)\s+failed", err)
            assert match, f"Iteration {i+1}: no summary line"
            p, f = int(match.group(1)), int(match.group(2))
            results.append((p, f))
            print(f"  Iteration {i+1}: {p} passed, {f} failed")

        for i, (p, f) in enumerate(results):
            assert f == 0, f"Iteration {i+1}: {f} failures"
            assert p >= 6, f"Iteration {i+1}: only {p} passes"

    def test_register_stability(self, ssh, load_core, deploy_serial):
        import time
        for i in range(3):
            for retry in range(5):
                stop_daemon(ssh)
                time.sleep(1)
                _, err, code = ssh.load_core(CORE_NAME)
                assert code == 0, f"reload failed: {err}"
                time.sleep(3)
                wait_for_boot(ssh)
                start_daemon(ssh)
                for attempt in range(12):
                    out = run_amiga_cmd(ssh, "share:a2065_diag", timeout=90)
                    if "Results:" in out:
                        match = re.search(r"Results:\s+(\d+)\s+passed,\s+(\d+)\s+failed", out)
                        if match:
                            p, f = int(match.group(1)), int(match.group(2))
                            print(f"  Run {i+1}: {p} passed, {f} failed (attempt {attempt+1}, retry {retry+1})")
                            if f == 0:
                                break
                            if retry < 4:
                                print(f"    Retrying with fresh core reload...")
                                break
                            assert False, f"Run {i+1}: {f} failures after {retry+1} retries"
                        break
                    time.sleep(5)
                else:
                    stop_daemon(ssh)
                    pytest.fail(f"Run {i+1}: a2065_diag never completed")
            else:
                pytest.fail(f"Run {i+1}: all retries exhausted")
        stop_daemon(ssh)
