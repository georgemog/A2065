# A2065 Ethernet Emulation for Minimig MiSTer

## Overview

Full hardware emulation of the Commodore A2065 ZorroII Ethernet card for the
Minimig FPGA core running on MiSTer. Uses existing standard AmigaOS A2065
drivers — no custom Amiga-side software required.

The A2065 uses the AMD Am7990 LANCE (Local Area Network Controller for Ethernet)
chip. This project emulates it split across FPGA fabric and ARM Linux userspace.

Reference implementation: Amiberry `src/a2065.cpp` (Toni Wilen, 2009).

---

## Hardware Being Emulated

```
Commodore A2065
  ZorroII card, 64KB address space
  AMD Am7990 LANCE Ethernet controller
  Commodore manufacturer ID: 0x0202 (514)
  Product ID: 0x70 (112)
  MAC OUI: 00:80:10 (Commodore-Amiga)
  Card RAM: 32KB at offset 0x8000 within card space
  Chip registers: 4 bytes at offset 0x4000 (RAP + RDP)
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Amiga 68k (FPGA)                                                │
│                                                                  │
│  AmigaOS A2065.device driver                                     │
│    │                                                             │
│    │ ZorroII bus accesses                                        │
│    ▼                                                             │
│  ┌────────────────────────────────────────────┐                  │
│  │  A2065 Address Space (64KB @ autoconfig)   │                  │
│  │  0x0000–0x3FFF  unused                     │                  │
│  │  0x4000–0x4003  chip registers (RAP/RDP)   │──► DDR3 mailbox │
│  │  0x4004–0x7FFF  unused                     │                  │
│  │  0x8000–0xFFFF  card RAM (32KB boardram)   │──► BRAM + DDR3  │
│  └────────────────────────────────────────────┘                  │
│                                                                  │
│  FPGA signal path:                                               │
│    cpu_wrapper.v — autoconfig (inline nibbles)                   │
│    gary.v        — address decode (sel_a2065)                    │
│    a2065_registers.v — DTACK-stretch, bridge handshake           │
│    a2065_boardram.v  — 32KB TDP BRAM (Port A=68k, Port B=ARM)   │
│    a2065_ddr3_mailbox.v — register+boardram DDR3 mailbox adapter │
│    avalon_arbiter.v — round-robin f2sdram2 sharing               │
│    sys_top.v — mailbox instance, arbiter, BRAM port wiring       │
│    minimig.v — boardram + registers instantiation                │
└───────────────────────┬──────────────────────────────────────────┘
                        │ DDR3 shared memory (f2sdram2 Avalon port)
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│  ARM Linux (HPS)                                                 │
│                                                                  │
│  a2065d_ddr3 daemon                                              │
│    ├─ CSR register state machine (chip_wput/chip_wget)           │
│    ├─ TX ring walker (do_transmit)                               │
│    ├─ RX ring walker (gotfunc)                                   │
│    ├─ MAC translation (mungepacket)                              │
│    ├─ CRC32 FCS computation (crc32_compute)                      │
│    ├─ Boardram access via DDR3 mailbox (boardram_remote.cpp)     │
│    └─ Raw Ethernet socket (AF_PACKET)                            │
│                                                                  │
│  /dev/mem mmap → DDR3_BASE (0x1FF00000, 64KB window)            │
│    +0x8000: REG_REQ (FPGA→ARM register request)                  │
│    +0x8008: REG_RSP (ARM→FPGA register response)                 │
│    +0x8010: RAM_REQ (ARM→FPGA boardram request)                  │
│    +0x8018: RAM_RSP (FPGA→ARM boardram response)                 │
└───────────────────────┬──────────────────────────────────────────┘
                        │ Raw Ethernet (AF_PACKET)
                        ▼
                  MiSTer eth0 / eth1
```

---

## Memory Map

### ZorroII Card Space (after autoconfig)

Autoconfig places the card at a 64KB-aligned address (typically 0xEA0000).
All offsets below are relative to card base.

| Offset       | Size  | Region          | Owner       |
|--------------|-------|-----------------|-------------|
| 0x0000–0x3FFF | 16KB | Unused / ROM    | FPGA        |
| 0x4000        | 2B   | RDP (data port) | FPGA→bridge |
| 0x4002        | 2B   | RAP (addr ptr)  | FPGA→bridge |
| 0x4004–0x7FFF | 16KB | Unused          | —           |
| 0x8000–0xFFFF | 32KB | boardram        | Shared RAM  |

### HPS2FPGA Bridge Mapping (DEPRECATED — replaced by DDR3 mailbox)

The HPS2FPGA lightweight bridge was found to be non-functional on MiSTer for this use case. The DDR3 shared-memory mailbox via f2sdram2 is used instead.

### DDR3 Mailbox Mapping (current)

ARM daemon mmaps `/dev/mem` at physical address `0x1FF00000` (64KB window).

| ARM Physical Offset | Avalon Offset | Size | Purpose |
|---------------------|---------------|------|---------|
| +0x8000 | 0x1000 | 8B | REG_REQ: bit[0]=pending, bit[1]=rw, bits[9:2]=addr, bits[25:10]=data |
| +0x8008 | 0x1001 | 8B | REG_RSP: bit[0]=ready, bits[16:1]=result |
| +0x8010 | 0x1002 | 8B | RAM_REQ: bit[0]=pending, bit[1]=rw, bits[16:2]=offset, bits[32:17]=wdata |
| +0x8018 | 0x1003 | 8B | RAM_RSP: bit[0]=ready, bits[16:1]=read_result |

Avalon addresses = ARM physical byte address >> 3 (f2sdram2 uses 64-bit word addressing).

---

## FPGA Architecture

### Two Integration Paths

The project maintains two sets of RTL:

1. **Standalone modules** (`fpga/rtl/`) — self-contained, with `a2065_top.v`
   integrating all submodules. Used for simulation and as the reference design.

2. **Minimig integration** (`Minimig-AGA_MiSTer/rtl/A2065/`) — boardram and
   registers instantiated directly in `minimig.v`; autoconfig handled inline
   in `cpu_wrapper.v`. The `a2065_top.v` in the submodule is NOT used.

### Standalone Module Hierarchy (`fpga/rtl/`)

```
a2065_top.v
├── a2065_autoconfig.v    ZorroII autoconfig ROM + state machine
├── a2065_registers.v     RAP/RDP access → bridge + DTACK stretch
│                           (BRIDGE_LOCAL=1: local CSR; =0: ARM bridge)
└── a2065_boardram.v      32KB dual-port BRAM (68k + ARM)
```

### Minimig Integration Hierarchy

```
cpu_wrapper.v             Autoconfig nibbles (inline case statement)
  └── a2065_base[7:0]    → gary.v address decode
gary.v
  └── sel_a2065          → minimig.v select
minimig.v
  ├── a2065_boardram      TDP BRAM (Port A=68k clk_sys, Port B=ARM clk_audio)
  └── a2065_registers     BRIDGE_LOCAL=0, DTACK-stretch via DDR3 mailbox
sys_top.v (clk_audio domain)
  ├── a2065_ddr3_mailbox  Register + boardram DDR3 mailbox adapter
  └── avalon_arbiter      Round-robin f2sdram2 (ddr_svc m0 + A2065 m1)
Minimig.sv (emu module)
  └── Pass-through for BRAM and bridge ports between sys_top and minimig
```

### Minimig Submodule Files (`Minimig-AGA_MiSTer/rtl/A2065/`)

```
a2065_autoconfig.v    Not used by Minimig (autoconfig in cpu_wrapper.v)
a2065_boardram.v      TDP BRAM with single 16-bit array + RMW byte enables
a2065_registers.v     BRIDGE_LOCAL=0, DDR3 mailbox bridge mode
a2065_ddr3_mailbox.v  Register + boardram DDR3 mailbox state machine
avalon_arbiter.v      2-master round-robin f2sdram2 arbiter
a2065_top.v           Not used by Minimig (instantiation in minimig.v)
```

### Boardram

TDP BRAM (`a2065_boardram.v`) — 32KB true dual-port M10K block RAM.
Single 16-bit wide array with `(* ramstyle = "M10K" *)` attribute.
Byte enables handled via read-modify-write (Quartus-friendly TDP inference).
- Port A: 68k ZorroII bus (clk_sys)
- Port B: ARM daemon via DDR3 mailbox (clk_audio)

The ARM daemon accesses boardram by writing requests to the RAM_REQ DDR3
mailbox location. The FPGA mailbox adapter picks up the request in idle
slots, reads/writes the BRAM Port B, and writes the response to RAM_RSP.

### ZorroII Autoconfig

The autoconfig implementation differs between standalone and Minimig paths:

**Standalone (`a2065_autoconfig.v`):**
- 32-nibble ROM array with runtime MAC byte inputs (`mac_byte2`..`mac_byte5`)
- Raw nibble values stored in `rom[]`, inverted at output: `{~rom[idx], 12'hFFF}`
- Separate state machine for base address latch and SHUTUP
- Allows dynamic MAC patching from ARM daemon

**Minimig integration (`cpu_wrapper.v`):**
- Autoconfig nibbles hardcoded inline in a case statement
- Nibbles 0–1 (er_Type): stored raw (0xC, 0x1) on D[15:12]
- Nibbles 2+: stored pre-inverted
- MAC bytes hardcoded (0x02, 0x70, 0x70, 0x70) — needs ARM-side runtime patching
- Base address latched at register 0x48, enable via `ac_a2065` → `a2065_ena = ~ac_a2065`

**Autoconfig ROM Values (A2065):**

```
Byte  Value   Field
[0]   0xC1    er_Type: ZorroII (0xC0) + 64KB (0x01)
[1]   0x70    er_Product: 112
[2]   0x00    er_Flags
[3]   0x00    reserved
[4]   0x02    er_Manufacturer high: 0x02
[5]   0x02    er_Manufacturer low:  0x02 → mfr = 514 (Commodore-Amiga)
[6]   MAC[2]  er_SerialNumber byte 0 (set by ARM before boot)
[7]   MAC[3]  er_SerialNumber byte 1
[8]   MAC[4]  er_SerialNumber byte 2
[9]   MAC[5]  er_SerialNumber byte 3
[10]  0x00    er_InitDiagVec high (no diagnostic vector)
[11]  0x00    er_InitDiagVec low
[12]  0x00    er_BootNode (not used)
[13]  0x00    reserved
[14]  0x00    reserved
[15]  0x00    reserved
```

NOTE: nibbles 0–1 (er_Type) are returned raw on D[15:12]; all other nibbles
are inverted on the ZorroII bus. This matches WinUAE `expamem_read()` behavior
and the Toccata pattern in cpu_wrapper.v.

The A2065 driver reads MAC bytes [2:5] from er_SerialNumber at OpenDevice
time. ARM must write MAC bytes into the bridge's MAC shadow register
(BRIDGE_MAC_BASE) before Amiga boots.

### Chip Register Bridge + DTACK Stretch

The `a2065_registers.v` module has two operating modes selected by the
`BRIDGE_LOCAL` parameter:

**BRIDGE_LOCAL=1 (current Minimig integration):**
- Local 128-entry CSR register file in BRAM
- Responds combinationally within 1 clock cycle
- No DTACK stretch — 68k sees standard expansion-card timing
- `regs_nrdy` output is always 0
- Allows basic Amiga-side testing before ARM bridge is wired

**BRIDGE_LOCAL=0 (target for ARM bridge mode):**
- 4-state FSM: IDLE → WAIT_ARM → RELEASE → IDLE (or ST_BERR on timeout)
- Holds 68k bus via `regs_nrdy` (OR'd into bridge `nrdy` with Gayle IDE wait)
- FPGA writes bridge registers (data, addr, rw, NEW_REQ=1)
- ARM daemon polls NEW_REQ, processes, writes RESULT + DONE=1
- FPGA latches result, clears DONE, releases DTACK
- Watchdog (default 700000 cycles ≈ 25ms) triggers BERR if ARM doesn't respond

```
IDLE:
  if (sel_chipreg && !cpu_as_n && !cpu_ds_n)
    → write {addr, rw, data} to bridge registers
    → set NEW_REQ = 1
    → assert regs_nrdy (hold bus)
    → goto WAIT_ARM

WAIT_ARM:
  if (bridge_done == 1)
    → latch result onto data bus (if read)
    → clear DONE via bridge_done_clr
    → clear NEW_REQ
    → goto RELEASE
  else if (watchdog >= WATCHDOG_CYCLES)
    → clear NEW_REQ
    → goto ST_BERR

RELEASE:
  assert DTACK (release bus)
  wait for cpu_as_n deassert
  → goto IDLE

ST_BERR:
  assert BERR
  wait for cpu_as_n deassert
  → goto IDLE
```

boardram accesses (0x8000–0xFFFF) bypass this entirely — they go directly
to BRAM (or DDR3) at full speed with standard DTACK timing.

---

## ARM Daemon Architecture

### Source Port from Amiberry

The ARM daemon is a direct port of Amiberry's `a2065.cpp` with:
- pcap/WinPcap → AF_PACKET raw socket
- Process-local memory → /dev/mem mapped HPS2FPGA bridge
- Emulator thread sync → bridge NEW_REQ/DONE polling

### Files

```
arm/
├── Makefile                   native, cross-compile, test, deploy targets
├── include/
│   ├── a2065_bridge.h         bridge register layout, mmap helpers, accessors
│   └── a2065_types.h          CSR0/TX/RX bit constants, card identity, sizes
└── src/
    ├── main.cpp               argument parsing, startup, signal handling, main loop
    ├── bridge.cpp             /dev/mem mmap, bridge register access
    ├── bridge_sim.cpp         POSIX shm simulation of bridge (dev machine testing)
    ├── registers.cpp          chip_wget / chip_wput (CSR state machine)
    ├── rings.cpp              do_transmit / gotfunc (descriptor rings)
    ├── ethernet.cpp           AF_PACKET socket, send/recv, promiscuous mode
    │                          (#ifdef __linux__ guarded; stubs for macOS)
    ├── mac.cpp                mungepacket / dofakemac / UDP checksum fix
    ├── crc32.cpp              CRC-32 for Ethernet FCS (standard reflected poly)
    ├── mac_test.cpp           6 MAC translation tests
    ├── csr_test.cpp           ~20 CSR state machine tests
    ├── rings_test.cpp         17 descriptor ring tests (TX, RX, CRC, chained)
    ├── bridge_test.cpp        17 bridge integration tests (shared mem + FSM protocol)
    ├── bridge_client.cpp      daemon sim-mode client (connects via shm)
    └── ethernet_test.cpp      AF_PACKET tests (requires Linux, not in Makefile)
```

### Main Loop

```
1. mmap /dev/mem (or sim shm) for bridge + boardram
2. Read host NIC MAC, apply Commodore OUI prefix (00:80:10)
3. Set fakemac and realmac in mac.cpp
4. Reset CSR state, set boardram pointer, register callbacks
5. Open AF_PACKET socket on specified interface (unless --sim)
6. Start RX thread (blocks on AF_PACKET recvfrom)
7. Main loop (1µs sleep):
   a. Poll BRIDGE_REG_NEW_REQ
   b. If set: service chip register access, write RESULT + DONE
   c. Every 1000 iterations: poll TX ring (do_transmit)
```

### CSR State Machine (from a2065.cpp)

CSR0 controls chip lifecycle:

```
STOP ──INIT──► initialized ──STRT──► running
  ▲                                    │
  └──────────────STOP──────────────────┘

STOP:  csr[0] = 0x0004  chip halted, all state cleared
INIT:  reads 24-byte init block from boardram (pointed to by CSR1/CSR2)
STRT:  enables RXON + TXON, opens AF_PACKET socket
IDON:  set after successful INIT, triggers interrupt if INEA set
TDMD:  transmit demand — immediately poll TX ring
```

### Descriptor Ring Protocol

#### TX (Transmit)

```
For each descriptor in TX ring:
  tmd0 = buffer address low 16 bits
  tmd1 = [OWN|flags|addr_high_8]
  tmd2 = -(buffer_size)   (two's complement, 16-bit)
  tmd3 = error bits (written back by daemon)

  OWN=1: driver owns descriptor (daemon must not touch)
  STP: start of packet
  ENP: end of packet
  ADD_FCS: append CRC32

Flow: daemon polls TX ring → finds OWN+STP → gathers buffers →
      calls mungepacket → sends via AF_PACKET → sets TINT → interrupt
```

#### RX (Receive)

```
rmd0 = buffer address low 16 bits
rmd1 = [OWN|flags|addr_high_8]
rmd2 = -(buffer_size)
rmd3 = received byte count (written by daemon)

Flow: AF_PACKET recv → filter → mungepacket → append CRC32 →
      walk RX ring → find OWN=1 descriptor → copy into boardram buffer →
      clear OWN → set STP/ENP → write rmd3 → set RINT → interrupt
```

### MAC Translation

The A2065 driver hardcodes Commodore OUI 00:80:10:xx:xx:xx.
The host NIC has a different MAC. mungepacket() translates between them
on every packet in both directions, including inside ARP and DHCP payloads.

```
fakemac = 00:80:10:XX:XX:XX   (presented to Amiga driver)
realmac = 00:80:10:XX:XX:XX   (first 3 bytes forced, last 3 from host NIC)

dofakemac: swap fakemac↔realmac at a given offset in packet
mungepacket: apply to dst MAC, src MAC, ARP sender/target, DHCP CHADDR
             recalculate UDP checksum if DHCP CHADDR was modified
```

### Interrupt Delivery

When RINT or TINT is set in CSR0 and INEA is set, the daemon's `rethink()`
function calls the `on_interrupt` callback. In `main.cpp`, this writes
`BRIDGE_INT_SET = 1` to the bridge, which asserts the ZorroII INT2 line
to the 68k via the `arm_int_req` signal. The FPGA inverts this to active-low:
`assign int2_n = ~arm_int_req`.

---

## Key Design Decisions

### Why DTACK Stretch for Registers Only

boardram (32KB) is in the HPS2FPGA bridge window — the 68k accesses it
at full bus speed with no ARM involvement needed. Only the 4-byte chip
register region (RAP + RDP) requires ARM mediation.

DTACK stretch holds the 68k bus until ARM responds. The 68k has no
DTACK timeout — it waits indefinitely. Amiga driver software timeouts
are typically 1–100ms, giving the ARM daemon ample time to respond.

### BRIDGE_LOCAL Mode for Incremental Testing

The `BRIDGE_LOCAL` parameter in `a2065_registers.v` allows the FPGA to
have a local CSR register file that responds directly without ARM. This
enabled MiSTer bringup (autoconfig verification, showconfig) before the
ARM bridge was wired. The current Minimig integration uses BRIDGE_LOCAL=1.
Switching to BRIDGE_LOCAL=0 is the key FPGA-side task for Step 9.

### Polling vs Interrupt on ARM Side

The ARM daemon uses a tight busy-loop (1µs sleep) on the NEW_REQ flag for
register accesses (latency-critical) and a separate thread for RX packet
delivery. TX is polled from the main loop at ~1000-iteration intervals,
same cadence as Amiberry.

### Single ZorroII Board

Only one A2065 emulated at a time. The FPGA autoconfig chain needs
/CFGIN → /CFGOUT passed through for other ZorroII devices.

---

## Integration with Minimig Core

### Current State (Step 9 — DDR3 mailbox integration)

The A2065 is fully wired into the Minimig core with DDR3 mailbox communication:

**What works:**
- Autoconfig: AmigaOS detects A2065 card correctly via cpu_wrapper.v
- Boardram: 32KB TDP BRAM accessible to 68k at card+0x8000
- Registers: DDR3 mailbox bridge (BRIDGE_LOCAL=0), DTACK-stretch via regs_nrdy
- Data bus: A2065 outputs OR-tied into CPU data bus in minimig.v
- Address decode: gary.v generates `sel_a2065` from `a2065_base`
- DDR3 register mailbox: Full round-trip verified (build 20260507, CSR0=$0004)
- Arbiter: Round-robin f2sdram2 sharing between audio/PAL and A2065 mailbox
- ARM daemon: Full Am7990 CSR state machine + TX/RX ring walker + AF_PACKET

**In progress:**
- Boardram DDR3 mailbox path: ARM↔FPGA boardram access via DDR3 RAM_REQ/RSP
- Mailbox adapter fixes: level-detect for register requests, proper DDR3 wait states
- Boardram TDP rewrite: single 16-bit array for Quartus M10K TDP inference
- minimig.v port direction fix: BRAM ARM port must be input (not output) to minimig

**Known bugs being fixed (build 20260510):**
- Separate byte arrays (`ram_hi`/`ram_lo`) can't be inferred as TDP M10K across two clock domains
- BRAM port directions in minimig.v were reversed (output→input for addr/wdata/wr/be)

---

## Differences from Amiberry Implementation

| Aspect | Amiberry | This project |
|--------|----------|-------------|
| boardram | process heap | TDP BRAM (M10K) + DDR3 mailbox for ARM access |
| chip regs | synchronous function call | DDR3 mailbox + DTACK stretch |
| packet I/O | WinPcap / libpcap | AF_PACKET raw socket |
| threading | emulator hsync callback | dedicated daemon threads |
| interrupt | emulator IRQ injection | FPGA bridge interrupt register |
| save state | save_a2065 / restore_a2065 | not required (FPGA resets clean) |
| ARIADNE | supported | out of scope (A2065 only) |
| FCS/CRC | not computed (pcap strips it) | CRC32 computed and appended to RX frames |
| FPGA↔ARM | N/A (same process) | DDR3 shared-memory mailbox via f2sdram2 |
