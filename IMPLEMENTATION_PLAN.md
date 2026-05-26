# A2065 Implementation Plan

## Guiding Principles

- Each step produces something testable independently.
- ARM work comes before FPGA work — more of it can be done on the dev machine.
- FPGA steps build incrementally: autoconfig first, then boardram, then registers.
- No step is "done" until its verification criteria pass.

---

## Step 0: Foundation — Bridge Protocol & Shared Types

**Goal:** Define the memory layout shared between FPGA and ARM. Both sides
compile against this header. Nothing runs yet but all later work depends on it.

### Deliverables

- `arm/include/a2065_bridge.h` — bridge register offsets (DATA, ADDR, RW,
  NEW_REQ, DONE, RESULT), MAC shadow (BRIDGE_MAC_BASE), interrupt registers
  (INT_SET, INT_CLR), boardram offset, mmap helpers for /dev/mem
- `arm/include/a2065_types.h` — CSR0/CSR3/mode bit constants, TX/RX descriptor
  bits, card identity (mfr=0x0202, prod=0x70, OUI=00:80:10), chip register
  offsets, RAM_SIZE/MAX_PACKET_SIZE

### Verification

```bash
# Compiles cleanly on both x86 (dev machine) and ARM (cross-compile)
gcc -I arm/include -c arm/include/a2065_bridge.h -o /dev/null
arm-none-linux-gnueabihf-gcc -I arm/include -c arm/include/a2065_bridge.h -o /dev/null
```

### Done When

- [x] Header compiles with zero warnings on both targets
- [x] All bridge register offsets documented with byte-exact layout
- [x] All CSR0/TX/RX bit constants present and match a2065.cpp

---

## Step 1: ARM — MAC Translation Unit

**Goal:** Port `mungepacket()` and `dofakemac()` from a2065.cpp.
Test it on the dev machine with synthetic packets — no hardware needed.

### Deliverables

- `arm/src/mac.cpp` — `mungepacket()`, `dofakemac()`, UDP checksum recalc
- `arm/src/mac_test.cpp` — 5 test functions with 6 CHECK assertions
- `arm/Makefile` — native build target for tests

### Key Logic (from a2065.cpp)

```c
// dofakemac: swap at packet offset if matches either MAC
static int dofakemac(uint8_t *packet) {
    if (!memcmp(fakemac, realmac, 6)) return 1;  // no-op if same
    if (!memcmp(packet, fakemac, 6)) { memcpy(packet, realmac, 6); return 1; }
    if (!memcmp(packet, realmac, 6)) { memcpy(packet, fakemac, 6); return 1; }
    return 0;
}

// mungepacket: apply to dst, src, and protocol-specific fields
// type 0x0806 (ARP): fix sender MAC (data+8), target MAC (data+8+10)
// type 0x0800 (IPv4) + proto 17 (UDP) + port 67/68: fix DHCP CHADDR (data+36)
//   then recalculate UDP checksum using pseudo-header
```

### Verification

```bash
cd arm && make test
./build/native/mac_test
# PASS: ARP sender MAC translated (fake->real)
# PASS: ARP target MAC translated (real->fake)
# PASS: DHCP CHADDR translated
# PASS: UDP checksum correct after CHADDR fix
# PASS: non-matching packet unchanged
# PASS: broadcast dst unchanged when src=realmac, real src translated to fake
```

### Done When

- [x] All 6 unit tests pass
- [x] Tested with synthetic ARP and DHCP packets

---

## Step 2: ARM — Raw Ethernet Socket

**Goal:** Send and receive raw Ethernet frames on Linux via AF_PACKET.
Loopback filtering and promiscuous mode. No A2065 logic yet.

### Deliverables

- `arm/src/ethernet.cpp` — socket open/close, send frame, recv callback,
  promiscuous mode, loopback suppression. `#ifdef __linux__` guarded with
  stubs for macOS native builds.
- `arm/src/ethernet_test.cpp` — socket open, promiscuous, loopback suppression,
  send/recv, frame size tests

### Key Logic (packet filter from gotfunc2 in a2065.cpp)

```c
// Drop: too short (< 20 bytes)
// Drop: unicast not addressed to us (unless promiscuous)
// Drop: multicast not in LADRF (simplified: allow all)
// Drop: src==dst==realmac (hairpin)
// Drop: dst==broadcast and src==realmac (our own broadcast echo)
```

### Verification

```bash
# Requires Linux (AF_PACKET). Not in Makefile test target.
# Run manually on a Linux host or veth pair:
g++ -std=c++11 -Wall -I include -o ethernet_test src/ethernet_test.cpp
./ethernet_test --iface eth1
# PASS: socket opens and binds on interface
# PASS: valid socket fd obtained
# PASS: interface index resolved
# PASS: host MAC address retrieved
# PASS: promiscuous mode receives frames
# PASS: own broadcast echo loopback suppressed
# PASS: frames received on eth1
# PASS: large frames received without error
# PASS: frame sizes 14-1514 all send correctly
```

### Done When

- [x] socket opens and binds on any named interface
- [x] promiscuous mode flag works
- [x] own-broadcast loopback suppression verified
- [x] frame sizes 14–1514 bytes all send/recv correctly
- [x] `#ifdef __linux__` guards allow macOS native builds

---

## Step 3: ARM — CSR State Machine

**Goal:** Port the CSR register emulation from a2065.cpp (`chip_wput`,
`chip_wget`). Drive it with a test harness that simulates what the A2065
driver does during init.

### Deliverables

- `arm/src/registers.cpp` — CSR state, `chip_wput()`, `chip_wget()`
- `arm/src/csr_test.cpp` — 9 test functions with ~20 CHECK assertions

### Init Sequence the Driver Performs

```
1. Write CSR0 = STOP (0x0004)
2. Write CSR1 = init_block_addr_lo
3. Write CSR2 = init_block_addr_hi
4. Write CSR0 = INIT | STRT (0x0003)
5. Poll CSR0 until IDON (0x0100) set — timeout ~100ms
6. CSR0 should have: IDON | STRT | TXON | RXON
```

### Init Block (24 bytes in boardram)

```
offset 0:  mode (uint16)
offset 2:  MAC byte 1 (byte-swapped pairs)
offset 4:  MAC byte 3
offset 6:  MAC byte 5
offset 8:  LADRF word 0
offset 10: LADRF word 1
offset 12: LADRF word 2
offset 14: LADRF word 3
offset 16: RDR (rx descriptor ring: 3-bit rlen | 24-bit rdra | 000)
offset 20: TDR (tx descriptor ring: 3-bit tlen | 24-bit tdra | 000)
```

### Verification

```bash
./build/native/csr_test
# PASS: STOP clears all state
# PASS: chip not initialized after STOP
# PASS: IDON set after INIT completes
# PASS: STRT set
# PASS: TXON set after STRT
# PASS: RXON set after STRT
# PASS: chip initialized after INIT+STRT
# PASS: am_rdr_rlen = 2^2 = 4 for ring size N=2
# PASS: am_tdr_tlen = 2^2 = 4 for ring size N=2
# PASS: am_rdr_rdra address matches init block value
# PASS: am_tdr_tdra address matches init block value
# PASS: chip_wget(CSR0) returns IDON|STRT|TXON|RXON after init
# PASS: chip_wget(CSR88) returns chip ID word (non-zero)
# PASS: chip_wget(CSR89) returns 0x3003
# PASS: INIT completes with init block at non-zero offset
# PASS: mode register read back as 0
# PASS: LADRF read back as 0
# PASS: RAP readback returns last written value (masked to 7 bits)
```

### Done When

- [x] All CSR tests pass
- [x] Simulated driver can complete init without polling timeout

---

## Step 4: ARM — Descriptor Ring Walker

**Goal:** Port `do_transmit()` and `gotfunc2()` ring walking logic.
Test with synthetic boardram contents.

### Deliverables

- `arm/src/rings.cpp` — `do_transmit()`, `gotfunc()`, ring state
- `arm/src/crc32.cpp` — CRC-32 computation for Ethernet FCS
- `arm/src/rings_test.cpp` — 8 test functions with 17 CHECK assertions

### TX Test Pattern

```
1. Allocate fake boardram (32KB buffer)
2. Write init block (step 3 format), call chip_init()
3. Write TX descriptor: OWN=1, STP=1, ENP=1, buf_size=-(60), addr=0x100
4. Write 60 bytes of fake frame at boardram[0x100]
5. Call do_transmit()
6. Verify: transmitbuffer contains the 60 bytes
7. Verify: TX descriptor OWN cleared
8. Verify: CSR0 has TINT set
```

### RX Test Pattern

```
1. Write RX descriptor: OWN=1, buf_size=-(1024)
2. Call gotfunc() with a 100-byte fake frame
3. Verify: RX descriptor OWN cleared
4. Verify: boardram contains the frame bytes
5. Verify: rmd3 = 104 (100 + 4 CRC bytes)
6. Verify: CSR0 has RINT set
```

### Verification

```bash
./build/native/rings_test
# PASS: TX single descriptor, correct length
# PASS: TX correct bytes in transmitbuffer
# PASS: TX OWN cleared after transmit
# PASS: TX TINT set in CSR0
# PASS: TX chained: 80 bytes from two 40-byte descriptors
# PASS: TX chained: correct byte sequence
# PASS: RX OWN cleared
# PASS: RX STP and ENP set
# PASS: RX frame bytes written to boardram
# PASS: RX rmd3 = 104 (100 + 4 CRC bytes)
# PASS: RX RINT set in CSR0
# PASS: CRC32 bytes appended to RX frame (non-zero)
# PASS: RX frame too short (< 20 bytes) dropped
# PASS: no RINT for too-short frame
# PASS: RX unicast not-for-me dropped
# PASS: RX own broadcast echo dropped
# PASS: RX broadcast from other host accepted
```

### Done When

- [x] All ring tests pass
- [x] CRC32 bytes appended to RX frames correctly
- [x] Chained TX (STP in one descriptor, ENP in another) handled

---

## Step 5: ARM — Full Daemon (Simulated Bridge)

**Goal:** Assemble all ARM pieces into `a2065d` daemon. Use a simulated
bridge (POSIX shared memory) instead of real /dev/mem so it runs on the
dev machine without MiSTer hardware.

### Deliverables

- `arm/src/bridge.cpp` — /dev/mem mmap, register read/write, boardram access
- `arm/src/bridge_sim.cpp` — simulates bridge via POSIX shm (`/dev/shm/a2065_bridge`)
- `arm/src/main.cpp` — daemon startup, signal handling, main loop
- `arm/src/bridge_test.cpp` — 11 test functions with 17 CHECK assertions:
  boardram R/W, init block, TX/RX via bridge, bridge FSM protocol,
  chip ID registers, interrupt callback
- `arm/src/bridge_client.cpp` — connects to running daemon via shm,
  simulates A2065 driver init sequence, tests TX/RX paths

### Main Loop

```
1. mmap /dev/mem (or sim shm) for bridge + boardram
2. Read host NIC MAC, apply Commodore OUI prefix
3. Write MAC bytes [2:5] into autoconfig shadow RAM
4. Start RX thread (blocks on AF_PACKET recvfrom)
5. Main loop:
   a. Poll bridge NEW_REQ register
   b. If set: service chip register access, write result + DONE
   c. Every 1000 iterations: check_transmit()
```

### Verification

```bash
# Automated integration test (does not require daemon running)
cd arm && make test
./build/native/bridge_test
# PASS: Boardram byte write/read pattern 0..255
# PASS: Boardram word write/read 0x1234
# PASS: Boardram word write/read 0xABCD
# PASS: Boardram full 32KB wrap pattern
# PASS: Boardram address wraps at 32KB
# PASS: Chip initialized after INIT+STRT
# PASS: RDR ring len = 4
# PASS: TDR ring len = 4
# PASS: RDR address = RX_RING_BASE
# PASS: TDR address = TX_RING_BASE
# PASS: CSR0 IDON set after init
# PASS: CSR0 TXON set after init
# PASS: CSR0 RXON set after init
# PASS: TX 60 bytes via bridge boardram
# PASS: TX bytes match boardram contents
# PASS: TX OWN cleared after transmit
# PASS: TX TINT set
# PASS: RX OWN cleared
# PASS: RX STP and ENP set
# PASS: RX frame bytes in boardram match input
# PASS: RX rmd3 = 104 (100 data + 4 CRC)
# PASS: RX RINT set
# PASS: Bridge FSM DONE set after write
# PASS: RAP set to 5 via bridge FSM
# PASS: Bridge FSM DONE set after read
# PASS: Bridge read CSR0: IDON set
# PASS: Bridge read CSR0: TXON set
# PASS: Bridge read CSR0: RXON set
# PASS: INIT+STRT via bridge FSM: chip initialized
# PASS: CSR0 read via bridge: IDON set
# PASS: CSR0 read via bridge: TXON set
# PASS: CSR88 chip ID non-zero via bridge
# PASS: CSR89 = 0x3003 via bridge
# PASS: Interrupt callback fired after TINT + INEA
```

### Daemon Sim-Mode Test (manual)

```bash
# Terminal 1: start daemon in sim mode
./a2065d --sim --iface veth0 --verbose

# Terminal 2: run client
./bridge_client --sim
# PASS: STOP accepted
# PASS: INIT accepted, init block parsed
# PASS: STRT accepted, IDON set
# PASS: TX frame sent via bridge
# PASS: TINT interrupt signalled
```

### Done When

- [x] Daemon starts without errors on dev machine
- [x] Init sequence completes (IDON set)
- [x] TX path: frame from ring reaches raw socket
- [x] RX path: frame from raw socket reaches ring
- [x] Ctrl+C cleanly shuts down daemon

---

## Step 6: FPGA — ZorroII Autoconfig

**Goal:** Implement autoconfig ROM response in Verilog. Test in simulation.
No DTACK stretching or boardram yet.

### Deliverables

- `fpga/rtl/a2065_autoconfig.v` — autoconfig state machine, ROM nibbles
- `fpga/sim/tb_autoconfig.v` — testbench simulating AmigaOS autoconfig reads
- `fpga/sim/Makefile` — iverilog simulation target

### Autoconfig State Machine

```verilog
States:
  WAIT      waiting for access to 0xE80000
  RESPOND   drive autoconfig nibble on D[15:12]
  CONFIGURED  card has a base address, autoconfig done
  SHUTUP    card silenced (SHUTUP write received)

Inputs:
  cpu_addr[23:0]   68k address bus
  cpu_rw           1=read, 0=write
  cpu_as_n         address strobe
  cpu_data_in[15:0] data bus (for SHUTUP write, address write)

Outputs:
  data_out[15:0]   autoconfig nibble on [15:12] (inverted)
  dtack_n          assert when responding
  card_base[7:0]   configured address >> 16 (e.g., 0xE9 for 0xE90000)
  card_configured  1 after base address written
```

### ROM Nibble Table (A2065, 32 nibbles)

```verilog
// autoconfig ROM stores RAW nibble values
// Output is inverted: {~rom[nibble_idx], 12'hFFF}
// MAC bytes [2:5] loaded into nibbles 12-19 from runtime inputs
rom[ 0] = 4'hC;  // er_Type high
rom[ 1] = 4'h1;  // er_Type low
rom[ 2] = 4'h7;  // er_Product high
rom[ 3] = 4'h0;  // er_Product low → product=0x70
rom[ 4] = 4'h0;  // er_Flags high
rom[ 5] = 4'h0;  // er_Flags low
rom[ 6] = 4'hF;  // reserved
rom[ 7] = 4'hF;  // reserved
rom[ 8] = 4'h0;  // er_Manufacturer high high: 0x0
rom[ 9] = 4'h2;  // er_Manufacturer high low:  0x2
rom[10] = 4'h0;  // er_Manufacturer low high:  0x0
rom[11] = 4'h2;  // er_Manufacturer low low:   0x2 → 0x0202
rom[12] = mac_byte2[7:4];  // er_SerialNumber byte 0 high
rom[13] = mac_byte2[3:0];  // er_SerialNumber byte 0 low
// ... MAC bytes 3-5 in nibbles 14-19
// nibbles 20-31: zeros (inverted to 0xF)
```

### Minimig Integration Note

In the actual Minimig integration, autoconfig is handled inline in
`cpu_wrapper.v` with a case statement returning pre-inverted nibbles
(nibbles 2+) and raw er_Type nibbles (0-1). MAC bytes are hardcoded
(0x02, 0x70, 0x70, 0x70) and need to be replaced with ARM-provided values
in Step 9.

### Simulation Testbench Sequence

```verilog
// Simulate AmigaOS autoconfig read sequence:
for (i = 0; i < 32; i++) begin
    drive_read(24'hE80000 + i*2);
    capture_nibble = dut.data_out[15:12];
end
// Write base address
drive_write(24'hE80048, base_lo);
// Write SHUTUP
drive_write(24'hE8004C, 8'h00);
// Verify card_configured=1, card_base=expected
```

### Verification

```bash
cd fpga/sim && make sim_autoconfig
# Expected:
# 32 nibbles read, match expected inverted values
# card_configured asserted after base address write
# card_base = configured value
# no response after SHUTUP
```

### Done When

- [x] All simulation tests pass in iverilog
- [x] Waveform (VCD) shows correct nibble sequence
- [x] SHUTUP write silences autoconfig correctly
- [x] Synthesises with zero critical warnings in Quartus

---

## Step 7: FPGA — boardram Window

**Goal:** Map the 32KB boardram through HPS2FPGA bridge. The 68k can
read/write boardram at full bus speed — no ARM involvement needed.

### Deliverables

- `fpga/rtl/a2065_boardram.v` — dual-port BRAM (standalone version)
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_boardram.v` — BRAM variant with hi/lo
  byte arrays for clean M10K inference
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_ddram.v` — DDR3 boardram variant (future)
- `Minimig-AGA_MiSTer/rtl/A2065/ddr_arbiter.v` — DDR3 bus arbiter (future)
- `fpga/sim/tb_boardram.v` — read/write cycles, cross-port tests

### Address Decoding

```verilog
// card_base is the byte configured by autoconfig (e.g., 0xE9)
// boardram region: card_base<<16 + 0x8000 .. card_base<<16 + 0xFFFF
wire sel_boardram = (cpu_addr[23:16] == card_base) &&
                    (cpu_addr[15] == 1'b1);    // bit 15 set = offset >= 0x8000
```

### Boardram Variants

1. **BRAM** (current): 32KB dual-port M10K. Minimig version splits into
   `ram_hi[0:16383]` and `ram_lo[0:16383]` for byte-lane write enables.
   ARM Port B is tied to zeros in current integration.

2. **DDR3** (future): `a2065_ddram.v` with 5-state FSM and `ddr_arbiter.v`
   for bus arbitration. Uses `nrdy` for DTACK stretch. Not yet wired.

### Verification

```bash
# In simulation:
# PASS: write/read at card+0x8000 (first word)
# PASS: write/read at card+0xFFFE (last word)
# PASS: access to card+0x7FFE returns 0 (below boardram)
# PASS: multiple offsets persist correctly
# PASS: sel=0 → data_out=0 (safe for OR-tie)
# PASS: ARM write, 68k read back (cross-port)
# PASS: 68k write, ARM read back (cross-port)
```

### Done When

- [x] Simulation passes all boardram read/write cases
- [x] Timing constraints met (Quartus timing analysis clean)
- [x] On MiSTer: boardram window accessible, AmigaOS reads card correctly

---

## Step 8: FPGA — Chip Register Bridge + DTACK Stretch

**Goal:** Implement the DTACK-stretch state machine for RAP/RDP register
accesses. The 68k is held until ARM daemon responds.

### Deliverables

- `fpga/rtl/a2065_registers.v` — address decode for RAP/RDP, DTACK stretch
  state machine, bridge register write/read. Two modes via BRIDGE_LOCAL
  parameter.
- `fpga/sim/tb_registers.v` — 9 tests with parameterised ARM delay:
  write/read at 0/10/100 cycle delays, back-to-back, watchdog timeout,
  wrong address/configured, wrong card_base
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_registers.v` — bug-fixed local CSR:
  temp variable for CSR0, write-gated RAP, corrected direction sense

### Bridge Register Layout (8 bytes at bridge base)

```
offset 0x00 (2B): BRIDGE_REG_DATA     data from 68k (writes) / to 68k (reads)
offset 0x02 (1B): BRIDGE_REG_ADDR     chip reg offset (0=RDP, 0x02=RAP)
offset 0x03 (1B): BRIDGE_REG_RW       0=read, 1=write
offset 0x04 (1B): BRIDGE_REG_NEW_REQ  FPGA writes 1 when access pending
offset 0x05 (1B): BRIDGE_REG_DONE     ARM writes 1 when result ready
offset 0x06 (2B): BRIDGE_REG_RESULT   ARM writes read result here
```

Additional bridge registers:
```
offset 0x08 (6B): BRIDGE_MAC_BASE     MAC[0:5] (ARM writes before boot)
offset 0x10 (1B): BRIDGE_INT_SET      ARM writes 1 to assert INT2
offset 0x11 (1B): BRIDGE_INT_CLR      ARM writes 1 to deassert INT2
```

### BRIDGE_LOCAL Mode

The module has two operating modes via the `BRIDGE_LOCAL` parameter:

**BRIDGE_LOCAL=1 (current Minimig state):**
- Local 128-entry CSR register file in BRAM
- Responds within 1 clock cycle, no DTACK stretch
- `regs_nrdy` is always 0
- Used for MiSTer bringup before ARM bridge is wired

**BRIDGE_LOCAL=0 (target for Step 9):**
- 4-state FSM: IDLE → WAIT_ARM → RELEASE → IDLE (or ST_BERR)
- Holds 68k via `regs_nrdy` until ARM responds
- Watchdog (700000 cycles ≈ 25ms at ~28MHz) triggers BERR on timeout

### DTACK Stretch State Machine (BRIDGE_LOCAL=0)

```verilog
IDLE:
  if (sel_chipreg && !cpu_as_n && !cpu_ds_n)
    → write {addr, rw, data} to bridge_reg
    → set NEW_REQ = 1
    → assert regs_nrdy (hold bus)
    → goto WAIT_ARM

WAIT_ARM:
  if (bridge_done == 1)
    → latch result onto data bus (if read)
    → assert bridge_done_clr (clears DONE)
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

### Simulation Test

```bash
cd fpga/sim && make sim_registers
# PASS: write to RAP captured in bridge register (0-cycle delay)
# PASS: read from RDP returns ARM result (0-cycle delay)
# PASS: write to RDP with 10-cycle ARM delay
# PASS: read from RAP with 100-cycle ARM delay
# PASS: back-to-back write then read handled correctly
# PASS: watchdog fires after 200 cycles with no ARM response (BERR)
# PASS: access outside register region — no bridge request
# PASS: card_configured=0 — no bridge request
# PASS: wrong card_base — no bridge request
```

### Verification on MiSTer (BRIDGE_LOCAL=1)

```bash
# With BRIDGE_LOCAL=1, local CSR register file responds directly.
# The ARM bridge is not involved — this tests FPGA decode only.
showconfig
# Expected: Commodore (West Chester) A 2065 Ethernet: Prod=514/112($202/$70) (@$EA0000 64KB)
```

### Done When

- [x] Simulation passes at 0, 10, 100 cycle ARM response delays
- [x] Watchdog fires correctly (BERR) if ARM does not respond
- [x] Meets Quartus timing constraints
- [x] On MiSTer: AmigaOS boots, `showconfig` lists A2065 card correctly
      (`Commodore (West Chester) A 2065 Ethernet: Prod=514/112($202/$70) (@$EA0000 64KB)`)

---

## Step 9: Integration — ARM Daemon + FPGA Core

**Goal:** Replace simulated bridge with real /dev/mem. Run full A2065
daemon against the FPGA on MiSTer. Use existing A2065 AmigaOS driver.

### Prerequisites

- Steps 1–8 all passing
- Minimig FPGA bitstream built with A2065 boardram + registers integrated
- ARM daemon built for ARMv7 (arm-none-linux-gnueabihf-g++)
- A2065.device on Amiga side (from Commodore NDK or WHDLoad)

### Current State

ARM daemon code is complete and verified (45 unit tests pass: 6 MAC + ~20 CSR
+ 17 rings + 17 bridge integration). FPGA integration in Minimig uses
BRIDGE_LOCAL=1 (local CSR, no ARM bridge).

### Remaining FPGA-Side Work

1. **Add Avalon-MM slave** in Platform Designer for A2065 bridge registers
   + boardram. Map to an address within the HPS2FPGA lightweight bridge window.

2. **Wire a2065_registers bridge signals** (`bridge_done`, `bridge_result`)
   from HPS2FPGA to the `a2065_regs_inst` in `minimig.v`. Currently tied to
   `1'b0` and `16'd0`.

3. **Wire a2065_boardram Port B** (`arm_addr`, `arm_data_in`, `arm_wr`,
   `arm_sel`) to HPS2FPGA. Currently tied to zeros.

4. **Switch BRIDGE_LOCAL** from 1 to 0 in `minimig.v`:
   ```verilog
   a2065_registers #(.BRIDGE_LOCAL(0)) a2065_regs_inst (...
   ```

5. **Replace hardcoded MAC** in `cpu_wrapper.v` with ARM-provided values,
   or wire `a2065_autoconfig.v` module with runtime MAC inputs instead of
   the inline case statement.

6. **Update BRIDGE_PHYS_BASE** in `a2065_bridge.h` to the actual FPGA address
   assigned by Platform Designer (currently `0xFF200000`).

7. **Wire `regs_nrdy`** output from `a2065_regs_inst` into the Minimig DTACK
   stretch logic (OR'd with Gayle IDE wait). Currently unconnected `()`.

8. **Wire `arm_int_req`** to the bridge interrupt register output for
   ZorroII INT2 delivery. Currently a static input to `a2065_top.v`.

### Integration Procedure

```bash
# 1. Build ARM daemon for MiSTer
make -C arm CXX=arm-none-linux-gnueabihf-g++ CROSS=1

# 2. Deploy
scp arm/build/arm/a2065d root@mister:/usr/local/bin/

# 3. On MiSTer: start daemon
a2065d --iface eth0 --verbose &

# 4. Boot Minimig core, mount A2065.device on Amiga

# 5. On Amiga shell:
Mount A2065/DEVICE=A2065.device UNIT=0
AddNetInterface A2065
ifconfig A2065 192.168.1.200 netmask 255.255.255.0

# 6. Test
ping 192.168.1.1
```

### Verification

```
On Amiga:
  ping 192.168.1.1              → replies received
  ping 192.168.1.1 -c 100       → 0% packet loss

On MiSTer:
  tcpdump -i eth0 -n host 192.168.1.200
  → ARP requests from 00:80:10:xx:xx:xx visible
  → ICMP echo requests visible

a2065d log output:
  7990: INIT+START. [CSR0 values]
  7990->DST:ff:ff:ff:ff:ff:ff SRC:00:80:10:xx:xx:xx E=0806 S=60
  7990<-DST:00:80:10:xx:xx:xx SRC:... E=0806 S=60
```

### Done When

- [ ] `ping` works (ICMP echo request/reply)
- [ ] ARP resolves correctly
- [ ] 100 pings, 0% packet loss
- [ ] DHCP address acquisition works (Roadshow / AmiTCP)
- [ ] Transfer test: `fetch http://...` downloads successfully

---

## Step 10: Stress Test & Polish

**Goal:** Validate stability, fix edge cases, document final state.

### Tests

| Test | Method | Pass Criterion |
|------|--------|----------------|
| Long-running ping | `ping -c 10000` | 0% loss |
| Large transfer | `wget` 100MB file | completes, no corruption |
| Multiple TCP sessions | AWeb browsing | no crashes |
| Driver reload | `Remount A2065` x 10 | no hang |
| Daemon restart | kill + restart a2065d | driver reconnects |
| Promiscuous mode | enable, verify extra frames seen | LADRF filter works |
| DHCP | Roadshow DHCP acquire | address assigned correctly |
| Bridge disconnect | pull eth0 cable | graceful no-crash |

### Documentation

- [ ] Update DESIGN.md with any changes from implementation
- [ ] Write `arm/README.md` — build and deploy instructions
- [ ] Write `fpga/README.md` — Quartus integration instructions
- [ ] Add `tests/` shell scripts for automated verification

### Done When

- [ ] All stress tests pass
- [ ] No memory leaks (valgrind on daemon sim mode)
- [ ] Quartus P&R report: no timing violations
- [ ] README complete enough for someone else to build from scratch

---

## Dependency Graph

```
Step 0 (bridge types)
  ├─► Step 1 (MAC translation)
  ├─► Step 2 (raw ethernet)
  └─► Step 3 (CSR state machine)
          └─► Step 4 (descriptor rings)
                  └─► Step 5 (full daemon, sim bridge)
                          │
Step 6 (FPGA autoconfig)  │
  └─► Step 7 (boardram)   │
        └─► Step 8 (chip register bridge)
                  └─► Step 9 (integration) ◄─────── Step 5
                          └─► Step 10 (stress test)
```

Steps 1, 2, 3 can run in parallel after Step 0.
Steps 6, 7, 8 can run in parallel with Steps 1–5.
Step 9 requires both Step 5 and Step 8.
Step 10 requires Step 9.

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| DTACK stretch too slow for driver timeout | Medium | High | Measure actual ARM response latency in Step 8 sim; tune watchdog |
| HPS2FPGA bridge throughput insufficient for 10Mbps | Low | Medium | boardram is direct-mapped, only regs go via bridge |
| Quartus P&R fails timing on Cyclone V | Low | Medium | Register signals, add pipeline stage in DTACK FSM |
| A2065.device not available for testing | Low | High | Use Roadshow demo disk or download from Aminet |
| AF_PACKET requires root on MiSTer | Known | Low | MiSTer runs as root already |
| MAC hardcoded in cpu_wrapper.v not patched before autoconfig | Medium | High | Arm daemon must write before FPGA releases reset; enforce via startup ordering |
| DDR3 boardram variant has timing/arbitration issues | Medium | Medium | Test with BRAM first; DDR3 variant is optional enhancement |
| BRIDGE_LOCAL→bridge mode switch breaks existing Minimig timing | Medium | High | The regs_nrdy path must be carefully wired to avoid spurious DTACK holds |
