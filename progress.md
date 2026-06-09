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

## Session: June 6-9, 2026 — Doorbell architecture: full lance-test pass + real networking

### Summary

Rebuilt the bridge as the **doorbell architecture** (branch `simplification/flat-ddr3-doorbell`): zero-latency CSR reads from a DDR3 shadow + a flat DDR3 boardram window, replacing the per-access DTACK-stretch mailbox. Root-caused and fixed every remaining failure. lance-test now passes **all five tests** ("Controller PASSED diagnostics"), **100/100** over a stress run, and a real AmigaOS TCP/IP stack gets a **DHCP lease** over the card.

### Fixes (all hardware-verified, committed)

| Fix | Build / commit | Result |
|-----|----------------|--------|
| Boardram 68k capture on raw `sel_br` (pre-strobe) → qualify with AS+DS + `cpu_rw` | 20260608h / `be20f41` | Buffer test PASS |
| Partial DDR3 writes unproven → read-modify-write | 20260608f | (folded in) |
| INIT RAP/RDP sequence lost (no back-pressure) → end-to-end drain handshake | 20260608i / `be20f41` | Config + IDON + Interrupt PASS |
| Word-granular RMW clobbered byte writes → forward UDS/LDS byte-enables | 20260609a / `ff20394` | memtest 10/10 |
| Daemon read 68k boardram byte-swapped → XOR byte offset in flat accessors | daemon `300c1a2` | Collision PASS (5/5) |
| Daemon busy-spin pegged a core → adaptive poll backoff + `--debug` flag | daemon `ba7b5ac` | idle CPU ~100%→~2-9% |

### Verification

| Test | Result |
|------|--------|
| lance-test diags (Buffer/Config/Interrupt/Collision/Internal-loopback) | **5/5 PASS** |
| lance-test **x100** (single persistent daemon) | **100/100, 100% each** |
| share:a2065_memtest | **10/10 PASS** |
| External loopback (real eth1 frame round-trip) | **PASS** |
| AddNetInterface A2065 → DHCP | **lease 192.168.1.190, route, DNS** |
| Daemon CPU under load / mem | 2-7% / ~2% (11.7 MB), no leak |

Surpasses the old-bridge baseline (3/4) on a simpler architecture. Steps 0-11 complete. Only the MAC-display cosmetic (FPGA station-address bytes unwired, issue #3) remains — left as-is, doesn't affect networking.

### Big gotchas this session

- **Capture must gate on the data strobe, not the address decode.** Raw `sel_br` is high during the 68k address phase before write data/direction are valid. AS+DS qualification (like the regfile) fixed it; earlier `cpu_rw` attempts failed only because they kept raw `sel_br`.
- **68k big-endian vs ARM little-endian DDR3 lane.** The daemon must XOR the flat byte offset with 1 to read 68k-written init blocks/descriptors. memtest/buffer pass without it (68k-self-consistent), masking the bug — only cross-domain (daemon parsing 68k data) breaks.
- **Stale duplicate header on the build host.** A `src/boardram_access.h` existed only on 192.168.1.97 (not the repo) and shadowed `include/` (same-dir `#include` wins over `-I include`). Header edits silently had zero effect (md5 unchanged). Cost ~5 build cycles. Verify the active header with `g++ -I include -E src/<f>.cpp | grep <sym>`.

---

## Current Status

_Doorbell architecture, branch `simplification/flat-ddr3-doorbell` (RBF `Minimig_20260609a.rbf`, daemon `a2065d_doorbell` commit `ba7b5ac`)._

| Component | Status |
|-----------|--------|
| FPGA autoconfig (A2065 at $EA0000) | ✅ Working |
| FPGA CSR regfile (zero-latency reads + doorbell) | ✅ Working |
| FPGA flat DDR3 boardram window (word + byte, AS+DS capture, byte-enable RMW) | ✅ Working |
| INIT register-sequence back-pressure | ✅ Working |
| ARM daemon: CSR emulation, ring walker, raw-socket ethernet | ✅ Working |
| Daemon boardram byte-order (XOR-1 flat accessors) | ✅ Fixed |
| Daemon efficiency (adaptive backoff) + `--debug` flag | ✅ Working (idle ~2-9%) |
| lance-test diags 5/5 + x100 100/100 + memtest 10/10 | ✅ PASS |
| External loopback + AddNetInterface DHCP | ✅ PASS |
| MAC display (FPGA station-address bytes unwired) | 🔲 Cosmetic — left as-is (issue #3) |
| `cpu_berr_n` (watchdog → BERR vs $0000) | 🔲 TODO (low) |

---

## Key Lessons

1. **Address formats matter**: Minimig uses `wire [23:1]` for word addresses. Always reconstruct byte addresses with `{addr, 1'b0}` when connecting to byte-addressed modules.

2. **Real hardware register maps**: Always verify against real hardware or emulator source. The A2065 has registers at offset $4000, not $0000.

3. **Connect all outputs**: An unconnected `output` in Verilog is silently ignored. `regs_nrdy` was declared but not wired — the register module was invisible to the DTACK path.

4. **Avalon burst tracking**: Reads and writes have different completion semantics. Writes complete when `!waitrequest`, reads complete when `readdatavalid`. The arbiter must track these separately.

5. **Avalon read protocol**: Masters must hold `read` asserted until `!waitrequest`. A single-cycle read pulse is insufficient if the slave (or arbiter) has waitstates.

6. **Combinatorial vs registered arbitration**: Grant decisions must be combinatorial to avoid single-cycle gaps where the wrong master can steal a burst.

7. **Incremental diagnostics**: Use hardcoded return values ($DEAD, $BEEF, $CAFE) to isolate which stage of a multi-stage path fails.

8. **Gate bus capture on the data strobe, not the address decode**: a raw chip-select is high during the 68k address phase, before write data and R/W are valid. Qualify request capture with `!AS && !DS` (and sample `cpu_rw` then). DS also deasserts between cycles, giving clean access separation without edge detection.

9. **Endianness bites only cross-domain**: 68k (big-endian) words land in the DDR3 lane little-endian. 68k-only tests (memtest, buffer) are self-consistent and pass, hiding the swap; it only breaks when the little-endian daemon parses 68k-written structures (init block, descriptors). Fix in the daemon: XOR the byte offset with 1.

10. **A handshake doorbell needs end-to-end drain confirmation**: a single no-back-pressure CMD slot silently drops writes when the producer outruns the consumer (lost the rapid LANCE INIT RAP/RDP sequence). Stretch the producer (DTACK) until the consumer has actually drained — not just until the value was posted.

11. **The build host is rsync'd, not the repo**: a stale duplicate header (`src/boardram_access.h`) existed only on the build machine and shadowed `include/` (same-dir `#include` wins over `-I`). Edits had zero effect, md5 unchanged. When a change "does nothing," preprocess (`g++ -E`) to see what the compiler actually used.
