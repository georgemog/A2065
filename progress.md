# A2065 Progress Log

## Session: May 25-26, 2026 — Step 9 Integration + Step 10 lance-test Diags

### Summary

Full MiSTer integration verified. DDR3 mailbox stable, interrupt generation working. Ran Commodore lance-test diagnostics 6 times — Buffer Memory and LANCE Configuration always PASS. Two remaining issues: intermittent interrupt test failures (timing race) and RX buffer exhaustion during collision/loopback test.

### MiSTer Verification Results (build 20260525a)

| Test | Result |
|------|--------|
| Register read/write (17 tests) | PASS |
| Boardram read/write (5 tests) | PASS |
| Interrupt assert/deassert/lifecycle (8 tests) | PASS |
| DDR3 stability (>60s idle) | PASS |

### lance-test Diags (6 runs)

| Test | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Run 6 |
|------|:-----:|:-----:|:-----:|:-----:|:-----:|:-----:|
| Buffer memory | PASS | PASS | PASS | PASS | PASS | PASS |
| LANCE config | PASS | PASS | PASS | PASS | PASS | PASS |
| Interrupt | PASS | **FAIL** | PASS | **FAIL** | PASS | PASS |
| Collision logic | **FAIL** | — | **FAIL** | — | **FAIL** | **FAIL** |

**Failure mode 1 — Interrupt test (runs 2, 4):** Intermittent. Amiga writes INEA (0x4000) expecting CSR0=0x01C1 (IDON+INIT+INEA+INTR). Sometimes gets 0x0001 (no INTR) or 0x0004 (STOP). Likely timing race in CSR0 flag propagation.

**Failure mode 2 — Collision logic test (runs 1, 3, 5, 6):** Test sends TX packets (60 bytes). After ~12-16 successful RX, the 16-entry RX ring exhausts and floods with "RX buffer error". Live network traffic on eth1 fills RX buffers faster than test reaps them. Collision test cannot complete.

**MAC address:** Shows `00:FFFFFF80:10:70:70:70` in all runs — known byte ordering bug in init block readback (hardcoded serial bytes 0x02/0x70/0x70/0x70 in cpu_wrapper.v).

### Key Fixes This Session

1. **Deadlock fix (service_bridge):** ARM now writes REG_RSP *before* calling chip_wput(). Previously chip_wput() → chip_init() → boardram access → DDR3 RAM mailbox → FPGA stuck waiting for REG_RSP = deadlock.

2. **Stale FPGA state fix:** On startup, write fake REG_RSP (0x1) to unstick FPGA left in register path by previous daemon. Eliminates core reload between restarts.

3. **MAC address fix:** Moved daemon_set_default_mac() to after ethernet_open(). Was all-zero before.

4. **Interrupt generation:** ARM writes MBX_INT every main loop iteration based on CSR0_INTR && CSR0_INEA. FPGA polls every 256 cycles. 2-stage CDC synchronizer (clk_audio→clk_sys) into Paula PORTS (INT2).

### Remaining Issues

- **MAC byte ordering:** Init block readback shows wrong bytes. Serial bytes in cpu_wrapper.v hardcoded as 0x02/0x70/0x70/0x70.
- **Interrupt timing race:** 2/6 runs fail interrupt test. CSR0 flag propagation timing needs investigation.
- **RX buffer exhaustion:** 16-entry RX ring too small for live network traffic during collision test. Need faster reaping, larger ring, or traffic filtering.
- **cpu_berr_n** not connected — watchdog returns $0000 instead of BERR.

---

## Session: May 3-4, 2026 — DDR3 Mailbox Bring-up

## Summary

Brought up the DDR3 shared-memory mailbox path between the FPGA (register bridge) and ARM daemon for A2065 Ethernet emulation. After systematic debugging through **8 FPGA builds**, the full FPGA-side path is now working end-to-end. The ARM daemon round-trip is the final test.

---

## Builds This Session

| Build | Date | Changes | Result | Timing |
|-------|------|---------|--------|--------|
| 20260503b | May 3 | Reverted `{cpu_address_out, 1'b0}` (byte address fix) | SUCCESS | +0.404ns setup |
| 20260503c | May 3 | Connected `regs_nrdy` to bridge nrdy, fixed deadlock | SUCCESS | -0.272ns setup (HDMI PLL, emu OK at +0.195ns) |
| 20260503d | May 3 | Added $DEAD watchdog diagnostic, register offset 0x4000 | SUCCESS | +0.245ns setup |
| 20260503e | May 3 | Arbiter fairness fix (inverted last_grant check) | SUCCESS | +0.177ns setup |
| 20260503f | May 3 | Combinatorial arbiter grant (no registration delay) | SUCCESS | +0.239ns setup, +0.025ns hold |
| 20260503g | May 3 | Mailbox bypass test — return $BEEF without DDR3 | SUCCESS | +0.332ns setup |
| 20260503h | May 3 | DDR3 write-only test — return $CAFE after write accepted | SUCCESS | +0.326ns setup |
| 20260504 | May 4 | **Full fix**: arbiter read burst tracking + mailbox avl_read hold | SUCCESS | +0.270ns setup |

---

## Issues Found and Fixed

### 1. Amiga Test Program Used Wrong Register Offsets
- **Symptom**: All reads returned $0000, no bus errors
- **Root cause**: Test accessed $EA0000/$EA0002 but real A2065 registers are at offset $4000 ($EA4000/$EA4002)
- **WinUAE reference**: `A2065_CHIP_OFFSET = 0x4000`, RAP=$4002, RDP=$4000
- **Fix**: Updated test program to use `cd->cd_BoardAddr + 0x4002` and `+ 0x4000`

### 2. Register Address Input Format
- **Symptom**: Register module sel_chipreg never fired at offset $4000
- **Root cause**: `cpu_address_out` is `wire [23:1]` (word address, 23 bits). Connecting it directly to the 24-bit `cpu_addr` port zero-padded the MSB, corrupting address decode
- **Fix**: Reverted to `{cpu_address_out, 1'b0}` which reconstructs the 24-bit byte address

### 3. Register Offset Changed Incorrectly
- **Symptom**: Address decode matched but registers at wrong offset
- **Root cause**: Changed decode from `cpu_addr[15:2] == 14'h1000` (offset $4000) to `cpu_addr[15:4] == 12'h000` (offset $0000) based on wrong assumption
- **Fix**: Reverted to `14'h1000` matching real A2065 hardware (offset $4000)

### 4. `regs_nrdy` Not Connected
- **Symptom**: Register module triggered but CPU never waited for ARM response
- **Root cause**: Line 961 had `.regs_nrdy()` — output not connected to anything
- **Fix**: Declared `wire regs_nrdy`, connected to register module output, OR'd into bridge module's `nrdy` input: `.nrdy((gayle_nrdy & rd_cyc) | regs_nrdy)`

### 5. `regs_nrdy` Deadlock
- **Symptom**: Would have caused CPU to hang (DTACK never asserted)
- **Root cause**: `regs_nrdy = sel_chipreg || (state != ST_IDLE)` — stays high in ST_RELEASE waiting for AS to drop, but CPU won't drop AS until DTACK asserted
- **Fix**: Changed to `((state == ST_WAIT_ARM) || (sel_chipreg && state == ST_IDLE))` — only holds DTACK during the bridge request, releases immediately when data is ready

### 6. ARM Daemon Bit Extraction Mismatch
- **Symptom**: Daemon would read wrong addr/data from DDR3 mailbox
- **Root cause**: ARM used `addr = (req >> 2) & 0xFFFF` (bits [17:2]) but FPGA packs addr at bits [9:2]
- **Fix**: Changed ARM to `addr = (req >> 2) & 0xFF` (bits [9:2]) and `data = (req >> 10) & 0xFFFF` (bits [25:10])

### 7. Avalon Arbiter Starvation (Registered Grant)
- **Symptom**: DDR3 mailbox adapter (m1) never got arbiter access
- **Root cause**: `grant` was registered (1-cycle delay). When m0 burst completed, new grant was scheduled but m0 started a new burst using the old grant value in the same cycle
- **Fix**: Made `grant` combinatorial: `assign grant = burst_active ? last_grant : ...`

### 8. Avalon Arbiter Read Burst Tracking (CRITICAL)
- **Symptom**: DDR3 writes worked ($CAFE test) but reads never completed
- **Root cause**: For burst=1 reads, `burst_remaining` was initialized to `burstcount - 1 = 0`. The arbiter immediately cleared `burst_active`, switching the grant away before `readdatavalid` arrived (10-20 cycles later). The response was routed to the wrong master.
- **Fix**: Separated read/write burst tracking with `burst_is_read` flag. Reads count down on `s_readdatavalid`, writes count down on write acceptance. `burst_count` initialized to full `burstcount` (not `burstcount - 1`). Burst completes when `burst_count == 1` at the time of the last event.

### 9. Mailbox Adapter avl_read Not Held (CRITICAL)
- **Symptom**: DDR3 read commands never issued to arbiter
- **Root cause**: `S_POLL_ISS` set `avl_read <= 1` (non-blocking, 1 cycle). `S_POLL_W` had no `avl_read` assignment, so default `avl_read <= 0` applied. The Avalon protocol requires keeping `read` asserted until `waitrequest` goes low.
- **Fix**: In `S_POLL_W`, keep `avl_read <= 1` when reissuing the poll (response not ready yet) or waiting for readdata. The read stays asserted across poll retries.

---

## Diagnostic Methodology

Used a systematic binary-search approach:

1. **$DEAD diagnostic**: Made watchdog timeout return 0xDEAD instead of 0x0000. Distinguished "register module triggered" from "open bus read"
2. **$BEEF bypass**: Removed DDR3 entirely, mailbox adapter returned hardcoded value. Confirmed CDC + handshake path works
3. **$CAFE write test**: Restored DDR3 write but skipped read/poll. Confirmed arbiter grants + f2sdram2 accepts writes
4. Gap identified: DDR3 reads fail — found arbiter burst tracking and avl_read holding bugs

---

## Current Status

| Component | Status |
|-----------|--------|
| FPGA autoconfig | ✅ Working (A2065 detected at $EA0000) |
| FPGA register decode (offset $4000) | ✅ Working ($DEAD confirmed) |
| FPGA DTACK hold (regs_nrdy) | ✅ Working |
| CDC (clk_sys → clk_audio) | ✅ Working ($BEEF confirmed) |
| DDR3 write path (FPGA → DDR3) | ✅ Working ($CAFE confirmed) |
| DDR3 read path (FPGA ← DDR3) | ✅ Fixed (arbiter + mailbox) — needs verification |
| ARM daemon DDR3 polling | ✅ Deployed (bridge_ddr3.cpp) |
| Full round-trip (Amiga → DDR3 → ARM → DDR3 → Amiga) | ⏳ Pending test |
| LANCE CSR emulation in ARM | 🔲 TODO |
| Boardram ARM access via DDR3 | 🔲 TODO |
| A2065 AmigaOS driver test | 🔲 TODO |

---

## Key Lessons

1. **Address formats matter**: Minimig uses `wire [23:1]` for word addresses. Always reconstruct byte addresses with `{addr, 1'b0}` when connecting to byte-addressed modules.

2. **Real hardware register maps**: Always verify against real hardware or emulator source. The A2065 has registers at offset $4000, not $0000.

3. **Connect all outputs**: An unconnected `output` in Verilog is silently ignored. `regs_nrdy` was declared but not wired — the register module was invisible to the DTACK path.

4. **Avalon burst tracking**: Reads and writes have different completion semantics. Writes complete when `!waitrequest`, reads complete when `readdatavalid`. The arbiter must track these separately.

5. **Avalon read protocol**: Masters must hold `read` asserted until `!waitrequest`. A single-cycle read pulse is insufficient if the slave (or arbiter) has waitstates.

6. **Combinatorial vs registered arbitration**: Grant decisions must be combinatorial to avoid single-cycle gaps where the wrong master can steal a burst.

7. **Incremental diagnostics**: Use hardcoded return values ($DEAD, $BEEF, $CAFE) to isolate which stage of a multi-stage path fails.
