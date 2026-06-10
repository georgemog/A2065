# Test Suite Reference

All tests run against a MiSTer FPGA running the Minimig (Amiga) core, connected via SSH and serial port.

## Test Files

### `test_ssh.py`
Basic connectivity check. Verifies SSH access to the MiSTer, confirms it's running Linux, lists `/media/fat`, and reads the MiSTer version.

### `test_load_core.py`
Loads the latest available Minimig core (sorted by date suffix) and verifies it switched from whatever was previously running. Does not run any Amiga-side checks.

### `test_amiga_boot.py`
Waits for the Amiga to boot by polling the serial port for the `DHO:` prompt. Does not load a core — expects one to already be running.

### `test_all.py`
Full system smoke test. Loads a different Minimig core from the one currently running, waits for the Amiga to boot, then runs a battery of checks:

- **test_ssh_connection** — SSH access and `/media/fat` are accessible
- **test_load_different_core** — Switches to a different core and verifies the process changed
- **test_amiga_booted** — Polls serial for the `DHO:` prompt
- **test_showconfig** — Runs `showconfig` and checks for PROCESSOR, CUSTOM CHIPS, and RAM lines
- **test_version** — Runs `version` and checks for Kickstart and Workbench strings
- **test_info** — Runs `info` and checks for mounted disks (DH0)
- **test_restart_log** — Reads `share:restart.log` and verifies its timestamp is within range of the core load time
- **test_gurus_log** — Reads `share:gurus.log`, compares against a saved baseline (`gurus.log.old`), and fails if any new guru meditation errors appeared

### `test_a2065.py`
A2065 network card register diagnostics. Loads a specific core, starts the `a2065d_ddr3` ARM-side daemon, then runs `a2065_diag` via serial. Validates individual register tests (A through G) and the overall pass/fail summary. The `load_time` fixture records when the core was loaded for restart.log cross-checking.

### `test_lance.py`
LANCE (Am7990) diagnostics. Loads the latest core, starts the `a2065d_ddr3` daemon on eth1, waits 10 seconds, then runs `share:lance-test diags` via serial with a 60-second capture window. After 60 seconds the core is restarted. Captures and prints the full diag output.

### `test_network.py`
End-to-end A2065 networking test. Loads a core, starts the ARM daemon, runs register sanity checks via `a2065_diag`, then attempts to configure the network interface on the Amiga side using `AddNetInterface`. Checks DHCP/configuration status and runs `arp -a` and `ShowNetStatus`. Cleans up the daemon process on completion.

### `test_boardram.py`
DDR3 board RAM mailbox verification across the ARM/68k boundary. Runs in four phases:

1. **ARM loopback** — ARM writes patterns to boardram via DDR3 and reads them back
2. **68k write, ARM read** — 68k writes known patterns, ARM reads them back via DDR3 to verify cross-domain integrity
3. **ARM write, 68k read** — ARM writes via DDR3, 68k reads back to verify reverse direction
4. **Stability** — Repeated ARM loopback runs to verify DDR3 doesn't degrade over time

## Support Scripts

| File | Purpose |
|------|---------|
| `mister_ssh.py` | SSH client wrapper (`MiSTerSSH` class) with helpers for `load_core`, `mount`, `reset`, etc. |
| `serial_cmd.py` | Sends a command over `/dev/ttyS1` (115200 baud) and captures ~5s of output |
| `serial_long.py` | Same as `serial_cmd.py` but with a configurable wait time (default 30s) for long-running commands |
| `serial_check.py` | Minimal serial poll — sends empty line and reads response |

## Environment Variables

| Variable | Used by | Purpose |
|----------|---------|---------|
| `A2065_CORE` | `test_a2065.py`, `test_network.py`, `test_boardram.py` | Override which Minimig core `.rbf` to load (defaults are pinned per file) |
