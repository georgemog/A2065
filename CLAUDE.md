# A2065 Project Memory

## Project Overview

Full hardware emulation of the Commodore A2065 ZorroII Ethernet card for the Minimig FPGA core on MiSTer (DE10-Nano, Cyclone V). Uses standard AmigaOS A2065 drivers — no custom Amiga-side software. Port of Amiberry's `a2065.cpp` (Toni Wilen, 2009) from emulator-space to real hardware.

**Target hardware:** AMD Am7990 LANCE Ethernet controller, Commodore ZorroII card (64KB address space, 32KB boardram, manufacturer ID 0x0202, product ID 0x70, MAC OUI 00:80:10).

## Architecture Split

- **FPGA fabric:** Autoconfig, 32KB dual-port BRAM (68k + ARM via DDR3 mailbox), register bridge (DTACK-stretch with ARM daemon via DDR3 mailbox), interrupt generation (INT2 via DDR3 mailbox)
- **ARM Linux daemon:** Am7990 CSR state machine, TX/RX ring walker, raw Ethernet socket (AF_PACKET), boardram access via DDR3 mailbox, interrupt state management
- **Bridge:** DDR3 shared-memory mailbox via f2sdram2 Avalon port (HPS2FPGA AXI bridge abandoned — non-functional on MiSTer)

## Remote Build Environment

- **Dev machine:** `nigelshearman@local` (macOS, working directory `/Volumes/Home/nigelshearman/Development/amiga/A2065`)
- **Quartus build host:** `nshearman@192.168.1.63` (Linux, Quartus 17.0 installed)
  - Source: `~/Development/Minimig-AGA_MiSTer/`
  - Quartus: `/opt/altera/17.0/quartus/bin/quartus_sh`
  - Build: `cd ~/Development/Minimig-AGA_MiSTer && /opt/altera/17.0/quartus/bin/quartus_sh --flow compile Minimig`
  - Logs: `~/Development/Minimig-AGA_MiSTer/logs/YYYYMMDDx_build.txt`
  - Output: `~/Development/Minimig-AGA_MiSTer/output_files/Minimig.rbf`
  - Build time: ~25 minutes wall, ~55 minutes CPU (6 cores)
- **ARM cross-compile host:** `root@192.168.1.98` (Proxmox VM, `/opt/armV7-linux-gcc/bin/arm-none-linux-gnueabihf-g++`)
  - Source: `/opt/development/minimig/A2065/arm/`
  - Build: `cd /opt/development/minimig/A2065/arm && mkdir -p build/ddr3 && make ddr3`
- **m68k cross-compiler:** `root@192.168.1.98`, `/opt/amiga/bin/m68k-amigaos-gcc`
  - Source: `/opt/development/minimig/A2065/tests/`
  - Build: `/opt/amiga/bin/m68k-amigaos-gcc -noixemul -O2 -o <name> <name>.c`
- **MiSTer:** `mister` or `mister.local` (root SSH access, IP 192.168.1.31)
  - Deploy: `/media/fat/trans/` (RBF, ARM daemon, Amiga test binaries)
  - Note: Build server 192.168.1.98 cannot resolve `mister` hostname — deploy via dev machine relay
  - Note: MiSTer is intermittently offline — must verify reachability before deploy

## Directory Structure

```
A2065/
├── CLAUDE.md               This file
├── README.md               Project overview, build instructions, status table
├── DESIGN.md               Full architecture document
├── IMPLEMENTATION_PLAN.md   11-step implementation plan with verification criteria
├── .gitmodules             Minimig-AGA_MiSTer submodule
├── arm/                    ARM Linux daemon (full Amiberry port)
│   ├── Makefile            Native, cross-compile, test, deploy, ddr3 targets
│   ├── include/
│   │   ├── a2065_bridge.h  HPS2FPGA bridge register layout, mmap helpers
│   │   ├── a2065_types.h   CSR0/TX/RX bit definitions, card identity constants
│   │   └── boardram_access.h Switchable boardram access (direct ptr or DDR3 remote)
│   └── src/
│       ├── registers.cpp, rings.cpp, mac.cpp, ethernet.cpp, crc32.cpp
│       ├── main_ddr3.cpp       DDR3 daemon main loop
│       ├── boardram_remote.cpp DDR3-backed boardram read/write
│       ├── test_deadlock.cpp   ARM-side deadlock regression test (6 tests)
│       ├── test_interrupt.cpp  ARM-side interrupt test (8 tests)
│       └── *_test.cpp          Unit tests
├── tests/
│   ├── a2065_test.c        Basic register read/write test
│   └── a2065_diag.c        RAP state diagnostic (7 tests, PASS/FAIL per test)
├── fpga/                   Verilog + sim + constraints
│   ├── rtl/                a2065_top.v, a2065_autoconfig.v, a2065_registers.v, a2065_boardram.v
│   ├── sim/                tb_autoconfig.v, tb_boardram.v (passing), tb_bridge_e2e.v (WIP)
│   └── constraints/        a2065.sdc (timing constraints)
├── Minimig-AGA_MiSTer/     Git submodule — upstream Minimig core (modified for A2065)
│   ├── rtl/A2065/          a2065_registers.v, a2065_boardram.v, a2065_ddr3_mailbox.v, avalon_arbiter.v
│   ├── rtl/minimig.v       Boardram + registers instantiation, BRAM port wiring, CDC for INT2
│   ├── sys/sys_top.v       Mailbox adapter instance, arbiter, f2sdram2 connection, interrupt wiring
│   └── Minimig.sv          emu module — A2065 BRAM/bridge/INT2 ports pass-through
├── reference/              Amiberry source files for reference during porting
└── docs/                   (empty — planned for Step 10)
```

## Minimig Core Modifications

### Files Modified in Minimig-AGA_MiSTer submodule:
- **cpu_wrapper.v** — A2065 autoconfig nibbles added (type=0xC1, product=0x70, mfr=0x0202), base address latched from 0xE80048, `a2065_ena` driven from `~ac_a2065`. Nibbles 0-1 raw (er_Type), nibbles 2+ inverted.
- **gary.v** — `sel_a2065 = a2065_ena && cpu_address_in[23:16]==a2065_base`
- **minimig.v** — `a2065_boardram` instantiated, `a2065_registers` instantiated (DTACK-stretch via bridge `nrdy`), data mux OR-tied into CPU data bus. BRAM Port B wired to mailbox adapter (input ports for addr/wdata/wr/be, output for rdata). **Interrupt:** 2-stage CDC synchronizer for `a2065_int2` (clk_audio→clk_sys), OR-tied into Paula's `int2` input alongside CIA-A and IDE/Gayle.
- **Minimig.sv (emu)** — A2065 BRAM, bridge, and INT2 ports added as pass-through between sys_top.v and minimig.v
- **sys_top.v** — `a2065_ddr3_mailbox` instantiated (clk_audio domain), `avalon_arbiter` for f2sdram2 sharing (ddr_svc m0 + A2065 mailbox m1), boardram Port B signals wired to mailbox adapter. `a2065_mailbox_int2` wired from mailbox to emu module.
- **files.qip** — Added A2065 Verilog files
- **Minimig.sdc** — Added boardram multicycle constraints

## Implementation Status

| Step | Description | Status |
|------|-------------|--------|
| 0 | Bridge protocol & shared types | Done |
| 1 | MAC translation unit | Done (6 tests) |
| 2 | Raw Ethernet socket | Done (skeleton) |
| 3 | CSR state machine | Done (10 tests) |
| 4 | Descriptor ring walker | Done (8 tests) |
| 5 | Full daemon (sim bridge) | Done (bridge client test) |
| 6 | FPGA autoconfig | Done (sim + Quartus clean) |
| 7 | FPGA boardram window | Done (sim + Quartus) |
| 8 | FPGA chip register bridge + DTACK stretch | Done (sim + Quartus + MiSTer verified) |
| 9 | Integration (ARM + FPGA on MiSTer) | **Done** — deadlock fix, stale state fix, MAC fix, interrupt generation |
| **10** | **Stress test & polish** | **In Progress** — lance-test diags, real driver testing |

## DDR3 Mailbox Architecture

**Why DDR3 mailbox:** The HPS2FPGA lightweight bridge was found to be non-functional on MiSTer for this use case. The DDR3 shared-memory approach uses the existing f2sdram2 Avalon port (already used for audio/PAL), shared via a round-robin arbiter.

### DDR3 Mailbox Protocol

**Register mailbox (FPGA→ARM for RAP/RDP access):**
- **REG_REQ** at `DDR3_BASE + 0x8000` (ARM physical) / Avalon `0x1000`
  - bit[0]=pending, bit[1]=rw, bits[9:2]=addr (0=RDP, 2=RAP), bits[25:10]=data
- **REG_RSP** at `DDR3_BASE + 0x8008` / Avalon `0x1001`
  - bit[0]=ready, bits[16:1]=result

**Boardram mailbox (FPGA↔ARM for boardram word access):**
- **RAM_REQ** at `DDR3_BASE + 0x8010` / Avalon `0x1002`
  - bit[0]=pending, bit[1]=rw, bits[16:2]=offset, bits[32:17]=write_data
- **RAM_RSP** at `DDR3_BASE + 0x8018` / Avalon `0x1003`
  - bit[0]=ready, bits[16:1]=read_result

**Interrupt mailbox (ARM→FPGA for INT2 control):**
- **MBX_INT** at `DDR3_BASE + 0x8020` / Avalon `0x1004`
  - bit[0]=1 → assert INT2 (m68k level 2 interrupt via Paula PORTS), bit[0]=0 → deassert
  - Written by ARM daemon every main loop iteration based on CSR0_INTR && CSR0_INEA
  - FPGA polls every 256 cycles in S_IDLE (at poll_div==0xFE, alternating with RAM_REQ poll at 0xFF)

### Address mapping
- **DDR3_BASE (ARM physical):** `0x1FF00000` (safe region above kernel RAM)
- **DDR3_BASE (f2sdram2 Avalon):** `0x03FE0000` (ARM physical >> 3, 8-byte word addressing)
- **f2sdram2 port:** 29-bit word address, 64-bit data, `clk_audio` (~49MHz)
- **Arbiter:** Round-robin between ddr_svc (m0, audio/PAL) and A2065 mailbox (m1)

### Signal Path
```
Register path:
Amiga 68000 → a2065_registers (clk_sys, sel_chipreg)
  → bridge_new_req/data/addr_off/rw [CDC 2-stage sync]
  → a2065_ddr3_mailbox (clk_audio)
  → avalon_arbiter → f2sdram2 → DDR3 → ARM daemon (polling /dev/mem)

Boardram ARM access:
  ARM daemon → boardram_xfer() → writes DDR3 RAM_REQ
  → FPGA mailbox adapter polls RAM_REQ in idle slots (every 256 cycles)
  → reads/writes boardram Port B → writes DDR3 RAM_RSP
  → ARM daemon reads RAM_RSP

Interrupt path:
  ARM daemon → main loop checks registers_csr0() → writes DDR3 MBX_INT
  → FPGA mailbox adapter polls MBX_INT in idle slots (every 256 cycles, offset from RAM poll)
  → latches a2065_int2 output (clk_audio domain)
  → sys_top.v → Minimig.sv → minimig.v [2-stage CDC to clk_sys]
  → OR-tied into Paula .int2() → INTREQ bit 3 (PORTS) → 68000 level 2 interrupt
```

## Quartus Build History

### Build 20260507 (register DDR3 round-trip verified)
- **Result:** SUCCESS, 0 errors, 75 warnings
- **Timing:** Setup +0.461ns (pll_hdmi), +0.465ns (emu PLL)
- **Fix:** f2sdram2 address mapping corrected — Avalon addresses = byte_addr >> 3
- **MiSTer verified:** Full DDR3 round-trip — CSR0=$0004 (STOP), daemon saw all 10 requests

### Build 20260508 (boardram DDR3 mailbox added)
- **Result:** SUCCESS, 0 errors, 77 warnings
- **Changes:** Extended mailbox adapter with boardram access, wired boardram Port B to mailbox adapter in sys_top.v
- **Bug:** DDR3 path completely broken — mailbox adapter's RAM polling loop flooded the arbiter

### Build 20260509 (mailbox adapter fix)
- **Result:** SUCCESS, 0 errors, 9 warnings (fitter)
- **Fix:** Level detection (`req_sync1`) instead of edge detection for register requests

### Build 20260510 v1–v5 (boardram TDP rewrite, port direction fix, no-abort fix)
- v1: SUCCESS — boardram TDP rewrite + port direction fix
- v4/v5: **Last known good** (`Minimig_20260510d.rbf`) — Tests A-D PASS, DDR3 dies after ~13.5s idle
- Root cause of DDR3 death: Mailbox adapter floods DDR3 with ~100M reads/sec during idle

### Build 20260510 v6–v9 (arbiter/throttle fix attempts — ALL REGRESSED)
- v6: Dead (timing regression)
- v7: 3-request death (arbiter fix broke grant)
- v8: Same as v7 (arbiter priority+hold)
- v9: 4-request death (timeout in S_RAM_WAIT)
- **Lesson:** Do not modify the arbiter

### Build 20260524a (poll throttle — DDR3 stable)
- **Result:** SUCCESS, 0 errors
- **Timing:** Setup +0.207ns (emu PLL)
- **Changes:** 8-bit `poll_div` counter with `&poll_div` gate — 256x DDR3 throttle (~190K reads/sec)
- **MiSTer verified:** 17/17 register tests PASS, DDR3 stable >60s idle

### Build 20260524e (boardram BRAM read latency + address mapping fix)
- **Result:** SUCCESS, 0 errors
- **Timing:** Setup +0.241ns (emu PLL)
- **Changes:** Added `S_RAM_BRAM_LAT` + `S_RAM_BRAM_WAIT` for BRAM registered read latency, fixed `avl_readdata[16:3]` address mapping (was `[16:2]`), stale mailbox clear at startup
- **MiSTer verified:** 17/17 register tests + 5/5 boardram tests PASS

### Build 20260525a (interrupt generation) — CURRENT
- **Result:** SUCCESS, 0 errors, 86 warnings
- **Timing:** Setup +0.283ns (emu PLL), Hold +0.245ns — all positive
- **Changes:**
  1. `a2065_ddr3_mailbox.v`: Added `a2065_int2` output, `MBX_INT` DDR3 slot (Avalon 0x1004), `S_INT_CAPTURE`/`S_INT_WAIT` states (5-bit state encoding), polled at `poll_div==0xFE`
  2. `sys_top.v`: Wire `a2065_mailbox_int2` from mailbox to emu module
  3. `Minimig.sv`: Added `A2065_INT2` port, pass-through to minimig
  4. `minimig.v`: 2-stage CDC synchronizer for `a2065_int2` (clk_audio→clk_sys), OR-tied into Paula's `int2`
  5. `main_ddr3.cpp`: Main loop writes `MBX_INT` every iteration based on `CSR0_INTR && CSR0_INEA`
- **MiSTer verified:** 6/6 register/boardram tests PASS, 8/8 interrupt tests PASS (assert on IDON+INEA, deassert on clear, full lifecycle)

## ARM Daemon Changes (Session 2025-05-25)

### Deadlock Fix (`service_bridge`)
- **Problem:** ARM called `chip_wput()` before writing REG_RSP. When `chip_wput()` triggered `chip_init()` (boardram access), FPGA was stuck waiting for REG_RSP → deadlock
- **Fix:** Respond to register requests BEFORE calling `chip_wput()`. For reads, `chip_wget()` is safe (no boardram). For writes, REG_RSP sent first, then `chip_wput()` runs with FPGA free to service RAM mailbox
- **Verified:** `test_deadlock` — INIT triggers `chip_init()` with boardram access, no timeouts

### Stale FPGA State Fix
- **Problem:** After daemon exit, FPGA stuck in register path (S_REG_POLL_W) polling for REG_RSP forever
- **Fix:** On startup, write fake REG_RSP (`0x1`) to unstick FPGA, wait 2ms, then clear all mailboxes
- **Verified:** Boardram loopback 6/6 pass without core reload, 3/3 consecutive restarts

### MAC Address Fix
- **Problem:** `daemon_set_default_mac()` called before `ethernet_open()` → all-zero MAC
- **Fix:** Moved to after `ethernet_open()`. MAC now `00:80:10:00:04:2B`

### Interrupt Management
- **Design:** Main loop checks `registers_csr0()` every iteration, writes `MBX_INT` = 1 if `CSR0_INTR && CSR0_INEA`, else 0
- **No callback needed:** `on_interrupt_cb()` is a no-op — interrupt state driven purely by main loop polling CSR0
- **Assert latency:** ~1ms (main loop) + ~5µs (FPGA poll) — adequate for AmigaOS drivers

## Critical Arbiter Notes

The v5 arbiter has a burst tracking "bug" that is actually **essential** for correct operation:
- For burst_count=1 writes, burst start and write acceptance happen on same cycle (both gated by `!s_waitrequest`)
- `if (burst_active)` tracking uses OLD value (0), so first write is never counted
- burst_active gets permanently stuck at 1, burst_count=1
- This **keeps grant=m1 permanently**, preventing m0 (ddr_svc, 128-word PAL bursts) from stealing the bus
- All attempts to "fix" this bug (v7, v8) broke the DDR3 path because grant switches route stale readdatavalid to wrong master
- **DO NOT modify the arbiter**

## Timing Constraints (Minimig.sdc)

Boardram multicycle constraints (lines 20-27):
```tcl
set_multicycle_path -from {emu|minimig|a2065_boardram_inst|*} \
                    -to   {emu|minimig|a2065_boardram_inst|*} -setup 2
set_multicycle_path -from {emu|minimig|a2065_boardram_inst|*} \
                    -to   {emu|minimig|a2065_boardram_inst|*} -hold 1
```

Also yc_out chroma LUT multicycle constraints (lines 29-34) to fix timing degradation from boardram BRAM routing congestion.

## Common Quartus Warnings (all benign)

- Synthesized away RAM nodes, open-drain buffers removed, pins stuck at VCC/GND
- 40+ hierarchies connectivity warnings — standard for large Minimig design

## ARM Daemon (`a2065d_ddr3`)

- **Source:** `arm/src/main_ddr3.cpp`
- **Deployed to:** `/media/fat/trans/a2065d_ddr3`
- **Build:** `make ddr3` (uses `BOARDRAM_REMOTE` define, `build/ddr3/` directory)
- **Boardram access:** Via DDR3 mailbox (`boardram_remote.cpp`) — word-level transactions
- **Polling:** DDR3_BASE+0x8000 every 1µs, `do_transmit()` every 1000 iterations
- **Interrupt:** Main loop writes MBX_INT (DDR3_BASE+0x8020) every iteration based on CSR0 state
- **Current version:** Includes debug logging (`[req N]` lines, first 200 requests)
- **Cross-compile:** scp source → 192.168.1.98 → compile → scp binary to MiSTer
- **Startup sequence:** Write fake REG_RSP to unstick FPGA, wait 2ms, clear all mailboxes (REG_REQ/RSP, RAM_REQ/RSP, MBX_INT)
- **Register processing:** Responds to register requests BEFORE calling chip_wput() (deadlock prevention)

### ARM Build Notes
- `boardram_access.h` uses `extern "C"` for function declarations to match `boardram_remote.cpp` definitions
- `registers_set_boardram()` has `#ifndef BOARDRAM_REMOTE` guard (skips `boardram = ram` assignment in DDR3 build)
- `boardram_remote.cpp` in `SRCS_DDR3` list in Makefile
- `make clean` removes entire `build/` — must `mkdir -p build/ddr3` before `make ddr3`
- Build server 192.168.1.98 cannot resolve `mister` hostname — deploy via dev machine relay
- Test binaries must be statically linked (`-static`) for MiSTer

## m68k AmigaOS Cross-Compiler

- **Host:** `ssh root@192.168.1.98`
- **Path:** `/opt/amiga/bin/m68k-amigaos-gcc`
- **Test programs:**
  - `tests/a2065_test.c` → basic register read/write (deployed to `/media/fat/trans/a2065_test`)
  - `tests/a2065_diag.c` → RAP state diagnostic with 7 subtests (deployed to `/media/fat/trans/a2065_diag`)

## Key Design Decisions

1. **DDR3 mailbox over HPS2FPGA:** HPS2FPGA lightweight bridge doesn't work for this on MiSTer. DDR3 shared memory via f2sdram2 works (confirmed build 20260507).
2. **Level detection for register requests:** Edge detection (`req_edge`) was missed when state machine was in RAM polling states. Level detection (`req_sync1`) ensures register requests are never lost.
3. **Read-modify-write for TDP boardram:** Quartus can't infer TDP M10K from separate byte arrays across two clock domains. Single 16-bit array with RMW for byte enables is the Quartus-friendly pattern.
4. **ARM port directions must be inputs to minimig:** The boardram ARM port signals flow from sys_top.v (mailbox adapter) DOWN through emu→minimig→boardram. They must be declared as `input` in minimig.v, not `output`.
5. **Boardram Port B clock = clk_audio:** Matches mailbox adapter clock domain, avoids additional CDC.
6. **Autoconfig convention:** Nibbles 0-1 (er_Type) raw on D[15:12]; all other nibbles inverted. Matches WinUAE and Toccata pattern.
7. **v5 arbiter must NOT be changed:** The "bug" (burst_active stuck at 1 after burst_count=1 writes) accidentally keeps grant=m1 permanently, preventing m0 (ddr_svc, 128-word PAL bursts) from stealing the bus.
8. **Respond before processing writes:** `service_bridge()` writes REG_RSP before calling `chip_wput()` to prevent deadlock when chip_init/do_transmit need boardram via DDR3 RAM mailbox.
9. **Fake REG_RSP at startup:** Writing `0x1` to REG_RSP on daemon startup unsticks the FPGA if it was left mid-register-path by a previous daemon instance. Eliminates need for core reload between daemon restarts.
10. **Interrupt state via DDR3 poll:** ARM writes MBX_INT every main loop iteration; FPGA polls every 256 cycles. No callback complexity — pure polling from CSR0 state.
11. **INT2 into Paula PORTS:** A2065 interrupt OR-tied into Paula's `int2` input (INTREQ bit 3 → level 2). 2-stage CDC synchronizer in minimig.v for clk_audio→clk_sys crossing.

## Known Issues / Notes

- MAC serial bytes in cpu_wrapper.v are hardcoded (0x02, 0x70, 0x70, 0x70) — needs ARM-side runtime patching
- `cpu_berr_n` from register module not connected — watchdog timeout returns $0000 (not BERR)
- `boardram_remote.cpp` has unused `fd` variable (cosmetic warning)
- `main_ddr3.cpp` format string warning for `%X` vs `long unsigned int` (cosmetic)
- **Thread safety:** RX thread (`gotfunc`) and main thread both access CSR0 and boardram. No mutex protection. Currently benign because `registers_csr0()` reads a volatile uint16_t (atomic on ARM), and boardram DDR3 mailbox is single-threaded through `boardram_xfer()`. If issues arise, add locking.
- **lance-test diags (build 20260524e):** Buffer memory PASS, LANCE config PASS, Interrupt test FAIL (interrupts not implemented then — now fixed in 20260525a), collision/loopback not reached. MAC showed `00:FFFFFF80:10:70:70:70` (byte ordering issue in MAC register readback or init block).
- **AddNetInterface A2065:** Daemon only saw 1 request then nothing. Driver appears to stall. May be related to now-fixed boardram timeouts or missing interrupt support.

## MiSTer Operational Notes

- Core reload via SSH: `echo 'load_core /media/fat/<rbf_file>' > /dev/MiSTer_cmd`
- Serial port: `/dev/ttyS1` at 115200 baud (stty configured)
- Kill daemon before deploying new binary: `killall a2065d_ddr3; rm /media/fat/trans/a2065d_ddr3`
- exFAT on `/media/fat` with `sync` mount — may need to `rm` before `scp` if file in use
- Boardram test (`--test-boardram`) now works without core reload (stale state fix)
- `--iface eth1` for daemon (eth0 may be used by MiSTer main)
- Test binaries need `-static` flag for ARM cross-compilation

## Git History

| Commit | Date | Description |
|--------|------|-------------|
| `54fe2bb` | Apr 25 | Initial commit: full project skeleton |
| `445e101` | Apr 25 | Step 3: CSR state machine + MAC init block bug fix |
| `820609d` | Apr 25 | Step 4: Descriptor ring walker + TX auto-padding fix |
| `e8a4cbf` | Apr 25 | Step 5: Full daemon with sim bridge |
| `82b41be` | Apr 25 | Step 6: FPGA autoconfig (sim + Quartus) |
| `632e78c` | Apr 26 | Step 7: FPGA boardram (sim + Quartus) |
| *(pending)* | Apr 27 | Autoconfig er_Type nibble fix |
| *(pending)* | Apr 28 | Step 8: FPGA chip register bridge + DTACK stretch |
| *(pending)* | May 3-4 | Step 9: DDR3 mailbox integration (arbiter + mailbox adapter, 8 builds) |
| *(pending)* | May 7 | DDR3 f2sdram2 address fix, full round-trip verified |
| *(pending)* | May 8 | ARM daemon with BOARDRAM_REMOTE, boardram_access.h, boardram_remote.cpp |
| *(pending)* | May 9-10 | Mailbox adapter level-detect fix, minimig.v port direction fix, boardram TDP rewrite |
| *(pending)* | May 13-14 | Arbiter fix attempts (v7-v9 all REGRESSED), poll throttle v10 |
| *(pending)* | May 24 | Poll throttle build 20260524a, boardram BRAM latency fix 20260524e |
| *(pending)* | May 25 | Deadlock fix, stale FPGA state fix, MAC fix, interrupt generation build 20260525a |

## Remaining Work

1. **Re-run lance-test diags** with build 20260525a — verify interrupt test now passes
2. **Investigate MAC byte ordering** — lance-test showed garbled MAC `00:FFFFFF80:10:70:70:70`
3. **Test AddNetInterface A2065** — real AmigaOS driver test
4. **Stress test & polish** (Step 10) — extended run stability, packet throughput
5. **Thread safety review** — consider mutex for CSR0/boardram access if issues arise
6. **Connect cpu_berr_n** — watchdog timeout should generate BERR, not return $0000
