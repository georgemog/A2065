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
│  │  0x4000–0x4003  chip registers (RAP/RDP)   │──► FPGA bridge  │
│  │  0x4004–0x7FFF  unused                     │                  │
│  │  0x8000–0xFFFF  card RAM (32KB boardram)   │──► HPS window   │
│  └────────────────────────────────────────────┘                  │
└───────────────────────┬──────────────────────────────────────────┘
                        │ HPS2FPGA Lightweight Bridge
                        ▼
┌──────────────────────────────────────────────────────────────────┐
│  ARM Linux (HPS)                                                 │
│                                                                  │
│  a2065d daemon                                                   │
│    ├─ CSR register state machine (chip_wput/chip_wget)           │
│    ├─ TX ring walker (do_transmit)                               │
│    ├─ RX ring walker (gotfunc)                                   │
│    ├─ MAC translation (mungepacket)                              │
│    └─ Raw Ethernet socket (AF_PACKET)                            │
│                                                                  │
│  /dev/mem → boardram window (32KB)                               │
│  Bridge register region (8 bytes)                                │
└───────────────────────┬──────────────────────────────────────────┘
                        │ Raw Ethernet (AF_PACKET)
                        ▼
                  MiSTer eth0 / eth1
```

---

## Memory Map

### ZorroII Card Space (after autoconfig)

Autoconfig places the card at a 64KB-aligned address (typically 0xE90000).
All offsets below are relative to card base.

| Offset       | Size  | Region          | Owner       |
|--------------|-------|-----------------|-------------|
| 0x0000–0x3FFF | 16KB | Unused / ROM    | FPGA        |
| 0x4000        | 2B   | RDP (data port) | FPGA→bridge |
| 0x4002        | 2B   | RAP (addr ptr)  | FPGA→bridge |
| 0x4004–0x7FFF | 16KB | Unused          | —           |
| 0x8000–0xFFFF | 32KB | boardram        | Shared RAM  |

### HPS2FPGA Bridge Mapping

| ARM Physical Addr  | Size  | Purpose                          |
|--------------------|-------|----------------------------------|
| BRIDGE_BASE + 0x00 | 2B    | RAP/RDP register shadow (write)  |
| BRIDGE_BASE + 0x02 | 2B    | register read result (ARM→FPGA)  |
| BRIDGE_BASE + 0x04 | 1B    | address of access (0x00 or 0x02) |
| BRIDGE_BASE + 0x05 | 1B    | R/W flag (0=read, 1=write)       |
| BRIDGE_BASE + 0x06 | 1B    | DONE flag (ARM sets when ready)  |
| BRIDGE_BASE + 0x07 | 1B    | NEW_REQ flag (FPGA sets on access)|
| BRIDGE_BASE + 0x8000 | 32KB | boardram (direct mapped)         |

BRIDGE_BASE is the ARM-side physical address of the HPS2FPGA lightweight
bridge window allocated for the A2065 card.

---

## FPGA Architecture

### Modules

```
a2065_top.v
├── a2065_autoconfig.v    ZorroII autoconfig ROM + state machine
├── a2065_registers.v     RAP/RDP access → bridge + DTACK stretch
└── a2065_boardram.v      32KB boardram window (direct HPS2FPGA access)
```

### ZorroII Autoconfig Sequence

1. At reset, FPGA listens at 0xE80000 for autoconfig reads.
2. FPGA responds with A2065 autoconfig nibbles (inverted on bus).
3. AmigaOS writes configured base address → FPGA latches it.
4. AmigaOS writes SHUTUP → autoconfig done, card mapped at configured addr.
5. FPGA decodes all subsequent accesses to card space.

### DTACK Stretch State Machine (for chip registers only)

```
IDLE
  │  68k access to RAP or RDP address
  ▼
HOLD_BUS     assert /DTACK_OVERRIDE (hold 68k)
  │  write {addr, rw, data} to bridge NEW_REQ register
  │  ARM polls NEW_REQ, processes, writes result + DONE=1
  ▼
RELEASE      release /DTACK, present data on bus
  │
  ▼
IDLE
```

boardram accesses (0x8000–0xFFFF) bypass this entirely — they go directly
through the HPS2FPGA bridge at full speed with standard DTACK timing.

### Autoconfig ROM Values (A2065)

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

NOTE: all values are inverted on the ZorroII data bus (hardware convention).
The FPGA outputs ~nibble on D[7:4] for each autoconfig read cycle.

The A2065 driver reads MAC bytes [2:5] from er_SerialNumber at OpenDevice
time. ARM must write MAC bytes into the bridge's autoconfig shadow RAM before
Amiga boots (or before the autoconfig scan runs).

### Alternative Autoconfig Implementation (fpga-toccata Reference)

The Minimig core includes a reference ZorroII card implementation (fpga-toccata)
that uses a different autoconfig approach worth considering:

**Toccata Approach (embedded in cpu_wrapper.v):**
- Autoconfig decode and response embedded directly in cpu_wrapper.v
- Case statement returns nibbles based on `chip_addr[6:1]`
- Base address written to `toccata_base` register at 0x48
- Autoconfig enabled via `ac_toccata` bit (set to 1 at reset)
- Values hardcoded in case statement (no runtime patching)

**A2065 Approach (a2065_autoconfig.v module):**
- Separate module with ROM array[0:31] for 32 nibbles
- MAC byte inputs allow runtime patching from ARM daemon
- Outputs inverted nibbles on D[15:12] (bus convention)
- Self-contained, reusable module
- Explicit state machine for base address latch and SHUTUP

### Comparison

| Aspect | Toccata Approach | A2065 Approach |
|--------|------------------|----------------|
| **Location** | Embedded in cpu_wrapper.v | Separate module |
| **Data storage** | Case statement | ROM array |
| **MAC patching** | Not needed (no MAC) | 4 inputs from ARM |
| **Base address** | toccata_base register | card_base output |
| **Integration complexity** | Simpler (no module) | More complex (requires wiring) |
| **Runtime flexibility** | Low (hardcoded values) | High (dynamic MAC patching) |

**Why A2065 Keeps Separate Module:**

The A2065 requires dynamic MAC address patching (bytes [2:5] in er_SerialNumber) by the ARM daemon before the Amiga boots. The Toccata approach of hardcoded values in cpu_wrapper.v cannot support this runtime patching.

However, the **integration pattern** from Toccata (enable bit, case statement for nibble response, base address latch) can be adopted when connecting `a2065_autoconfig.v` to cpu_wrapper.v, providing a consistent ZorroII card interface across both implementations.

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
├── Makefile
├── include/
│   ├── a2065_bridge.h     bridge register layout + mmap helpers
│   └── a2065_daemon.h     daemon API
└── src/
    ├── main.cpp           argument parsing, startup, signal handling
    ├── bridge.cpp         /dev/mem mmap, bridge register access
    ├── registers.cpp      chip_wget / chip_wput (CSR state machine)
    ├── rings.cpp          do_transmit / gotfunc (descriptor rings)
    ├── ethernet.cpp       AF_PACKET socket, send/recv, promiscuous mode
    └── mac.cpp            mungepacket / dofakemac / UDP checksum fix
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

Flow: AF_PACKET recv → filter → mungepacket → walk RX ring →
      find OWN=1 descriptor → copy into boardram buffer →
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

When RINT or TINT is set in CSR0 and INEA is set, the daemon signals
the FPGA via a bridge interrupt register. The FPGA asserts the ZorroII
INT2 line to the 68k, which triggers AmigaOS level-2 interrupt processing.

---

## Key Design Decisions

### Why DTACK Stretch for Registers Only

boardram (32KB) is in the HPS2FPGA bridge window — the 68k accesses it
at full bus speed with no ARM involvement needed. Only the 4-byte chip
register region (RAP + RDP) requires ARM mediation.

DTACK stretch holds the 68k bus until ARM responds. The 68k has no
DTACK timeout — it waits indefinitely. Amiga driver software timeouts
are typically 1–100ms, giving the ARM daemon ample time to respond.

### Polling vs Interrupt on ARM Side

The ARM daemon uses a tight busy-loop on the NEW_REQ flag for register
accesses (latency-critical) and a separate thread for RX packet delivery.
TX is polled from the RX thread at ~1ms intervals, same as Amiberry.

### Single ZorroII Board

Only one A2065 emulated at a time. The FPGA autoconfig chain needs
/CFGIN → /CFGOUT passed through for other ZorroII devices.

---

## Integration with Minimig Core

### Required FPGA Changes

1. Add `a2065_top.v` to the Minimig RTL hierarchy.
2. Connect ZorroII expansion bus signals to `a2065_top`.
3. Add HPS2FPGA bridge window for A2065 (64KB, 64KB-aligned).
4. Wire A2065 /INT2 output to Minimig interrupt controller.
5. Add A2065 enable/disable configuration bit (OSD menu).

### Required ARM Changes

1. Start `a2065d` daemon from MiSTer main binary before core load.
2. Daemon reads host NIC MAC, patches autoconfig ROM shadow, opens bridge.
3. Daemon runs until core is unloaded or system exits.

---

## Differences from Amiberry Implementation

| Aspect | Amiberry | This project |
|--------|----------|-------------|
| boardram | process heap | /dev/mem HPS2FPGA window |
| chip regs | synchronous function call | async bridge + DTACK stretch |
| packet I/O | WinPcap / libpcap | AF_PACKET raw socket |
| threading | emulator hsync callback | dedicated daemon threads |
| interrupt | emulator IRQ injection | FPGA bridge interrupt register |
| save state | save_a2065 / restore_a2065 | not required (FPGA resets clean) |
| ARIADNE | supported | out of scope (A2065 only) |
