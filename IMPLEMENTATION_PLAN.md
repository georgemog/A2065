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

- `arm/include/a2065_bridge.h` — bridge register offsets, boardram base,
  flag definitions, mmap helpers for /dev/mem
- `arm/include/a2065_types.h` — uint8_t/uint16_t/uint32_t aliases,
  CSR0/TX/RX bit constants (ported from a2065.cpp defines)

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
- `arm/src/mac_test.cpp` — unit tests: ARP packet, DHCP packet, regular IP
- `arm/Makefile` — native build target for tests

### Key Logic (from a2065.cpp)

```c
// dofakemac: swap at packet offset if matches either MAC
static int dofakemac(uint8_t *packet) {
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
./mac_test
# Expected output:
# PASS: ARP sender MAC translated
# PASS: ARP target MAC translated
# PASS: DHCP CHADDR translated
# PASS: UDP checksum correct after CHADDR fix
# PASS: non-matching packet unchanged
# PASS: broadcast dst unchanged when src=realmac
```

### Done When

- [x] All 6 unit tests pass
- [x] Valgrind clean (no memory errors)
- [x] Tested with a real DHCP capture (wireshark .pcap fed in as bytes)

---

## Step 2: ARM — Raw Ethernet Socket

**Goal:** Send and receive raw Ethernet frames on Linux via AF_PACKET.
Loopback filtering and promiscuous mode. No A2065 logic yet.

### Deliverables

- `arm/src/ethernet.cpp` — socket open/close, send frame, recv callback,
  promiscuous mode, loopback suppression
- `arm/src/ethernet_test.cpp` — send a ping, verify frame received back

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
./ethernet_test --iface eth0
# Expected:
# Opened AF_PACKET socket on eth0 (index N)
# Promiscuous mode enabled
# Sent 60-byte ARP probe
# Received 60-byte frame (our own ARP filtered out? yes)
# Received N frames in 5 seconds
```

Test on dev machine with a loopback bridge (veth pair) to avoid needing
MiSTer hardware at this stage.

### Done When

- [x] socket opens and binds on any named interface
- [x] promiscuous mode flag works
- [x] own-broadcast loopback suppression verified (send ARP broadcast,
      confirm it is NOT delivered back to recv callback)
- [x] frame sizes 14–1514 bytes all send/recv correctly

---

## Step 3: ARM — CSR State Machine

**Goal:** Port the CSR register emulation from a2065.cpp (`chip_wput`,
`chip_wget`). Drive it with a test harness that simulates what the A2065
driver does during init.

### Deliverables

- `arm/src/registers.cpp` — CSR state, `chip_wput()`, `chip_wget()`
- `arm/src/csr_test.cpp` — simulate A2065 driver init sequence

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
./csr_test
# Expected:
# PASS: STOP clears all state
# PASS: INIT reads init block, extracts MAC/mode/ring pointers
# PASS: STRT sets TXON + RXON
# PASS: IDON set after INIT completes
# PASS: am_rdr_rlen = 2^N as expected for ring size N
# PASS: am_tdr_tdra address matches init block value
# PASS: chip_wget(CSR0) returns IDON|STRT|TXON|RXON after init
# PASS: chip_wget(CSR88) returns chip ID word
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
- `arm/src/rings_test.cpp` — populate boardram descriptors, verify TX/RX

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
2. Call gotfunc2() with a 100-byte fake frame
3. Verify: RX descriptor OWN cleared
4. Verify: boardram contains the frame bytes
5. Verify: rmd3 = 104 (100 + 4 CRC bytes)
6. Verify: CSR0 has RINT set
```

### Verification

```bash
./rings_test
# PASS: TX single descriptor, correct bytes in transmitbuffer
# PASS: TX OWN cleared after transmit
# PASS: TX TINT set in CSR0
# PASS: RX frame written to boardram buffer
# PASS: RX OWN cleared
# PASS: RX RINT set in CSR0
# PASS: RX frame too short (< 20 bytes) dropped
# PASS: RX unicast not-for-me dropped
# PASS: RX own broadcast echo dropped
# PASS: RX multicast accepted (LADRF != 0)
```

### Done When

- [x] All ring tests pass
- [x] CRC32 bytes appended to RX frames correctly
- [x] Chained TX (STP in one descriptor, ENP in another) handled

---

## Step 5: ARM — Full Daemon (Simulated Bridge)

**Goal:** Assemble all ARM pieces into `a2065d` daemon. Use a simulated
bridge (shared memory file) instead of real /dev/mem so it runs on the
dev machine without MiSTer hardware.

### Deliverables

- `arm/src/bridge.cpp` — /dev/mem mmap, register read/write, boardram access
- `arm/src/main.cpp` — daemon startup, signal handling, main loop
- `arm/src/bridge_sim.cpp` — simulates bridge via POSIX shared memory file

### Main Loop

```
1. mmap /dev/mem (or sim shm) for bridge + boardram
2. Read host NIC MAC, apply Commodore OUI prefix
3. Write MAC bytes [2:5] into autoconfig shadow RAM
4. Start RX thread (blocks on AF_PACKET recvfrom)
5. Main loop:
   a. Poll bridge NEW_REQ register
   b. If set: service chip register access, write result + DONE
   c. Every 1ms: check_transmit()
```

### Simulated Bridge Test

Write a companion `bridge_client.cpp` that:
- Simulates A2065 driver init sequence (writes to bridge registers)
- Sends a frame via the TX ring
- Verifies the frame appears on the raw socket
- Injects a frame on the raw socket
- Verifies it appears in the RX ring

### Verification

```bash
# Terminal 1: start daemon in sim mode
./a2065d --sim --iface veth0 --verbose

# Terminal 2: run client
./bridge_client --sim
# PASS: STOP accepted
# PASS: INIT accepted, init block parsed
# PASS: STRT accepted, IDON set
# PASS: TX frame sent to veth0
# PASS: RX frame injected appears in ring
# PASS: TINT interrupt signalled
# PASS: RINT interrupt signalled
```

### Done When

- [ ] Daemon starts without errors on dev machine
- [ ] Init sequence completes (IDON set)
- [ ] TX path: frame from ring reaches raw socket
- [ ] RX path: frame from raw socket reaches ring
- [ ] Ctrl+C cleanly shuts down daemon

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
  RESPOND   drive autoconfig nibble on D[7:4]
  CONFIGURED  card has a base address, autoconfig done
  SHUTUP    card silenced (SHUTUP write received)

Inputs:
  cpu_addr[23:0]   68k address bus
  cpu_rw           1=read, 0=write
  cpu_as_n         address strobe
  cpu_data[15:0]   data bus (for SHUTUP write, address write)

Outputs:
  data_out[15:0]   autoconfig nibble on [15:12] (inverted)
  dtack_n          assert when responding
  card_base[7:0]   configured base address >> 16 (e.g., 0xE9 for 0xE90000)
  card_configured  1 after base address written
```

### ROM Nibble Table (A2065, 32 nibbles)

```verilog
// autoconfig[n] holds nibble n, output inverted on D[15:12]
// n=0: byte[0] high nibble = 0xC → output ~0xC = 0x3
// n=1: byte[0] low nibble  = 0x1 → output ~0x1 = 0xE
// n=2: byte[1] high nibble = 0x7 → output ~0x7 = 0x8
// n=3: byte[1] low nibble  = 0x0 → output ~0x0 = 0xF
// ... etc.
// nibbles 12–13 (byte[6]) and 14–19 (bytes[7:9]) = MAC bytes patched by ARM
```

ARM writes MAC nibbles via bridge before core loads Amiga ROM.

### Simulation Testbench Sequence

```verilog
// Simulate AmigaOS autoconfig read sequence:
for (i = 0; i < 32; i++) begin
    drive_read(24'hE80000 + i*2);
    capture_nibble = dut.data_out[15:12];
end
// Write base address
drive_write(24'hE80048, base_lo);
drive_write(24'hE8004A, base_hi);
// Write SHUTUP
drive_write(24'hE8004C, 8'h00);
// Verify card_configured=1, card_base=expected
```

### Verification

```bash
cd fpga/sim && make sim_autoconfig
# Expected:
# PASS: 32 nibbles read, match expected inverted values
# PASS: nibbles 12-19 reflect ARM-written MAC bytes
# PASS: card_configured asserted after base address write
# PASS: card_base = 0xE9 (or configured value)
# PASS: no response after SHUTUP
```

### Done When

- [ ] All simulation tests pass in iverilog
- [ ] Waveform (VCD) shows correct nibble sequence
- [ ] SHUTUP write silences autoconfig correctly
- [ ] Synthesises with zero critical warnings in Quartus

---

## Step 7: FPGA — boardram Window

**Goal:** Map the 32KB boardram through HPS2FPGA bridge. The 68k can
read/write boardram at full bus speed — no ARM involvement needed.

### Deliverables

- `fpga/rtl/a2065_boardram.v` — address decode for card+0x8000, DTACK gen,
  bridge window pass-through
- `fpga/sim/tb_boardram.v` — read/write cycles to boardram

### Address Decoding

```verilog
// card_base is the byte configured by autoconfig (e.g., 0xE9)
// boardram region: card_base<<16 + 0x8000 .. card_base<<16 + 0xFFFF
wire sel_boardram = (cpu_addr[23:16] == card_base) &&
                    (cpu_addr[15] == 1'b1);    // bit 15 set = offset >= 0x8000

// Pass-through to HPS2FPGA bridge at fixed ARM physical address
// Bridge offset = cpu_addr[14:0] (15-bit boardram offset)
assign bridge_addr = {boardram_base_arm, cpu_addr[14:0]};
```

### Verification

```bash
# In simulation:
# PASS: write 0xABCD to card+0x8000, read back 0xABCD
# PASS: write 0x1234 to card+0xFFFF, read back 0x1234
# PASS: access to card+0x7FFF does NOT hit boardram (goes to autoconfig/chip reg path)
# PASS: DTACK asserted within 2 clock cycles for boardram access

# On hardware (MiSTer, without Amiga driver, just ARM test):
# ARM writes 0xDEAD to bridge+0x8000
# FPGA reads it back via loopback test
```

### Done When

- [ ] Simulation passes all boardram read/write cases
- [ ] Timing constraints met (Quartus timing analysis clean)
- [ ] On MiSTer: ARM can write to boardram window, 68k reads correct value

---

## Step 8: FPGA — Chip Register Bridge + DTACK Stretch

**Goal:** Implement the DTACK-stretch state machine for RAP/RDP register
accesses. The 68k is held until ARM daemon responds.

### Deliverables

- `fpga/rtl/a2065_registers.v` — address decode for RAP/RDP, DTACK stretch
  state machine, bridge register write/read
- `fpga/sim/tb_registers.v` — simulate register read and write cycles with
  ARM response modelled after configurable delay

### Bridge Register Layout (8 bytes at bridge_base)

```
offset 0x00 (2B): data from 68k (for writes) / data to 68k (for reads)
offset 0x02 (1B): address offset within chip (0x00=RDP, 0x02=RAP)
offset 0x03 (1B): R/W (0=read, 1=write)
offset 0x04 (1B): NEW_REQ (FPGA writes 1 when access pending)
offset 0x05 (1B): DONE (ARM writes 1 when result ready, FPGA clears)
offset 0x06 (2B): result data (ARM writes for read responses)
```

### DTACK Stretch State Machine

```verilog
IDLE:
  if (sel_chipreg && !cpu_as_n)
    → write {addr, rw, data} to bridge_reg
    → set NEW_REQ = 1
    → assert dtack_override (hold bus)
    → goto WAIT_ARM

WAIT_ARM:
  if (bridge_reg[DONE] == 1)
    → latch result onto data bus (if read)
    → clear DONE, clear NEW_REQ
    → release dtack_override
    → goto IDLE
  else
    → keep holding bus (68k waits indefinitely)

// Watchdog: if ARM doesn't respond in ~10ms, release with bus error
// (prevents hard lockup if daemon crashes)
```

### Simulation Test

Model ARM response with parameterised delay (0, 1, 10, 100 clock cycles):

```verilog
// For each delay:
// 1. Drive 68k write to RAP (addr 0x4002, data 0x0000)
// 2. Assert AS
// 3. Wait for DTACK_n deassert (FPGA holding bus)
// 4. Model ARM: wait N cycles, write result + DONE=1
// 5. Verify DTACK_n asserts (bus released) within N+2 cycles
// 6. Drive 68k read from RDP (addr 0x4000)
// 7. Verify data[15:0] = ARM's result value
```

### Verification

```bash
cd fpga/sim && make sim_registers
# PASS: write to RAP captured in bridge register
# PASS: 68k held (DTACK not asserted) until DONE set
# PASS: 68k released immediately when DONE set
# PASS: read result from RDP = value ARM wrote
# PASS: watchdog fires after 10ms with no ARM response (bus error)
# PASS: back-to-back register accesses handled correctly
```

### Done When

- [ ] Simulation passes at 0, 1, 10, 100 cycle ARM response delays
- [ ] Watchdog fires correctly if ARM does not respond
- [ ] Meets Quartus timing constraints
- [ ] On MiSTer with ARM dummy responder: 68k can RAP/RDP write/read
      without hanging

---

## Step 9: Integration — ARM Daemon + FPGA Core

**Goal:** Replace simulated bridge with real /dev/mem. Run full A2065
daemon against the FPGA on MiSTer. Use existing A2065 AmigaOS driver.

### Prerequisites

- Steps 1–8 all passing
- Minimig FPGA bitstream built with a2065_top.v integrated
- ARM daemon built for ARMv7 (arm-none-linux-gnueabihf-g++)
- A2065.device on Amiga side (from Commodore NDK or WHDLoad)

### Integration Procedure

```bash
# 1. Build ARM daemon for MiSTer
make -C arm CXX=arm-none-linux-gnueabihf-g++ CROSS=1

# 2. Deploy
scp arm/build/a2065d root@mister:/usr/local/bin/
scp arm/build/a2065d root@mister:/usr/local/bin/

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
| Driver reload | `Remount A2065` × 10 | no hang |
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

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| DTACK stretch too slow for driver timeout | Medium | High | Measure actual ARM response latency in Step 8 sim; tune watchdog |
| HPS2FPGA bridge throughput insufficient for 10Mbps | Low | Medium | boardram is direct-mapped, only regs go via bridge |
| Quartus P&R fails timing on Cyclone V | Low | Medium | Register signals, add pipeline stage in DTACK FSM |
| A2065.device not available for testing | Low | High | Use Roadshow demo disk or download from Aminet |
| AF_PACKET requires root on MiSTer | Known | Low | MiSTer runs as root already |
| MAC[2:5] not written to FPGA before autoconfig scan | Medium | High | Arm daemon must write before FPGA releases reset; enforce via startup ordering |
