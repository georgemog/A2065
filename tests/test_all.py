import os
import re
import pytest
import time
from datetime import datetime
from mister_ssh import MiSTerSSH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
GURUS_OLD_PATH = os.path.join(BASE_DIR, "gurus.log.old")


@pytest.fixture(scope="session")
def ssh():
    with MiSTerSSH() as client:
        yield client


@pytest.fixture(scope="session")
def serial_script():
    return os.path.join(BASE_DIR, "serial_cmd.py")


@pytest.fixture(scope="session")
def load_time():
    return {"value": None}


def _clean_serial(out):
    out = re.sub(r"\d+h", "", out)
    return re.sub(r"[\x9b\x0f]|\x1b\[[^@-~]*[@-~]", "", out)


def _get_gurus_log(ssh, serial_script):
    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()
    out, err, code = ssh.execute('python3 /tmp/serial_cmd.py "type share:gurus.log"', timeout=30)
    assert code == 0, f"type share:gurus.log failed: {err}"
    return _clean_serial(out)


def test_ssh_connection(ssh):
    stdout, _, code = ssh.execute("uname -a")
    assert code == 0
    assert "Linux" in stdout

    stdout, _, code = ssh.execute("ls /media/fat")
    assert code == 0


def test_load_different_core(ssh, load_time):
    stdout, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    assert stdout, "MiSTer not running"
    current = stdout.split()[-1]

    stdout, _, _ = ssh.execute("ls -1 /media/fat/Minimig_*.rbf")
    cores = stdout.splitlines()
    assert len(cores) >= 2, "Need at least 2 cores"

    target = None
    for core in cores:
        name = core.split("/")[-1]
        if f"/media/fat/{name}" != current:
            target = name
            break
    assert target, "No different core to load"

    _, stderr, code = ssh.load_core(target)
    assert code == 0, f"load_core failed: {stderr}"

    load_time["value"] = datetime.now()

    time.sleep(5)

    stdout, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    loaded = stdout.split()[-1]
    assert loaded == f"/media/fat/{target}"


def test_amiga_booted(ssh, serial_script):
    time.sleep(30)

    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()

    for attempt in range(1, 7):
        out, err, code = ssh.execute("python3 /tmp/serial_cmd.py", timeout=30)
        assert code == 0, f"Serial check failed: {err}"

        if "DHO:" in out:
            return

        if attempt < 6:
            time.sleep(5)

    pytest.fail("Amiga did not boot — DHO: prompt not detected on serial port")


def test_showconfig(ssh, serial_script):
    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()

    out, err, code = ssh.execute("python3 /tmp/serial_cmd.py showconfig", timeout=30)
    assert code == 0, f"showconfig failed: {err}"
    assert "PROCESSOR" in out, f"No PROCESSOR line in output: {out}"
    assert "CUSTOM CHIPS" in out, f"No CUSTOM CHIPS line in output: {out}"
    assert "RAM" in out, f"No RAM line in output: {out}"

    print(f"Amiga hardware config:\n{out}")


def test_version(ssh, serial_script):
    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()

    out, err, code = ssh.execute("python3 /tmp/serial_cmd.py version", timeout=30)
    assert code == 0, f"version failed: {err}"
    assert "Kickstart" in out, f"No Kickstart in output: {out}"
    assert "Workbench" in out, f"No Workbench in output: {out}"

    print(f"Amiga version:\n{out}")


def test_info(ssh, serial_script):
    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()

    out, err, code = ssh.execute("python3 /tmp/serial_cmd.py info", timeout=30)
    assert code == 0, f"info failed: {err}"
    assert "Mounted disks" in out, f"No Mounted disks in output: {out}"
    assert "DH0" in out, f"No DH0 in output: {out}"

    print(f"Amiga disk info:\n{out}")


def test_restart_log(ssh, serial_script, load_time):
    assert load_time["value"] is not None, "load_time not captured — test_load_different_core must run first"

    sftp = ssh._client.open_sftp()
    sftp.put(serial_script, "/tmp/serial_cmd.py")
    sftp.close()

    out, err, code = ssh.execute('python3 /tmp/serial_cmd.py "type share:restart.log"', timeout=30)
    assert code == 0, f"type share:restart.log failed: {err}"

    out_clean = _clean_serial(out)
    match = re.search(r"((?:Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday) \d+-\w+-\d+ \d+:\d+:\d+)", out_clean)
    assert match, f"No timestamp found in restart.log: {out}"

    restart_str = match.group(1)
    restart_time = datetime.strptime(restart_str, "%A %d-%b-%y %H:%M:%S")
    load_t = load_time["value"]
    diff = (restart_time - load_t).total_seconds()

    print(f"load_core sent at:  {load_t.strftime('%H:%M:%S')}")
    print(f"restart.log time:   {restart_str} ({restart_time.strftime('%H:%M:%S')})")
    print(f"Difference:         {diff:.0f}s")

    assert -120 <= diff <= 60, f"Restart time {restart_str} not within expected range of load_core time {load_t} (diff={diff:.0f}s). restart.log may not have updated if same core was already loaded."


def test_gurus_log(ssh, serial_script):
    new_log = _get_gurus_log(ssh, serial_script)
    new_entries = _parse_entries(new_log)

    if os.path.exists(GURUS_OLD_PATH):
        with open(GURUS_OLD_PATH, "r") as f:
            old_entries = _parse_entries(f.read())
    else:
        print("No previous gurus.log.old found — saving current as baseline")
        with open(GURUS_OLD_PATH, "w") as f:
            f.write(new_log)
        print(f"[PASS] Baseline saved ({len(new_entries)} entries)")
        return

    added = _get_new_entries(old_entries, new_entries)

    if not added:
        print(f"[PASS] No new guru meditation errors ({len(new_entries)} entries unchanged)")
        return

    print(f"[FAIL] {len(added)} new guru meditation error(s) detected:\n")
    for entry in added:
        print(entry)
        print()

    with open(GURUS_OLD_PATH, "w") as f:
        f.write(new_log)
    print(f"gurus.log.old updated ({len(new_entries)} total entries)")

    pytest.fail(f"{len(added)} new guru meditation error(s) found")


def _parse_entries(log):
    entries = []
    for block in re.split(r"={10,}", log):
        block = block.strip()
        if "DEADEND" in block:
            normalized = re.sub(r"[\x9b\x0f\r]|\x1b\[[^@-~]*[@-~]", "", block)
            normalized = re.sub(r"\s+", " ", normalized).strip()
            entries.append(normalized)
    return entries


def _count_entries(log):
    return log.count("DEADEND")


def _get_new_entries(old_entries, new_entries):
    old_set = set(old_entries)
    return [e for e in new_entries if e not in old_set]
