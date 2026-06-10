import paramiko
from typing import Optional


class MiSTerSSH:
    def __init__(self, host: str = "192.168.1.29", username: str = "root", password: str = "1", port: int = 22):
        self.host = host
        self.username = username
        self.password = password
        self.port = port
        self._client: Optional[paramiko.SSHClient] = None

    def connect(self) -> None:
        self._client = paramiko.SSHClient()
        self._client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self._client.connect(
            hostname=self.host,
            port=self.port,
            username=self.username,
            password=self.password or None,
            timeout=10,
            look_for_keys=True,
            allow_agent=True,
        )

    def disconnect(self) -> None:
        if self._client:
            self._client.close()
            self._client = None

    @property
    def connected(self) -> bool:
        transport = self._client.get_transport() if self._client else None
        return transport is not None and transport.is_active()

    def execute(self, command: str, timeout: int = 30) -> tuple[str, str, int]:
        if not self._client:
            raise RuntimeError("Not connected. Call connect() first.")
        stdin, stdout, stderr = self._client.exec_command(command, timeout=timeout)
        exit_code = stdout.channel.recv_exit_status()
        return stdout.read().decode("utf-8", errors="replace").strip(), stderr.read().decode("utf-8", errors="replace").strip(), exit_code

    def send_mister_cmd(self, command: str) -> tuple[str, str, int]:
        return self.execute(f'echo "{command}" > /dev/MiSTer_cmd')

    def load_core(self, path: str) -> tuple[str, str, int]:
        return self.send_mister_cmd(f"load_core {path}")

    def mount(self, index: int, path: str) -> tuple[str, str, int]:
        return self.send_mister_cmd(f"mount {index} {path}")

    def umount(self, index: int) -> tuple[str, str, int]:
        return self.send_mister_cmd(f"umount {index}")

    def reset(self) -> tuple[str, str, int]:
        return self.send_mister_cmd("reset")

    def reboot(self) -> tuple[str, str, int]:
        return self.send_mister_cmd("reboot")

    def screenshot(self) -> tuple[str, str, int]:
        return self.send_mister_cmd("screenshot")

    def toggle_osd(self) -> tuple[str, str, int]:
        return self.send_mister_cmd("osd")

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
