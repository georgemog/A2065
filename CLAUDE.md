# A2065 Project Memory

## Project Overview

Full hardware emulation of the Commodore A2065 ZorroII Ethernet card for the Minimig FPGA core on MiSTer (DE10-Nano, Cyclone V). Uses standard AmigaOS A2065 drivers — no custom Amiga-side software. Port of Amiberry's `a2065.cpp` (Toni Wilen, 2009) from emulator-space to real hardware.

**Target hardware:** AMD Am7990 LANCE Ethernet controller, Commodore ZorroII card (64KB address space, 32KB boardram, manufacturer ID 0x0202, product ID 0x70, MAC OUI 00:80:10).

## Architecture Split

- **FPGA fabric:** Autoconfig, 32KB boardram, register bridge (DTACK-stretch handshake with ARM)
- **ARM Linux daemon:** Am7990 CSR state machine, TX/RX ring walker, raw Ethernet socket (AF_PACKET)
- **Bridge:** HPS2FPGA mmap'd registers for FPGA↔ARM communication

## Remote Build Environment

- **Build host:** `nshearman@192.168.1.63`
- **Quartus:** `/opt/altera/17.0/quartus/bin/quartus_sh` (must use full PATH)
- **Build command:** `cd ~/Development/Minimig-AGA_MiSTer && /opt/altera/17.0/quartus/bin/quartus_sh --flow compile Minimig`
- **Build logs:** `~/Development/Minimig-AGA_MiSTer/logs/YYYYMMDDx_build.txt`
- **Build output:** `~/Development/Minimig-AGA_MiSTer/output_files/Minimig.rbf`
- **Build time:** ~25 minutes wall, ~55 minutes CPU (6 cores)

## Directory Structure

```
A2065/
├── CLAUDE.md               This file
├── README.md               Project overview, build instructions, status table
├── DESIGN.md               Full architecture document (359 lines)
├── IMPLEMENTATION_PLAN.md   11-step implementation plan with verification criteria (655 lines)
├── .gitmodules             Minimig-AGA_MiSTer submodule
├── arm/                    ARM Linux daemon (full Amiberry port)
│   ├── Makefile            Native, cross-compile, test, deploy targets
│   ├── include/
│   │   ├── a2065_bridge.h  HPS2FPGA bridge register layout, mmap helpers
│   │   └── a2065_types.h   CSR0/TX/RX bit definitions, card identity constants
│   └── src/                registers.cpp, rings.cpp, mac.cpp, ethernet.cpp, bridge.cpp, crc32.cpp
├── fpga/                   Verilog + sim + constraints
│   ├── rtl/                a2065_top.v, a2065_autoconfig.v, a2065_registers.v, a2065_boardram.v
│   ├── sim/                tb_autoconfig.v, tb_boardram.v (iverilog, both passing)
│   └── constraints/        a2065.sdc (timing constraints)
├── Minimig-AGA_MiSTer/     Git submodule — upstream Minimig core (modified for A2065)
│   └── rtl/A2065/          A2065 files placed for Quartus integration (boardram variant differs)
├── reference/              Amiberry source files for reference during porting
├── docs/                   (empty — planned for Step 10)
└── tests/                  (empty — planned for Step 10)
```

## Minimig Core Modifications

### Files Modified in Minimig-AGA_MiSTer submodule:
- **cpu_wrapper.v** — A2065 autoconfig nibbles added (type=0xC1, product=0x70, mfr=0x0202), base address latched from 0xE80048, `a2065_ena` driven from `~ac_a2065`. Nibbles 0-1 raw (er_Type), nibbles 2+ inverted.
- **gary.v** — `sel_a2065 = a2065_ena && cpu_address_in[23:16]==a2065_base`
- **minimig.v** — `a2065_boardram` instantiated, data mux OR-tied into CPU data bus
- **files.qip** — Added `rtl/A2065/a2065_boardram.v`
- **Minimig.sdc** — Added boardram multicycle constraints (see Timing section below)

### Key Difference: fpga/rtl/ vs Minimig-AGA_MiSTer/rtl/A2065/
The Minimig submodule's `a2065_top.v` does NOT instantiate boardram (it's in minimig.v instead). The boardram in the submodule also lacks the ARM read port output — ARM side not yet connected.

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
| **8** | **FPGA chip register bridge + DTACK stretch** | **TODO** |
| **9** | **Integration (ARM + FPGA on MiSTer)** | **TODO** |
| **10** | **Stress test & polish** | **TODO** |

## Quartus Build History

### Build 20260427 (Minimig.sdc with `ram_rd[*]` filter)
- **Result:** SUCCESS, 0 errors, 83 warnings
- **Timing:** Setup +0.125ns, Hold +0.225ns (both passing but tight)
- **SDC issue:** `ram_rd[*]` multicycle path filter didn't match — Quartus inferred `ram[]` as `altsyncram:ram_rtl_0` (M10K block RAM), absorbing `ram_rd` into the megafunction hierarchy. Also `[*]` doesn't match a `reg [15:0]` scalar register.
- **Synthesis warning:** `ram_addr_b` assigned but never read (a2065_boardram.v:43) — ARM port tied to zeros, harmless.

### Build 20260427b (same, second run)
- Same results

### Build 20260427c (fixed SDC — `a2065_boardram_inst|*` to/from)
- **Result:** SUCCESS, 0 errors, 77 warnings (down from 83)
- **SDC fix:** Changed `-to {ram_rd[*]}` to `-to {a2065_boardram_inst|*}` — ram_rd SDC warnings eliminated (3→0)
- **Timing:** Setup +0.005ns (very tight, was +0.125ns), Hold +0.250ns (improved from +0.225ns)
- **Note:** Broader wildcard changed fitter optimization, setup margin essentially zero. Monitor closely in Steps 8-9.

### Build 20260427d (boardram disabled for testing)
- Boardram disabled, tested on MiSTer — black screen
- Pointed to autoconfig nibbles being wrong (not boardram)

### Build 20260427e (autoconfig nibble fix — er_Type raw, not inverted)
- **Result:** SUCCESS, 0 errors, 86 warnings
- **Fix:** Nibbles 0-1 (er_Type) changed from inverted to raw values, matching Toccata pattern
  - Nibble 0: `4'b0011` → `4'b1100` (er_Type high = 0xC = Zorro-II)
  - Nibble 1: `4'b1110` → `4'b0001` (er_Type low = 0x1 = 64KB)
  - All other nibbles were already correctly inverted
- **Root cause:** Zorro II autoconfig bus returns er_Type (nibbles 0-1) non-inverted on D[15:12], all other nibbles inverted. WinUAE `expamem_read()` confirms: addresses 0x00/0x02 returned raw, everything else inverted.
- **Timing:** Setup +0.146ns, Hold +0.247ns (both improved from 20260427c)
- **Build time:** 25:45 wall, 61:30 CPU
- **Tested on MiSTer:** AmigaOS boots, `showconfig` shows A2065 correctly:
  - `Commodore (West Chester) A 2065 Ethernet: Prod=514/112($202/$70) (@$EA0000 64KB)`

## Timing Constraints (Minimig.sdc)

The A2065 boardram constraints added at lines 20-27:
```tcl
# A2065 boardram: 1-cycle synchronous BRAM read latency
# Quartus infers ram[] as altsyncram (ram_rtl_0), absorbing ram_rd into the
# block RAM — so ram_rd is no longer a separate register.  Constrain the full
# hierarchy through the inferred M10K instead.
set_multicycle_path -from {emu|minimig|a2065_boardram_inst|*} \
                    -to   {emu|minimig|a2065_boardram_inst|*} -setup 2
set_multicycle_path -from {emu|minimig|a2065_boardram_inst|*} \
                    -to   {emu|minimig|a2065_boardram_inst|*} -hold 1
```

Also added yc_out chroma LUT multicycle constraints (lines 29-34) to fix timing degradation caused by boardram BRAM routing congestion.

## Common Quartus Warnings (all benign)

- **Synthesized away RAM nodes** (`nRam_rtl_0` in fx68k/nanoRom) — unused portions
- **Open-drain buffers removed** (`SD_SCK`, `SD_MOSI`, `HDMI_I2C_SCL`) — MiSTer routes through HPS
- **Pins stuck at VCC/GND** (`SDRAM_nCS=GND`, `SDRAM_CKE=VCC`) — HPS handles SDRAM control
- **40 hierarchies connectivity warnings** — standard for large Minimig design

## Git History

| Commit | Date | Description |
|--------|------|-------------|
| `54fe2bb` | Apr 25 | Initial commit: full project skeleton |
| `445e101` | Apr 25 | Step 3: CSR state machine + MAC init block bug fix |
| `820609d` | Apr 25 | Step 4: Descriptor ring walker + TX auto-padding fix |
| `e8a4cbf` | Apr 25 | Step 5: Full daemon with sim bridge |
| `82b41be` | Apr 25 | Step 6: FPGA autoconfig (sim + Quartus) |
| `632e78c` | Apr 26 | Step 7: FPGA boardram (sim + Quartus) |
| *(pending)* | Apr 27 | Autoconfig er_Type nibble fix (inverted → raw for nibbles 0-1) |

## Known Issues / Notes

- MAC serial bytes in cpu_wrapper.v are hardcoded (0x02, 0x70, 0x70, 0x70) — will need ARM-side runtime patching
- ARM bridge port on boardram is tied to zeros in the Minimig submodule — not yet connected to HPS2FPGA bridge
- `a2065_top` and `a2065_registers` modules exist in fpga/rtl/ but are NOT yet wired into the Minimig core (Step 8)
- Tight setup slack (+0.146ns on emu PLL clock after build 20260427e) — improved but still worth monitoring
- yc_out chroma LUT timing was degraded by boardram BRAM routing congestion — addressed with multicycle constraints
- **Autoconfig convention (confirmed):** Nibbles 0-1 (er_Type) must be stored raw in `autocfg_data`; all other nibbles stored inverted. Matches WinUAE `expamem_read()` behavior and Toccata pattern in cpu_wrapper.v.
