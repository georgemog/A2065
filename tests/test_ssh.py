from mister_ssh import MiSTerSSH


def test_ssh_connection():
    with MiSTerSSH() as ssh:
        assert ssh.connected, "SSH connection failed"

        stdout, stderr, code = ssh.execute("uname -a")
        assert code == 0, f"Command failed with exit code {code}: {stderr}"
        assert "MiSTer" in stdout or "Linux" in stdout, f"Unexpected output: {stdout}"
        print(f"[PASS] uname -a: {stdout}")

        stdout, _, code = ssh.execute("ls /media/fat")
        assert code == 0, f"Failed to list /media/fat: {stdout}"
        print(f"[PASS] /media/fat contents: {stdout}")

        stdout, _, code = ssh.execute("cat /etc/MiSTer")
        print(f"[PASS] MiSTer version: {stdout}" if code == 0 else f"[WARN] No version file: {stdout}")

    print("\nAll tests passed.")


if __name__ == "__main__":
    test_ssh_connection()
