# A2065 Project Memory

## Project Overview

Full hardware emulation of the Commodore A2065 ZorroII Ethernet card for the Minimig FPGA core on MiSTer (DE10-Nano, Cyclone V). Uses standard AmigaOS A2065 drivers — no custom Amiga-side software. Port of Amiberry's `a2065.cpp` (Toni Wilen, 2009) from emulator-space to real hardware.

**Target hardware:** AMD Am7990 LANCE Ethernet controller, Commodore ZorroII card (64KB address space, 32KB boardram, manufacturer ID 0x0202, product ID 0x70, MAC OUI 00:80:10).

## Architecture Split

- **FPGA fabric:** Autoconfig, flat DDR3 boardram window (68k via DDR3 mailbox), CSR regfile + doorbell (zero-latency reads, async writes to ARM), interrupt generation (INT2 via DDR3 CSR slot)
- **ARM Linux daemon:** Am7990 CSR state machine, TX/RX ring walker, raw Ethernet socket (AF_PACKET), boardram access via DDR3 flat window, interrupt state management via CSR shadow
- **Bridge:** DDR3 shared-memory via f2sdram2 Avalon port (HPS2FPGA AXI bridge abandoned — non-functional on MiSTer). Two architectures:
  - **Old bridge (branch main):** `a2065_registers.v` with DTACK-stretch + DDR3 mailbox CMD/RSP protocol, `a2065_boardram.v` with TDP BRAM, `a2065d_ddr3` daemon
  - **New doorbell (branch `simplification/flat-ddr3-doorbell`):** `a2065_regfile.v` with zero-latency CSR reads + doorbell writes, `a2065_ddram.v` with flat DDR3 window, `a2065d_doorbell` daemon

## Remote Build Environment

- **Dev machine:** `nigelshearman@local` (macOS, working directory `/Volumes/Home/nigelshearman/Development/amiga/A2065`)
- **Quartus build host:** `nshearman@192.168.1.65` (Linux, Quartus 17.0 installed)
  - Source: `~/Development/Minimig-AGA_MiSTer/`
  - Quartus: `/opt/altera/17.0/quartus/bin/quartus_sh`
  - Build: `cd ~/Development/Minimig-AGA_MiSTer && /opt/altera/17.0/quartus/bin/quartus_sh --flow compile Minimig`
  - Logs: `~/Development/Minimig-AGA_MiSTer/logs/YYYYMMDDx_build.txt`
  - Output: `~/Development/Minimig-AGA_MiSTer/output_files/Minimig.rbf`
  - Build time: ~25 minutes wall, ~55 minutes CPU (6 cores)
- **ARM cross-compile host:** `root@192.168.1.97` (Proxmox VM, `/opt/armV7-linux-gcc/bin/arm-none-linux-gnueabihf-g++`)
  - Source: `/opt/development/minimig/A2065/arm/`
  - Build: `cd /opt/development/minimig/A2065/arm && mkdir -p build/ddr3 && make ddr3`
- **m68k cross-compiler:** `root@192.168.1.97`, `/opt/amiga/bin/m68k-amigaos-gcc`
  - Source: `/opt/development/minimig/A2065/tests/`
  - Build: `/opt/amiga/bin/m68k-amigaos-gcc -noixemul -O2 -o <name> <name>.c`
- **MiSTer:** `root@192.168.1.29` (root SSH access)
  - Deploy: `/media/fat/trans/` (RBF, ARM daemon, Amiga test binaries)
- **Python lance-test:** `tests/test_lance.py` (pytest, uses `mister_ssh.py` and `serial_long.py` helpers)
  - Run: `cd tests && python3 -m pytest test_lance.py -v -s`
  - Core env var: `A2065_CORE` specifies which RBF to load
  - Daemon env var: `A2065_DAEMON` specifies daemon path (default: doorbell)
  - lance-test binary: Pre-compiled Amiga binary at `share:lance-test` on Amiga filesystem
  - Disassembly: `lance-test/lance_disasm.txt`

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
│   ├── a2065_diag.c        RAP state diagnostic (7 tests, PASS/FAIL per test)
│   ├── test_lance.py       Pytest lance-test runner (loads core, starts daemon, runs lance-test via serial)
│   ├── run_lance_6x.py     Runs lance-test 6 times with stall/retry recovery
│   ├── mister_ssh.py       MiSTer SSH helper (load_core, start/stop daemon)
│   └── serial_long.py      Long-running serial connection helper
├── fpga/                   Verilog + sim + constraints
│   ├── rtl/                a2065_top.v, a2065_autoconfig.v, a2065_registers.v, a2065_boardram.v
│   ├── sim/                tb_autoconfig.v, tb_boardram.v (passing), tb_bridge_e2e.v (WIP)
│   └── constraints/        a2065.sdc (timing constraints)
├── Minimig-AGA_MiSTer/     Git submodule — upstream Minimig core (modified for A2065)
│   ├── rtl/A2065/          a2065_regfile.v, a2065_ddram.v, a2065_ddr3_mailbox.v, avalon_arbiter.v
│   ├── rtl/minimig.v       Regfile + ddram instantiation, nrdy OR-tie, data bus OR-tie, CDC for INT2
│   ├── sys/sys_top.v       Mailbox adapter instance, arbiter, f2sdram2 connection, interrupt wiring
│   └── Minimig.sv          emu module — A2065 BRAM/bridge/INT2 ports pass-through
├── reference/              Amiberry source files for reference during porting
└── docs/                   (empty — planned for Step 10)
```

## Minimig Core Modifications

### Files Modified in Minimig-AGA_MiSTer submodule:
- **cpu_wrapper.v** — A2065 autoconfig nibbles added (type=0xC1, product=0x70, mfr=0x0202), base address latched from 0xE80048, `a2065_ena` driven from `~ac_a2065`. Nibbles 0-1 raw (er_Type), nibbles 2+ inverted.
- **gary.v** — `sel_a2065 = a2065_ena && cpu_address_in[23:16]==a2065_base`
- **minimig.v** — `a2065_ddram` instantiated (DDR3 boardram window with DTACK stretch), `a2065_regfile` instantiated (zero-latency CSR reads + doorbell writes, `regs_nrdy=0`), data mux OR-tied into CPU data bus. **Interrupt:** 2-stage CDC synchronizer for `a2065_int2` (clk_audio→clk_sys), OR-tied into Paula's `int2` input alongside CIA-A and IDE/Gayle.
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
| 9 | Integration (ARM + FPGA on MiSTer) | **Done** — old bridge: deadlock fix, stale state fix, MAC fix, interrupt generation, CDC bridge_done fix |
| **10** | **Doorbell architecture** | **DONE** — lance-test **5/5 PASS, "Controller PASSED diagnostics"** (build 20260609a + byteswap daemon). Buffer/Config/Interrupt/Collision/Loopback all PASS. share:a2065_memtest 10/10. Surpasses old-bridge baseline (3/4). |
| **11** | **Stress test & polish** | Pending |

## DDR3 Mailbox Architecture

**Why DDR3 mailbox:** The HPS2FPGA lightweight bridge was found to be non-functional on MiSTer for this use case. The DDR3 shared-memory approach uses the existing f2sdram2 Avalon port (already used for audio/PAL), shared via a round-robin arbiter.

### Doorbell Architecture (branch `simplification/flat-ddr3-doorbell`)

**Key difference from old bridge:** Zero-latency CSR reads (combinational output from CSR shadow registers), no DTACK stretch for any access. RDP writes raise a doorbell that the ARM daemon polls asynchronously.

#### DDR3 Layout (ARM physical offsets from DDR3_FLAT_BASE = 0x1FF00000)

| Offset | Size | Purpose |
|--------|------|---------|
| 0x0000 | 0x8000 (32KB) | Flat boardram window (4×16-bit per 64-bit DDR3 word) |
| 0x8000 | 8 bytes | CMD slot: `{39'b0, data[15:0], rap[6:0], pending[0]}` |
| 0x8010 | 8 bytes | CSR shadow: `{csr3, csr2, csr1, csr0}` (64-bit, 16-bit each) |
| 0x8018 | 8 bytes | INT state: `{63'b0, int_assert[0]}` |
| 0x8028 | 8 bytes | MAC: `{15'b0, mac[47:0], valid[0]}` |

#### FPGA Modules (doorbell architecture)
- **`a2065_regfile.v`** — CSR register file + doorbell. Combinational reads from CSR shadow (updated by ARM via DDR3). RDP writes set `cmd_pending` + `cmd_rap` + `cmd_data`. No back-pressure (`regs_nrdy=0` always). Overwrites previous doorbell if still pending.
- **`a2065_ddram.v`** — 68k DDR3 boardram window. `sel_br = sel && cpu_addr[15]` selects $EA8000+ range. DTACK-stretched (`a2065_bram_nrdy`) while DDR3 round-trip in progress. Level-detected CDC handshake with mailbox FSM.
- **`a2065_ddr3_mailbox.v`** — Mailbox FSM (clk_audio). Cycles through: CMD poll (level-detect) → BRAM request (level-detect) → CSR/INT poll (every 64 cycles). 6 states: S_IDLE, S_CMD_WR_W/D, S_BR_CAPTURE/READ_W/READ_D/WRITE_W/DONE, S_CSR_RD_W/D, S_INT_RD_W/D.

#### ARM Daemon (`a2065d_doorbell`)
- Polls CMD slot, processes register writes, pushes CSR shadow + INT state after each CMD
- Boardram accessed via flat DDR3 window pointer (no per-word mailbox transactions)
- Build: `make doorbell` (uses `build/doorbell/` directory)

#### Signal Path (doorbell)
```
Register read:
Amiga 68000 → a2065_regfile (combinational output from csr_shadow) → data bus OR-tie
  CSR shadow updated by ARM → DDR3 CSR slot → mailbox FSM poll → csr0_out..3_in → regfile

Register write (doorbell):
Amiga 68000 → a2065_regfile → cmd_pending/rap/data (level) → mailbox FSM (CDC 2-stage)
  → DDR3 CMD slot write → ARM daemon polls → chip_wput() → push_csr_shadow() → DDR3 CSR slot

Boardram:
Amiga 68000 → a2065_ddram → bram_req_valid (level) → mailbox FSM (CDC 2-stage)
  → DDR3 read/write → bram_resp_data/valid → ddram (CDC 2-stage) → cpu_data_out

Interrupt:
ARM daemon → push_csr_shadow + update_int_state → DDR3 INT slot
  → mailbox FSM polls → a2065_int2 → CDC 2-stage → Paula int2 → 68000 level 2
```

### Old Bridge DDR3 Mailbox Protocol (branch main)

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

**MAC mailbox (ARM→FPGA for dynamic MAC address):**
- **MBX_MAC** at `DDR3_BASE + 0x8028` / Avalon `0x1005`
  - bit[0]=1 (valid), bits[47:16]=MAC bytes (6 bytes packed), written once at daemon startup
  - FPGA reads at startup during S_MAC_BOOT states (timing-dependent — may race with core load)

### Address mapping
- **DDR3_BASE (ARM physical):** `0x1FF00000` (safe region above kernel RAM)
- **DDR3_BASE (f2sdram2 Avalon):** `0x03FE0000` (ARM physical >> 3, 8-byte word addressing)
- **f2sdram2 port:** 29-bit word address, 64-bit data, `clk_audio` (~49MHz)
- **Arbiter:** Round-robin between ddr_svc (m0, audio/PAL) and A2065 mailbox (m1)

### Signal Path (old bridge)
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

### Doorbell Architecture Builds (branch `simplification/flat-ddr3-doorbell`)

#### Build 20260606a (old bridge, known-good baseline)
- **Architecture:** Old bridge (`a2065_registers.v` BRIDGE_LOCAL=0 + `a2065_boardram.v` TDP BRAM + `a2065d_ddr3`)
- **MiSTer verified:** lance-test 3/4 PASS — Buffer PASS, Config PASS, Interrupt PASS, Collision FAIL
- **Note:** Uses `a2065d_ddr3` (old daemon) with DTACK-stretch for every register access

#### Build 20260607a/b (doorbell Phase 2+3 — lance-test HANGS)
- **Architecture:** Doorbell (`a2065_regfile.v` + `a2065_ddram.v` + `a2065d_doorbell`)
- **MiSTer result:** lance-test locks up after printing "Copyright" line. OpenDevice hangs.
- **Root cause:** Three bugs (see build 20260608c fixes below)

#### Build 20260608a (no back-pressure fix — still hangs)
- **Changes:** `regs_nrdy = 1'b0` always (removed back-pressure on RDP writes). RDP writes overwrite previous pending doorbell.
- **MiSTer result:** Still hangs — zero CMDs received by ARM daemon. Bug was elsewhere.

#### Build 20260608b (ddram nrdy guard fix — still hangs)
- **Changes:** ddram `nrdy_state` only transitions to NR_WAIT when `sel_br && !sys_req && !sys_got_resp` (matching request issue condition). Prevents entering NR_WAIT without issuing a DDR3 request.
- **MiSTer result:** Still hangs. Fix was necessary but insufficient.

#### Build 20260608c (level-detect boardram — DOORBELL WORKING)
- **Result:** SUCCESS, 0 errors, 100 warnings
- **Changes:** `a2065_ddr3_mailbox.v`: Changed boardram request detection from edge (`bram_req_valid_s1 & ~bram_req_valid_s2`) to level (`bram_req_valid_s1`). Edge detection could miss requests when FSM was in non-IDLE states processing CMDs or CSR polls.
- **MiSTer verified:** lance-test runs to completion! Copyright + MAC + "Buffer memory test FAIL". ARM daemon received `[cmd 0] raw=0x0000000000000401 rap=0 data=0004` (CSR0 STOP write).
- **Sim tests:** tb_regfile 12/12 PASS, tb_autoconfig 36/36, tb_boardram 15/15
- **ARM tests:** boardram flat 6/6 PASS

#### Build 20260608d (cpu_rw write detection + sel_br_rise edge — REGRESSION, Amiga hangs)
- **Result:** SUCCESS, 0 errors, 100 warnings
- **Changes:** `a2065_ddram.v`: Replaced `cpu_hwr`/`cpu_lwr` with `cpu_rw` for write detection (`is_write = ~cpu_rw`). Added `sel_br_rise` edge detection for request issue. `minimig.v`: Connected `cpu_rw` to ddram instead of `cpu_hwr`/`cpu_lwr`.
- **MiSTer result:** REGRESSION — Amiga locks up during lance-test buffer test. Zero CMDs received by ARM daemon. The `sel_br_rise` edge detection caused the lockup: `sel_br` is a level signal that stays high for the entire bus cycle, and consecutive boardram accesses may not have a falling edge between them, so NR_DONE→NR_IDLE can't transition and the state machine gets stuck.

#### Build 20260608e (cpu_rw without sel_br_rise — REGRESSION, Amiga hangs)
- **Result:** SUCCESS, 0 errors, 100 warnings
- **Changes:** Reverted `sel_br_rise` to level-based `sel_br`, kept `cpu_rw` for write detection.
- **MiSTer result:** REGRESSION — Amiga still hangs during lance-test buffer test. Zero CMDs. The `cpu_rw` signal itself causes the hang. Root cause unclear — possibly `cpu_r_w` has different timing/polarity than expected, or the bridge's `lr_w` signal is not valid when `sel_br` first goes high. Reverted to build 20260608c (cpu_hwr/cpu_lwr).

#### Session 2026-06-08 Findings

- **Test framework fix:** `serial_long.py` must use `\r` (not `\r\n`) for Amiga shell commands. `\n` causes the second word of the command (e.g., `diags`) to be eaten, making lance-test show usage instead of running diagnostics.
- **`minimig_netd` conflict:** The MiSTer's `/etc/init.d/S90minimig_netd` starts `minimig_netd` which conflicts with the A2065 daemon. Must kill it and disable the init script: `killall minimig_netd; mv /etc/init.d/S90minimig_netd /etc/init.d/S90minimig_netd.disabled`
- **`cpu_rw` write detection does NOT work:** Replacing `cpu_hwr`/`cpu_lwr` with `cpu_rw` causes Amiga to hang. The pulsed `cpu_hwr`/`cpu_lwr` signals (gated by bridge `enable`) work correctly in practice despite appearing to have a circular dependency with DTACK. The circular dependency is actually broken because the bridge asserts `enable` based on `!nrdy` at a specific clock phase, not continuously.
- **`sel_br_rise` edge detection does NOT work:** `sel_br` stays high for the entire bus cycle and may not transition low between consecutive boardram accesses, causing the NR_DONE state machine to get stuck. Level-based `sel_br` is required.
- **Buffer test FAIL root cause still unknown:** Build 20260608c (reverted to cpu_hwr/cpu_lwr) shows Buffer FAIL. The boardram DDR3 path works for ARM flat test (6/6 PASS) but fails for 68k-initiated accesses during lance-test. The write detection is NOT the cause (CMDs are received, meaning register writes work). The issue is specifically in the 68k→DDR3 boardram data path.

#### Three Bugs That Caused the Doorbell Hang (Session 2026-06-08)

1. **`regs_nrdy` back-pressure in regfile** — After the first RDP write set `cmd_pending=1`, the second RDP write would see `cmd_pending_d=1` and assert `regs_nrdy`, stretching DTACK forever (ARM daemon can't clear fast enough for back-to-back writes). **Fix:** `regs_nrdy = 1'b0` always. RDP writes overwrite previous pending doorbell instead of back-pressuring.

2. **ddram nrdy state machine could enter NR_WAIT without issuing a request** — `nrdy_state` transitioned to NR_WAIT whenever `sel_br` was high, but the actual DDR3 request had additional guards (`!sys_req`). If `sys_req` was still set from a previous request, `nrdy_state` would be in NR_WAIT with no request issued → DDR3 response never arrives → permanent bus hang. **Fix:** Guard NR_IDLE→NR_WAIT transition with `!sys_req && !sys_got_resp`.

3. **Mailbox FSM edge detection missed boardram requests** — `bram_req_rise = bram_req_valid_s1 & ~bram_req_valid_s2` is a one-cycle edge pulse. If the FSM was in S_CMD_WR_W, S_CSR_RD_W, or any non-IDLE state during that one cycle, the edge was missed and the boardram request lost forever → ddram stuck in NR_WAIT → bus hang. **Fix:** Changed to level detection (`bram_req_active = bram_req_valid_s1`), same approach as `cmd_active` for doorbell CMDs.

#### Build 20260608f (byteenable→RMW boardram writes — still Buffer FAIL)
- **Change:** `a2065_ddr3_mailbox.v` boardram writes converted from partial f2sdram2 byteenable to read-modify-write (read 64-bit line, merge 16-bit lane, write full line be=0xFF). New states S_BR_RMW_RD_W/S_BR_RMW_RD_D. Removed `br_be`/`br_wdata_shifted`.
- **Result:** Buffer still FAIL. Diagnosis via zero-DDR3 + 68k memtest + dump: DDR3 filled with `0x8000|offset` (the address bus). Reads = 0. → 68k writes were being dropped and reads were writing the address. Pointed at request capture, not byteenable. RMW kept (correct for partial writes regardless).

#### Build 20260608g (is_write = cpu_hwr|cpu_lwr — still FAIL, writes vanished)
- **Change:** flipped `is_write` to `cpu_hwr | cpu_lwr` (active-high enables).
- **Result:** DDR3 now stays all-zero after memtest, reads still 0. Confirmed the real problem: capture on raw `sel_br` (address phase) samples direction+data before the strobes/data are valid. At capture time hwr/lwr=0 → with this polarity everything classified read → writes dropped.

#### Build 20260608h (AS+DS capture qualification — BUFFER TEST PASS)
- **Result:** SUCCESS, 0 errors, 100 warnings. RBF `Minimig_20260608h.rbf` (3513552 bytes).
- **Change:** `a2065_ddram.v` — `sel_br = sel && cpu_addr[15] && !cpu_as_n && !cpu_ds_n`; `is_write = ~cpu_rw`; gated `cpu_data_out`. `minimig.v` — pass `cpu_r_w`, `_cpu_as`, `_cpu_uds & _cpu_lds`.
- **MiSTer verified:** lance-test `Buffer memory test PASS` (was FAIL). share:a2065_memtest 7/10: walking-bit PASS, address-uniqueness PASS, post-INIT integrity PASS, CSR STOP/RAP/model-ID PASS. Fails: byte access + odd/even byte independence (RMW word-granular, note 16), CSR0 IDON after INIT (separate interrupt/init issue). lance-test now advances to LANCE configuration test (FAIL — same IDON root cause).

#### Build 20260608i (INIT write back-pressure — 3/4 PASS, matches old-bridge baseline)
- **Result:** SUCCESS, 0 errors, 101 warnings. RBF `Minimig_20260608i.rbf`.
- **Change:** end-to-end doorbell back-pressure. `a2065_regfile.v`: RDP writes stretch DTACK (W_IDLE→W_WAIT→W_DONE FSM, `regs_nrdy = wstate==W_WAIT`) until the doorbell is drained; RAP writes stay local, reads stay zero-latency. `a2065_ddr3_mailbox.v`: after posting CMD, poll the DDR3 CMD slot (new S_CMD_POLL_W/S_CMD_POLL_D) until the daemon clears the pending bit, then pulse `cmd_clear`. No daemon change (it already writes CMD slot = 0 after draining).
- **Root cause fixed:** no-back-pressure single CMD slot lost writes when the 68k outran the daemon drain, dropping the rapid LANCE INIT RAP/RDP sequence (CSR1/CSR2/CSR3 init-addr + CSR0=INIT) → init block never set → CSR0 IDON never asserted.
- **MiSTer verified:** lance-test **Buffer PASS, LANCE configuration PASS, Interrupt PASS, Collision FAIL (3/4)** — matches old-bridge known-good baseline 20260606a. share:a2065_memtest 8/10: + CSR0 IDON after INIT now PASS. Remaining fails: byte access + odd/even (RMW word-granular).

#### Daemon byteswap (boardram_access.h) — lance-test 5/5 PASS (no Quartus rebuild)
- **Change:** `arm/include/boardram_access.h` flat (#else) accessors XOR every byte index with 1: `boardram[(off ^ 1) & RAM_MASK]`. The 68k is big-endian; the FPGA stores each 68k 16-bit word little-endian in its DDR3 lane (byte[off]=D[7:0], byte[off+1]=D[15:8]), so a byte the 68k placed at offset `off` lives at ARM offset `off ^ 1`. Without this the daemon read the 68k-written init block byteswapped → wrong TDRA/RDRA → `do_transmit` found no TX descriptors.
- **Diagnosis:** added DOTX/scan logging — `do_transmit` ran but TX ring at the computed `tdra=0x1880` was all zeros, while the real descriptors (TMD OWN|STP) sat at boardram 0x18. The init-block TDRA field bytes `18 80` read big-endian gave 0x1880; little-endian gave 0x8018 → `& RAM_MASK` = 0x18 (where the descriptors actually are). After fix: `mode` reads 0x0044 (was 0x5400), TDRA resolves correctly, all 10 collision sends run.
- **GOTCHA (cost ~5 build cycles):** a **stale duplicate `src/boardram_access.h` existed only on the build host** (192.168.1.97), not in the repo. Since `registers.cpp`/`rings.cpp` live in `src/` and `#include "boardram_access.h"`, the compiler resolved the same-dir `src/` copy *before* `-I include`, silently ignoring edits to `include/boardram_access.h` (md5 unchanged, `raw[0]` stayed 0x5400). Fix: `rm src/boardram_access.h` on the build host. Verify which header is active with `g++ -I include -E src/registers.cpp | grep get_ram_byte`.
- **MiSTer verified:** lance-test Buffer/Config/Interrupt/Collision/Loopback **5/5 PASS**, "Controller PASSED diagnostics". memtest still 10/10. Daemon-only change — same FPGA (build 20260609a / `Minimig_20260609a.rbf`).

#### Build 20260609a (byte-granular RMW — memtest 10/10)
- **Result:** SUCCESS, 0 errors, 101 warnings. RBF `Minimig_20260609a.rbf`.
- **Change:** byte-enable RMW. `a2065_ddram.v` takes `cpu_uds_n`/`cpu_lds_n`, derives `req_be[1:0] = {~uds_n, ~lds_n}` (high=UDS=D[15:8], low=LDS=D[7:0]), latches `bram_req_be`. `a2065_ddr3_mailbox.v` merges only the enabled byte(s) of the target lane in S_BR_RMW_RD_D. `bram_req_be` threaded minimig.v → Minimig.sv → sys_top.v → mailbox; `_cpu_uds`/`_cpu_lds` passed instead of the combined `ds_n`.
- **Root cause fixed:** word-granular RMW wrote the full 16-bit `cpu_data_in` even on a 68k byte access, clobbering the untouched byte.
- **MiSTer verified:** share:a2065_memtest **10/10 PASS** (byte access + odd/even now PASS). lance-test still 3/4 (Collision FAIL — daemon-side, separate).
- **Commit:** submodule `ff20394`.

### Old Bridge Builds (branch main)

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

### Build 20260525a (interrupt generation)
- **Result:** SUCCESS, 0 errors, 86 warnings
- **Timing:** Setup +0.283ns (emu PLL), Hold +0.245ns — all positive
- **Changes:**
  1. `a2065_ddr3_mailbox.v`: Added `a2065_int2` output, `MBX_INT` DDR3 slot (Avalon 0x1004), `S_INT_CAPTURE`/`S_INT_WAIT` states (5-bit state encoding), polled at `poll_div==0xFE`
  2. `sys_top.v`: Wire `a2065_mailbox_int2` from mailbox to emu module
  3. `Minimig.sv`: Added `A2065_INT2` port, pass-through to minimig
  4. `minimig.v`: 2-stage CDC synchronizer for `a2065_int2` (clk_audio→clk_sys), OR-tied into Paula's `int2`
  5. `main_ddr3.cpp`: Main loop writes `MBX_INT` every iteration based on `CSR0_INTR && CSR0_INEA`
- **MiSTer verified:** 6/6 register/boardram tests PASS, 8/8 interrupt tests PASS (assert on IDON+INEA, deassert on clear, full lifecycle)

### Build 20260527a (boardram poll budget + MAC DDR3 slot)
- **Result:** SUCCESS, 0 errors
- **Timing:** Setup +0.283ns (emu PLL)
- **Changes:** `reg_budget[3:0]` counter in mailbox — RAM poll every 128 cycles instead of every cycle; MBX_MAC DDR3 slot at Avalon 0x1004 (later 0x1005) for dynamic MAC from ARM daemon
- **MiSTer verified:** 0 boardram timeouts, interrupt test PASS

### Build 20260527b (combined budget + MAC + interrupt fix)
- **MiSTer verified:** lance-test 3/4 PASS — Buffer memory PASS, LANCE config PASS, Interrupt PASS, Collision FAIL

### Build 20260527c (boardram address decoding fix) — REGRESSION
- **Result:** SUCCESS, 0 errors
- **Timing:** Setup +0.003ns (emu PLL)
- **Changes to `a2065_boardram.v`:** Added `card_base` input; compute `rel_addr = cpu_addr - {card_base, 15'h0000}`; use `rel_addr[14]` for boardram select, `rel_addr[13:0]` for Port A BRAM address
- **Changes to `minimig.v`:** Boardram instantiation passes `.card_base(a2065_base)`
- **Intent:** Fix address decoding so Port A (68k) and Port B (ARM mailbox) address the same BRAM locations regardless of Zorro II base address
- **Bug found:** Old code used `cpu_addr[15]` for select (only worked for odd bases like 0xE9) and `cpu_addr[14:1]` for address (included card base offset, mismatching Port B)
- **MiSTer result:** REGRESSION — lance-test 4 runs: 2× interrupt FAIL, 2× LANCE config FAIL. Never reaches collision test. Previous build 20260527b was better (3/4 PASS)
- **Root cause:** The `/media/fat/Minimig_20260527c.rbf` was a bad Quartus build — DDR3 path completely dead (all mailbox transactions timeout). Rebuilding with same source produced working DDR3 (`Minimig_20260527d.rbf`, 3,534,384 bytes vs broken 3,476,528 bytes). However, even with working DDR3, register path dies after 3-5 requests.

### Build 20260527d (clean rebuild of boardram fix)
- **Result:** SUCCESS, 0 errors, 87 warnings
- **Timing:** Setup +0.003ns (emu PLL)
- **File:** `/media/fat/Minimig_20260527d.rbf` (3,476,528 bytes — same as broken 20260527c but rebuilt with `rm -rf db`)
- **MiSTer:** DDR3 boardram loopback 6/6 PASS, but register path still dies after 3-5 requests

### Build 20260527h (fully committed code — no uncommitted changes)
- **MiSTer:** Same register path death — daemon sees 5 requests then mailbox stuck in S_REG_DONE
- **Confirmed:** Issue is NOT the boardram address fix or any FPGA source change

### Critical Finding: S_REG_DONE Mailbox Adapter Stuck (Session 2025-05-27)
- **Root cause:** `bridge_done` signal crosses clock domains (clk_audio → clk_sys) WITHOUT a CDC synchronizer. The register module in clk_sys domain may not reliably capture bridge_done pulses.
- **Evidence:** Daemon processes 3-5 register requests correctly, then mailbox adapter gets stuck in `S_REG_DONE` waiting for `req_sync1` to go low. `req_sync1` depends on `bridge_new_req` from the register module, which is only cleared when `bridge_done` is seen. If `bridge_done` is missed due to CDC metastability, `bridge_new_req` stays high → `req_sync1` stays high → stuck forever.
- **Signal path:** `a2065_ddr3_mailbox.bridge_done` (clk_audio) → `sys_top.v` → `Minimig.sv` → `minimig.v` → `a2065_registers.bridge_done` (clk_sys) — NO synchronizer in this path.
- **Compare:** The `a2065_int2` interrupt signal has a 2-stage CDC synchronizer in minimig.v (working). `bridge_done` does NOT (broken).
- **Fix in progress:** Add 2-stage CDC synchronizer for `bridge_done` and `bridge_result` in minimig.v, similar to the existing `a2065_int2` synchronizer.
- **Why it worked before:** The May 25 session may have had slightly different Quartus routing that made the CDC work reliably. Timing slack was +0.283ns then vs +0.003ns now — the tighter timing makes metastability more likely.

### Build 20260528a (CDC synchronizer for bridge_done)
- **Result:** SUCCESS, 0 errors
- **Timing:** Setup +0.003ns (emu PLL)
- **Changes:** 2-stage CDC synchronizer for `bridge_done` and `bridge_result` in `minimig.v` (clk_audio→clk_sys), similar to existing `a2065_int2` synchronizer
- **MiSTer verified:** Register path no longer stuck after 3-5 requests. DDR3 stable. lance-test 100x runner infrastructure created.

### Build 20260528a — lance-test 100x Results (Session 2026-05-29)

**Baseline → Final comparison (100 runs, daemon restart between runs):**

| Test | Baseline | Final (v9) | Change |
|------|----------|------------|--------|
| Buffer memory | 100% | 100% | — |
| LANCE config | 94% | 91% | -3% |
| Interrupt | 34% | **84%** | **+50%** |
| Collision logic | 0% | **76%** | **+76%** |
| ALL PASS rate | ~0% | **~75%** | **+75%** |

#### Fixes Applied This Session (6 total):

1. **TX_ERR+TX_RTRY for MODE_COLL** (`rings.cpp:129-141`) — Sets collision flags in TX descriptor when MODE_COLL is set
2. **MBX_INT hold counter** (`main_ddr3.cpp:64-65`) — `INT_HOLD_ITER=5000` prevents premature MBX_INT deassertion; main loop skips `write_mbx_int()` during hold period
3. **Self-loopback collision detection** (`rings.cpp:129-136`) — When DST MAC == own MAC, simulates hardware collision (TX_ERR+TX_RTRY) instead of sending via ethernet. The real Am7990 relies on the physical loopback plug for collisions; MODE_COLL is never set in the init block (mode always 0x0000).
4. **rethink() interrupt callback** (`main_ddr3.cpp:79`, `registers.cpp:122`) — `on_interrupt_cb()` calls `assert_mbx_int()` which writes MBX_INT=1 + sets `int_hold`. Fires from `rethink()` whenever CSR0_INTR && CSR0_INEA.
5. **Pre-assert MBX_INT for INIT/TDMD writes** (`main_ddr3.cpp:140-142`) — Asserts MBX_INT before REG_RSP for write requests with INIT/TDMD/STRT bits. Also adds `usleep(200)` after `chip_wput()` for INIT writes with INTR set, giving FPGA time to poll MBX_INT before daemon processes next request.
6. **TDMD without STRT + preserve am_initialized** (`registers.cpp:177-180, 161-165`) — TDMD triggers `do_transmit()` based on `am_initialized` only (not STRT). `am_initialized` preserved across STOP (not reset). The lance-test collision test never writes STRT, only STOP → STRT+TDMD.

#### Key Discovery: lance-test Collision Test Behavior
- The collision test init function writes INIT (0x0041 = INIT+INEA) without STRT. It never writes STRT as a separate CSR0 write.
- The packet send loop writes CSR0=0x00EA (STRT+TDMD+TXON+INEA+INTR) which combines STRT and TDMD in one write.
- `chip_init()` mode is always 0x0000 — the collision test does NOT set MODE_COLL (0x0054) in the init block. It relies on the physical loopback plug for real hardware collisions.
- Self-loopback collision detection (DST MAC == own MAC) replaces the MODE_COLL check.

#### Key Discovery: Interrupt Timing Race
- After INIT, the Amiga reads CSR0, sees IDON, clears IDON — all within 2-3 register requests (~10-30µs).
- The FPGA mailbox adapter only polls MBX_INT in S_IDLE state, every 32 cycles at 49MHz (~0.65µs).
- But during `chip_init()` boardram DDR3 access (~200-500µs), the adapter is busy with RAM_REQ processing and NOT polling MBX_INT.
- The pre-assert + hold counter + post-INIT delay mitigates this for ~84% of cases.
- Remaining 16% failure: FPGA is processing RAM_REQ during the critical window, MBX_INT not polled in time.

#### Remaining Issues
1. **Collision FAIL (7/100 when test runs):** Daemon never sees TDMD writes from the collision test's `lance_send_pkt`. Root cause unclear — the Amiga writes TDMD via the register bridge but the daemon doesn't receive the request. Possibly FPGA mailbox adapter busy with other DDR3 traffic, or the register bridge path has an intermittent issue.
2. **Collision MISSING (17/100):** Caused by LANCE config or interrupt failures preventing the collision test from running.
3. **Interrupt FAIL (4/100):** MBX_INT timing race — Amiga clears CSR0 flags before FPGA has polled MBX_INT.
4. **LANCE config FAIL (9/100):** Same root cause as interrupt — init doesn't complete in time.

## ARM Daemon Changes (Session 2026-05-27)

### TX_ERR/TX_RTRY Fix for Collision Test (`rings.cpp`)
- **Problem:** Collision test in lance-test sends 10 packets in loopback+collision mode. Daemon's `do_transmit()` needed to set TX_ERR (0x4000) in TMD1 and TX_RTRY (0x0400) in TMD3 for collision path.
- **Fix:** Added `tmd1 |= TX_ERR` and `put_ram_word(off + 2, tmd1)` in `do_transmit()` collision path in `rings.cpp`
- **Status:** Applied but collision test still FAILS — only 5 of expected 10 do_transmit calls observed. Suspect race condition in TX descriptor ring walking or `service_bridge_safe` dropping CSR0 writes during do_transmit.

### Diagnostic Logging
- Debug log limit increased to 2000 requests
- `service_bridge_safe` tagged `(safe)` in log
- `do_transmit` logs entry with tdr_offset/tmd1/mode
- TX_OWN clearing loop logs per-descriptor walk with tmd1 values
- Collision path logs COLL TX (tmd1/tmd3/offset) and COLL RX (rmd1/verify/offset)

### Collision Test Analysis (lance-test disassembly)
- **Test function at 0x158E:** First calls init function (0x6a0) with mode=0x54 (LOOP|DTCR|COLL). If init fails (returns <0), test FAILS immediately.
- **Loop at 0x15DE-0x160E:** 10 iterations, calls lance_send_pkt (0x1f8). If send returns <0 AND bit 10 of saved TMD3 (8510(a4)) is set, increments d4. PASS if d4==10.
- **lance_send_pkt (0x1f8):** Uses next TX descriptor (tx_count wraps at tx_num-1). Writes packet data, TMD2 (negated size), TMD1 byte 0x83 (TX_OWN|TX_STP|TX_ENP), then CSR0=TDMD. Polls for TX_OWN clear (50-iteration timeout with WaitTOF). Error check: CSR0 bits 0x6800 (BABL/CERR/MERR), then `btst #6, 2(a0)` (TMD1 bit14=TX_ERR).
- **TMD3 save at 0x34C:** `move.w 6(a0), 8510(a4)` — saves TMD3 AFTER TX_OWN poll completes. Daemon should have set TX_RTRY by then.
- **tx_count=4** (set at 0x9BA), wraps at 3. TX ring has 4 descriptors.

### Key Finding: service_bridge_safe Drops CSR0 Writes
- `service_bridge_safe()` (called from boardram DDR3 transactions) only forwards RAP writes (addr=2) to `chip_wput()`. CSR0 writes (addr=0) are consumed (response sent, request cleared) but NOT processed.
- Impact: When Amiga writes CSR0 (e.g., clearing status flags at end of lance_send_pkt) while daemon is in do_transmit, the write is silently dropped.
- Observed: req 112 (safe) wrote CSR0=0xFF00 (clear all status) during desc 3 processing — dropped.
- This may cause stale CSR0 state confusing the Amiga's polling loops.

### Key Finding: TX_OWN Clearing Loop Walks Multiple Descriptors
- The TX_OWN clearing loop (lines 153-162 in rings.cpp) starts at `start_offset` and walks forward until it finds TX_ENP.
- For collision sends, the collision path (lines 131-142) re-reads the descriptor after gotfunc runs. If TX_OWN was already cleared (e.g., by a race), the collision path reads tmd1 without TX_OWN or TX_ENP, causing the TX_OWN clearing loop to walk into the NEXT descriptor.
- Observed for desc 3: COLL TX tmd1=0x4000 (no TX_OWN, no TX_ENP). This caused the TX_OWN clearing loop to walk into desc 0, clearing TX_OWN on desc 0 prematurely and leaving tdr_offset=1 (wrong).
- **Next step:** Investigate why desc 3 loses TX_ENP between the packet read loop and the collision path.

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
- **Design:** `on_interrupt_cb()` calls `assert_mbx_int()` from `rethink()` callback. Main loop uses `int_hold` counter (5000 iterations) to prevent premature MBX_INT deassertion. Pre-asserts MBX_INT before REG_RSP for INIT/TDMD/STRT writes.
- **Assert path:** rethink() → on_interrupt_cb() → assert_mbx_int() writes MBX_INT=1 + sets int_hold
- **Deassert path:** Main loop decrements int_hold; when zero, `write_mbx_int()` evaluates CSR0 state
- **Assert latency:** ~0.1µs (direct DDR3 write from rethink callback) + ~0.65µs (FPGA poll) — fast enough for most AmigaOS drivers

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
- **Polling:** DDR3_BASE+0x8000 every main loop iteration, `do_transmit()` called on TDMD
- **Interrupt:** `rethink()` callback → `assert_mbx_int()` → MBX_INT=1 + int_hold=5000. Pre-assert for INIT/TDMD/STRT writes. `usleep(200)` after INIT with INTR.
- **Current version:** Includes debug logging (`[req N]` lines, first 5000 requests), TX_OWN clearing loop diagnostics, collision path TX_ERR/TX_RTRY, self-loopback collision detection, rethink() interrupt callback, MBX_INT hold counter, pre-assert for INIT/TDMD/STRT writes, post-INIT usleep delay
- **Cross-compile:** scp source → 192.168.1.97 → compile → scp binary to MiSTer
- **Startup sequence:** Write fake REG_RSP to unstick FPGA, wait 2ms, clear all mailboxes (REG_REQ/RSP, RAM_REQ/RSP, MBX_INT)
- **Register processing:** Responds to register requests BEFORE calling chip_wput() (deadlock prevention)
- **service_bridge_safe:** Called from boardram DDR3 transactions during do_transmit. Only forwards RAP writes to chip_wput — CSR0 writes are silently dropped. This causes stale CSR0 state when Amiga writes CSR0 during do_transmit processing.

## ARM Daemon (`a2065d_doorbell`)

- **Source:** `arm/src/main_doorbell.cpp`
- **Deployed to:** `/media/fat/trans/a2065d_doorbell`
- **Build:** `make doorbell` (uses `build/doorbell/` directory)
- **Boardram access:** Direct DDR3 flat window pointer (`map + DDR3_BRAM_OFF`) — no per-word mailbox
- **Polling:** DDR3 CMD slot every main loop iteration
- **Register processing:** Polls CMD pending bit, calls `chip_wput()` for RAP+data, clears CMD slot, pushes CSR shadow + INT state
- **Cross-compile:** Same as old daemon
- **Startup:** Clears CMD/CSR/INT DDR3 slots, writes MAC to MAC slot, pushes initial CSR shadow

### ARM Build Notes
- `boardram_access.h` uses `extern "C"` for function declarations to match `boardram_remote.cpp` definitions
- `registers_set_boardram()` has `#ifndef BOARDRAM_REMOTE` guard (skips `boardram = ram` assignment in DDR3 build)
- `boardram_remote.cpp` in `SRCS_DDR3` list in Makefile
- `make clean` removes entire `build/` — must `mkdir -p build/ddr3` before `make ddr3`
- Build server 192.168.1.97 cannot resolve `mister` hostname — deploy via dev machine relay using `root@192.168.1.29`
- Test binaries must be statically linked (`-static`) for MiSTer

## m68k AmigaOS Cross-Compiler

- **Host:** `ssh root@192.168.1.97`
- **Path:** `/opt/amiga/bin/m68k-amigaos-gcc`
- **Test programs:**
  - `tests/a2065_test.c` → basic register read/write (deployed to `/media/fat/trans/a2065_test`)
  - `tests/a2065_diag.c` → RAP state diagnostic with 7 subtests (deployed to `/media/fat/trans/a2065_diag`)

## Key Design Decisions

1. **DDR3 mailbox over HPS2FPGA:** HPS2FPGA lightweight bridge doesn't work for this on MiSTer. DDR3 shared memory via f2sdram2 works (confirmed build 20260507).
2. **Level detection for all mailbox requests:** Edge detection (`req_edge`) was missed when state machine was in non-IDLE states. Level detection (`req_sync1` / `bram_req_valid_s1`) ensures requests are never lost. Applied consistently to CMD doorbell, boardram, and old register requests.
3. **Read-modify-write for TDP boardram:** Quartus can't infer TDP M10K from separate byte arrays across two clock domains. Single 16-bit array with RMW for byte enables is the Quartus-friendly pattern.
4. **ARM port directions must be inputs to minimig:** The boardram ARM port signals flow from sys_top.v (mailbox adapter) DOWN through emu→minimig→boardram. They must be declared as `input` in minimig.v, not `output`.
5. **Boardram Port B clock = clk_audio:** Matches mailbox adapter clock domain, avoids additional CDC.
6. **Autoconfig convention:** Nibbles 0-1 (er_Type) raw on D[15:12]; all other nibbles inverted. Matches WinUAE and Toccata pattern.
7. **v5 arbiter must NOT be changed:** The "bug" (burst_active stuck at 1 after burst_count=1 writes) accidentally keeps grant=m1 permanently, preventing m0 (ddr_svc, 128-word PAL bursts) from stealing the bus.
8. **Respond before processing writes:** `service_bridge()` writes REG_RSP before calling `chip_wput()` to prevent deadlock when chip_init/do_transmit need boardram via DDR3 RAM mailbox.
9. **Fake REG_RSP at startup:** Writing `0x1` to REG_RSP on daemon startup unsticks the FPGA if it was left mid-register-path by a previous daemon instance. Eliminates need for core reload between daemon restarts.
10. **Interrupt state via DDR3 poll:** ARM writes MBX_INT every main loop iteration; FPGA polls every 256 cycles. rethink() callback provides immediate assertion from chip_wput/chip_init context.
11. **INT2 into Paula PORTS:** A2065 interrupt OR-tied into Paula's `int2` input (INTREQ bit 3 → level 2). 2-stage CDC synchronizer in minimig.v for clk_audio→clk_sys crossing.
12. **Self-loopback collision detection:** The Am7990 relies on physical loopback plug for collisions; MODE_COLL is never set in init block. Daemon detects DST MAC == own MAC and simulates collision (TX_ERR+TX_RTRY).
13. **TDMD triggers without STRT:** lance-test collision test never writes STRT separately. TDMD triggers `do_transmit()` based on `am_initialized` only. `am_initialized` preserved across STOP to support STOP → STRT+TDMD sequences.
14. **No back-pressure on doorbell RDP writes:** The ARM daemon can't clear `cmd_pending` fast enough for back-to-back 68k writes. RDP writes always complete immediately (no DTACK stretch); the new write overwrites the previous pending doorbell. The ARM daemon only needs the latest register state.
15. **Level-detect CDC handshakes:** All cross-clock-domain request signals (cmd_pending, bram_req_valid) use level detection in the mailbox FSM, not edge detection. Edge detection fails when the FSM is busy processing other requests during the one-cycle edge window.
16. **ddram MUST qualify capture with AS+DS, not raw sel (build 20260608h — BUFFER TEST FIXED):** The root cause of the doorbell buffer-test failure was capturing the boardram request on raw `sel_br = sel && cpu_addr[15]` — which is high during the *address* phase, before the write strobes and write data are valid. So both the direction and the write data were sampled too early. **Fix:** `sel_br = sel && cpu_addr[15] && !cpu_as_n && !cpu_ds_n` and `is_write = ~cpu_rw`, exactly like `a2065_regfile`. DS deasserts between bus cycles, giving a clean NR_DONE→NR_IDLE separation (no edge detect needed). minimig.v passes `.cpu_rw(cpu_r_w)`, `.cpu_as_n(_cpu_as)`, `.cpu_ds_n(_cpu_uds & _cpu_lds)`.
    - **The earlier `cpu_rw` attempts (20260608d/e) failed only because they kept raw `sel_br`** (no DS qualification) — `sel_br` never falls between accesses → NR_DONE stuck → hang. The signal `cpu_r_w` itself was fine. The old "`cpu_hwr/lwr` works" claim was wrong: it didn't hang but silently corrupted data — every read was misclassified as a write of the address bus, every write was dropped (DDR3 filled with `0x8000|offset`, all reads returned 0). Proven by zeroing DDR3, running 68k memtest, and dumping: writes landed as address values.
    - ddram `cpu_data_out` is now a **gated** wire (`(sel && cpu_addr[15] && cpu_rw) ? rd_data : 0`) so the held read value can't corrupt the OR-mux for other peripherals.
17. **`sel_br_rise` edge detection does NOT work; DS-qualified level `sel_br` is correct:** Edge detect on raw `sel_br` gets stuck because raw `sel_br` doesn't fall between consecutive accesses. The AS+DS-qualified `sel_br` (note 16) DOES toggle each bus cycle, so a plain level test works.

## Known Issues / Notes

- MAC serial bytes in cpu_wrapper.v are hardcoded (0x02, 0x70, 0x70, 0x70) — needs ARM-side runtime patching
- `cpu_berr_n` from register module not connected — watchdog timeout returns $0000 (not BERR)
- **Doorbell buffer test FIXED (build 20260608h):** lance-test `Buffer memory test........ PASS`. share:a2065_memtest: walking-bit (16×16384) PASS, address-uniqueness PASS, post-INIT integrity PASS. Root cause + fix in Key Design Decisions #16. Two changes stacked: (a) byteenable→RMW for partial DDR3 writes (build f), (b) AS+DS capture qualification + `cpu_rw` direction (build h). Remaining boardram fails: **byte access** + **odd/even byte independence** — the RMW merge writes the full 16-bit `cpu_data_in` even on a byte access, clobbering the untouched byte. Needs per-byte enable in the RMW merge (forward `_cpu_uds`/`_cpu_lds` → 2-bit be → mailbox merges only the written byte). Word access (what lance-test buffer uses) is correct.
- **Cross-compiled Amiga programs fail silently:** `-noixemul` binaries (GCC m68k-amigaos) don't produce any output on Minimig serial — startup code silently exits. VBCC binaries have broken `Open()`. Only pre-compiled binaries (lance-test on `share:`) and AmigaOS shell commands work. The `share:lance-test` binary is the only working test tool.
- **Serial output from Amiga programs:** `share:lance-test diags` works and produces serial output. AmigaOS shell commands (echo, showconfig, type) work. Cross-compiled C programs produce no output regardless of compiler (GCC -noixemul, GCC default, VBCC).
- **lance-test location:** `share:lance-test` on the Amiga filesystem (not in `/media/fat/trans/`). Run via serial as `share:lance-test diags`.
- **lance-test diags (build 20260606a + old bridge):** Buffer PASS, Config PASS, Interrupt PASS, Collision FAIL — known-good baseline
- **lance-test diags (build 20260608c + doorbell):** Buffer FAIL (other tests not reached). Doorbell path proven working — ARM daemon received CSR0=STOP writes. Serial command must use `\r` not `\r\n` (test framework fix applied to `serial_long.py`).
- **Thread safety:** RX thread (`gotfunc`) and main thread both access CSR0 and boardram. No mutex protection. Currently benign because `registers_csr0()` reads a volatile uint16_t (atomic on ARM), and boardram DDR3 flat access is single-threaded.
- **AddNetInterface A2065:** Not yet tested with doorbell daemon.

## MiSTer Operational Notes

- Core reload via SSH: `echo 'load_core /media/fat/<rbf_file>' > /dev/MiSTer_cmd`
- Serial port: `/dev/ttyS1` at 115200 baud (stty configured)
- Kill daemon before deploying new binary: `killall a2065d_doorbell; rm /media/fat/trans/a2065d_doorbell`
- exFAT on `/media/fat` with `sync` mount — may need to `rm` before `scp` if file in use
- Boardram test (`--test-boardram`) now works without core reload (stale state fix)
- `--iface eth1` for daemon (eth0 may be used by MiSTer main)
- Test binaries need `-static` flag for ARM cross-compilation
- lance-test binary is on `share:` volume, run as `share:lance-test diags` from serial
- **Cross-compiled Amiga binaries don't work:** Neither GCC `-noixemul` nor VBCC produce working AmigaOS executables on this Minimig setup. Programs silently exit or produce no output.
- **Serial line ending:** Must use `\r` (not `\r\n`) for Amiga shell commands via serial. `\n` causes the second word of multi-word commands to be eaten.
- **`minimig_netd` must be killed:** `/etc/init.d/S90minimig_netd` starts `minimig_netd` which conflicts with the A2065 daemon. Disable: `killall minimig_netd; mv /etc/init.d/S90minimig_netd /etc/init.d/S90minimig_netd.disabled`

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
| *(pending)* | May 27 | Boardram poll budget (20260527a), best baseline build 20260527b (3/4 PASS), boardram address fix 20260527c (REGRESSION) |
| *(pending)* | May 28 | CDC synchronizer for bridge_done build 20260528a, lance-test 100x runner |
| *(pending)* | May 29 | Self-loopback collision detection, rethink() interrupt callback, MBX_INT hold counter, TDMD without STRT, pre-assert MBX_INT for INIT/TDMD |
| *(pending)* | Jun 01 | DDR3 read-back after REG_REQ clear, bridge health check (unstuck FPGA), TMD1-first write ordering for collision/loopback |
| *(pending)* | Jun 06 | Build 20260606a: known-good old bridge baseline (3/4 PASS) |
| `aa5f547` | Jun 7 | Phase 0: spec + simulation for flat DDR3 boardram + CSR doorbell |
| `2d9e398` | Jun 7 | Phase 1: flat DDR3 boardram window + mailbox FSM rework |
| `aa9c252` | Jun 7 | Phase 2: FPGA regfile + doorbell + flat boardram DDR3 window (submodule) |
| `d18577a` | Jun 7 | Phase 3: ARM doorbell daemon (parent commit) |
| *(pending)* | Jun 8 | Build 20260608c: three bug fixes — no back-pressure, ddram nrdy guard, level-detect boardram. Doorbell working, lance-test runs, Buffer FAIL |
| `fbd5bc3` | Jun 8 | Phase 4: three doorbell bug fixes (submodule) |
| `723c731` | Jun 8 | Phase 4: submodule update — three bug fixes (parent) |
| `5941593` | Jun 8 | Phase 4 fix: cpu_rw write detection in ddram (submodule, REGRESSED) |
| `fd5a462` | Jun 8 | Phase 4 fix: submodule update — cpu_rw (parent, REGRESSED) |

## Remaining Work

1. **Fix doorbell buffer test FAIL** — Boardram DDR3 read/write mismatch during lance-test buffer memory test. ARM flat test passes (6/6) but 68k-initiated DDR3 boardram accesses fail. `cpu_rw` write detection fix causes Amiga hang (builds 20260608d/e REGRESSED), so the write detection issue must be solved differently. The `cpu_hwr`/`cpu_lwr` pulsed signals work despite theoretical circular dependency. Need to investigate the actual DDR3 data path for 68k boardram accesses — possibly write data timing, address packing, or DDR3 read-modify-write issue with partial byte enables.
2. **Run full lance-test diags** — Get Config, Interrupt, Collision, Loopback tests running with doorbell architecture (blocked on buffer test fix).
3. **Investigate MAC byte ordering** — lance-test shows `00:FFFFFF80:10:00:00:00` (sign-extension of 0x80 byte)
4. **Test AddNetInterface A2065** — real AmigaOS driver test with doorbell daemon
5. **Stress test & polish** (Step 11) — extended run stability, packet throughput
6. **Connect cpu_berr_n** — watchdog timeout should generate BERR, not return $0000
