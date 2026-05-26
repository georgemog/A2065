import time
from mister_ssh import MiSTerSSH

BOOT_DELAY = 30
RETRY_INTERVAL = 5
MAX_RETRIES = 6


def test_amiga_booted():
    with MiSTerSSH() as ssh:
        print(f"Waiting {BOOT_DELAY}s for Amiga to boot...")
        time.sleep(BOOT_DELAY)

        sftp = ssh._client.open_sftp()
        sftp.put(
            "/Volumes/Home/nigelshearman/Development/amiga-mcp/serial_check.py",
            "/tmp/serial_check.py",
        )
        sftp.close()

        for attempt in range(1, MAX_RETRIES + 1):
            print(f"Attempt {attempt}/{MAX_RETRIES}: checking serial port...")
            out, err, code = ssh.execute("python3 /tmp/serial_check.py", timeout=30)
            assert code == 0, f"Serial check failed: {err}"
            print(f"  Response: {repr(out)}")

            if "DHO:" in out:
                print(f"[PASS] Amiga has booted — DHO: prompt detected (attempt {attempt})")
                return

            if attempt < MAX_RETRIES:
                print(f"  Not ready, retrying in {RETRY_INTERVAL}s...")
                time.sleep(RETRY_INTERVAL)

        assert False, f"Amiga did not boot after {BOOT_DELAY + MAX_RETRIES * RETRY_INTERVAL}s"


if __name__ == "__main__":
    test_amiga_booted()
