import re
import time
from mister_ssh import MiSTerSSH


def get_loaded_core(ssh: MiSTerSSH) -> str:
    stdout, _, _ = ssh.execute("ps -ef | grep MiSTer | grep -v grep")
    parts = stdout.split()
    return parts[-1] if parts else ""


def test_load_different_core():
    with MiSTerSSH() as ssh:
        stdout, _, code = ssh.execute("ls -1 /media/fat/Minimig_*.rbf")
        assert code == 0, f"Failed to list cores: {stdout}"
        cores = stdout.splitlines()
        assert len(cores) >= 2, "Need at least 2 cores to test switching"

        def sort_key(name):
            m = re.search(r"(\d{8})", name)
            suffix = re.search(r"(\d{8})(.*)\.rbf", name)
            date = int(m.group(1)) if m else 0
            tag = suffix.group(2) if suffix else ""
            return (date, tag)

        cores_sorted = sorted(cores, key=sort_key)

        current = get_loaded_core(ssh)
        print(f"Currently loaded: {current}")

        target = None
        for core in reversed(cores_sorted):
            core_path = f"/media/fat/{core.split('/')[-1]}"
            if core_path != current:
                target = core
                break
        assert target, "No different core available to load"
        core_name = target.split("/")[-1]
        print(f"Loading different core: {core_name}")

        stdout, stderr, code = ssh.load_core(core_name)
        assert code == 0, f"load_core failed: {stderr}"

        time.sleep(5)

        loaded = get_loaded_core(ssh)
        expected = f"/media/fat/{core_name}"
        assert loaded == expected, f"Expected {expected}, got {loaded}"
        print(f"[PASS] Core switched: {current} -> {loaded}")


if __name__ == "__main__":
    test_load_different_core()
