# A2065 Architecture Simplification — Flat DDR3 Boardram + CSR Doorbell

**Status:** Verified
**Date:** 2026-06-06
**Verified:** 2026-06-06 — all codebase claims confirmed against source (see §16)
**Supersedes:** DDR3 mailbox register/boardram RPC (build 20260507–20260601c)
**Author:** Design review session

---

## 1. Executive Summary

The current A2065 emulation splits the Am7990 LANCE across FPGA fabric and an
ARM Linux daemon, connected by a **synchronous DDR3 mailbox RPC**. Every 68k
access to the chip registers stalls the bus (DTACK stretch) until the ARM
answers over DDR3, and the ARM reads card RAM one word at a time through a
second mailbox slot. A single 19-state mailbox FSM round-robins register, RAM,
interrupt, and MAC traffic with throttled polling.

This architecture is the root cause of every Step 10 stress-test failure:

| Test | Pass rate | Bottleneck |
|------|-----------|-----------|
| Buffer memory (pure RAM) | 100% | — |
| LANCE config | 97% | init round-trip latency |
| Interrupt | 88% | MBX_INT poll race during `chip_init` |
| Collision | 76% | TDMD writes invisible to daemon |
| Loopback | 53% | TMD propagation timing |
| **ALL PASS** | **~53%** | bridge-health flood |

The pure-memory test passes 100%; every timed/asynchronous test fails. The
failures are bridge artifacts, not LANCE-emulation bugs.

This document proposes realigning the design to the model the ARM code was
**originally written for** — Amiberry's flat in-process shared memory with
synchronous register semantics — by making two changes:

1. **Boardram lives in DDR3.** Both the 68k (via FPGA f2sdram windowing) and the
   ARM daemon (via direct `/dev/mem` mmap) access the *same* 32 KB region. The
   ARM reads the init block and TX/RX buffers as plain memory — no mailbox,
   no per-word round-trips.

2. **Chip registers become a small FPGA register file with an asynchronous
   doorbell.** The 68k reads/writes RAP/RDP at full bus speed. Side-effecting
   writes raise a doorbell; the ARM applies LANCE semantics and writes status
   bits back into the FPGA shadow plus the INT2 line. No DTACK stretch, no
   watchdog, no BERR, no `bridge_done` CDC.

The result is **more faithful to real silicon** (the Am7990 never stalls the
68k for milliseconds — it runs asynchronously and signals completion via status
bits and INT) and **structurally simpler** (the entire DTACK-stretch FSM and
most of the mailbox FSM are deleted).

---

## 2. Background — Why the Current Design Fails

### 2.1 Current data paths

```
Register path (synchronous RPC, stalls 68k):
  68k → a2065_registers (DTACK stretch via regs_nrdy)
      → bridge_new_req/data/addr [CDC]
      → a2065_ddr3_mailbox (S_REG_* states)
      → arbiter → f2sdram → DDR3 → ARM daemon (polling)
      → ARM writes REG_RSP → FPGA latches → releases DTACK

Boardram ARM access (word-at-a-time, slow):
  ARM → boardram_xfer() → writes DDR3 RAM_REQ
      → FPGA polls RAM_REQ every 256 cycles (S_RAM_* states)
      → reads/writes BRAM Port B → writes RAM_RSP
      → ARM reads RAM_RSP

Interrupt path (polled, races):
  ARM → write MBX_INT every loop iteration
      → FPGA polls MBX_INT every 256 cycles in S_IDLE
      → latches a2065_int2 → CDC → Paula int2
```

### 2.2 The four structural problems

1. **Synchronous register RPC.** The 68k bus is held for the entire DDR3
   round-trip. This forces the DTACK-stretch FSM, the 25 ms watchdog, the BERR
   path, and the `bridge_done` clock-domain-crossing synchronizer. Each of these
   has been an independent source of bugs (CLAUDE.md "Critical Finding:
   S_REG_DONE Mailbox Adapter Stuck", the CDC fix in build 20260528a).

2. **Per-word boardram access.** `chip_init()` reads a 24-byte init block as 12
   separate DDR3 mailbox round-trips, each hundreds of microseconds. During this
   ~200–500 µs window the FPGA adapter is busy servicing RAM_REQ and **stops
   polling MBX_INT** — the interrupt race that caps the Interrupt test at ~88%.

3. **Single shared FSM with throttled polling.** One 19-state machine
   (`a2065_ddr3_mailbox.v`) multiplexes register, RAM, interrupt and MAC slots,
   each gated behind a `poll_div` throttle to avoid flooding the arbiter. Any
   mailbox clear while the FPGA is mid-transaction is fatal (CLAUDE.md:
   "bridge health is fundamentally harmful — all variants cause damage").

4. **Fights the source model.** The ARM daemon is a direct port of Amiberry's
   `a2065.cpp`, which assumes a flat memory buffer (`boardram` = process heap)
   and synchronous in-process register calls. The bridge re-introduces async
   RPC semantics that the ported code was never designed to tolerate, producing
   the `service_bridge_safe` CSR0-drop hack, the `int_hold` counter, the
   deferred-CSR0 queue, and the "restart daemon every 3 runs" workaround.

### 2.3 Why this is the wall, not the bugs

Step 10 has applied 6+ one-at-a-time fixes (self-loopback collision, rethink()
callback, MBX_INT hold counter, TDMD-without-STRT, pre-assert, TMD1-first
ordering). Each trades one race for another. The pure-memory test is at 100%
while everything timed is flaky — a textbook signature of an architecture limit
rather than a logic defect.

### 2.4 Verification results (2026-06-06)

All claims in this document were verified against source:

- **19-state mailbox FSM** confirmed (`a2065_ddr3_mailbox.v`): 8 `S_REG_*`, 8
  `S_RAM_*`, 3 idle/interrupt. `S_REG_POLL_DRAIN` is dead code (defined but
  never entered). `MBX_MAC` slot absent from FPGA despite ARM writing it.
- **ARM daemon workarounds** confirmed in `main_ddr3.cpp` (493 lines): all §8.1
  deletions present (`service_bridge_safe`, `deferred_csr0_queue`, `int_hold`,
  triple-write `assert_mbx_int`, INT refresh heartbeat, `tdmd_retry`,
  pre-assert + usleep).
- **DDR3 `+0x0000–+0x7FFF`** confirmed unused — no FPGA or ARM access in any
  code path. Proposed boardram region at that offset has zero conflict.
- **`boardram_access.h`** big-endian word accessors confirmed reusable for flat
  DDR3 — both native and remote paths do MSB-first word assembly.
- **Register module `gen_local` path** confirmed as working starting point for
  doorbell conversion — has 128-entry CSR array, 7-bit RAP, write-1-to-set/clear
  logic, no DTACK stretch.
- **New finding: clock domain crossing risk.** The 68k runs in `clk_sys` (~7 MHz)
  but the arbiter and f2sdram2 port are in `clk_audio` (~49 MHz). A 68k DDR3
  boardram window requires CDC for every Avalon signal. The current mailbox
  adapter avoids this because it operates in `clk_audio`. See §10 for analysis.

---

## 3. Proposed Architecture

### 3.1 Principle

> The 68k owns the **bus**. The ARM owns the **chip**. They communicate through
> **flat shared memory** plus a **doorbell**, never through a synchronous
> register RPC.

This mirrors real hardware: the Am7990 and the 68k share the board RAM; the 68k
pokes CSR0 and continues; the chip works on its own clock and signals via status
bits + INT.

### 3.2 Target data paths

```
Boardram (flat, shared — no mailbox):
  68k  → FPGA address decode → f2sdram window → DDR3 region  (DTACK-stretched
                                                              data access)
  ARM  → /dev/mem mmap of same DDR3 region → plain load/store

Chip registers (FPGA register file + doorbell):
  68k read  RAP/RDP → FPGA register file → returns immediately (full speed)
  68k write RAP     → FPGA stores pointer (full speed)
  68k write RDP w/ side-effect → FPGA latches {rap,data} into CMD slot,
                                 raises DOORBELL, returns immediately
  ARM polls/IRQ on DOORBELL → applies LANCE semantics (chip_wput)
                            → updates boardram in DDR3 directly
                            → writes resulting CSR0 status into FPGA shadow
                            → drives INT2 via single latched register

Interrupt (single latched line):
  ARM → write INT_STATE register (DDR3 or direct) → FPGA latches → CDC → Paula
```

### 3.3 Block diagram

```
┌──────────────────────────────────────────────────────────────────┐
│  Amiga 68k (FPGA, clk_sys)                                         │
│                                                                    │
│  AmigaOS A2065.device                                              │
│    │ ZorroII bus                                                   │
│    ▼                                                               │
│  gary.v sel_a2065 ─┬─► card+0x4000 RAP/RDP ─► a2065_regfile ──┐    │
│                    │                          (read: instant)  │    │
│                    │                          (write CSR: → CMD)│    │
│                    └─► card+0x8000 boardram ─► a2065_ddr_window │    │
│                                                (DTACK stretch)  │    │
│                                                    │            │    │
│  a2065_int2 ◄── INT latch ◄── CDC ◄────────────────┼────────────┘    │
└────────────────────────────────────────────────────┼───────────────┘
                                                      │ f2sdram2 (clk_audio)
                                                      ▼
┌──────────────────────────────────────────────────────────────────┐
│  ARM Linux (HPS)                                                   │
│                                                                    │
│  a2065d daemon (flat-memory model — Amiberry-native)               │
│    ├─ poll/IRQ DOORBELL                                            │
│    ├─ chip_wput / chip_wget   (CSR semantics)                      │
│    ├─ do_transmit / gotfunc   (ring walk, flat DDR3 pointers)      │
│    ├─ mungepacket / crc32     (unchanged)                          │
│    └─ AF_PACKET raw socket                                         │
│                                                                    │
│  /dev/mem mmap:                                                    │
│    DDR3 boardram region   (flat 32 KB, shared with 68k)            │
│    CMD/DOORBELL slot       (FPGA→ARM register-write events)        │
│    CSR shadow write slot   (ARM→FPGA status writeback)             │
│    INT_STATE slot          (ARM→FPGA interrupt line)               │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. Memory Map

### 4.1 ZorroII card space (unchanged from 68k view)

| Offset | Size | Region | Backing |
|--------|------|--------|---------|
| 0x0000–0x3FFF | 16 KB | unused | — |
| 0x4000 | 2 B | RDP (data port) | FPGA regfile + doorbell |
| 0x4002 | 2 B | RAP (addr pointer) | FPGA regfile (instant) |
| 0x4004–0x7FFF | 16 KB | unused | — |
| 0x8000–0xFFFF | 32 KB | card RAM | **DDR3 region** |

### 4.2 DDR3 layout (ARM physical, mmap of /dev/mem)

`DDR3_BASE = 0x1FF00000` (unchanged — safe region above kernel RAM).

| ARM offset | Size | Name | Direction | Purpose |
|-----------|------|------|-----------|---------|
| +0x0000 | 32 KB | BOARDRAM | shared | Card RAM — 68k and ARM both access |
| +0x8000 | 8 B | CMD | FPGA→ARM | Doorbell: pending, rap[6:0], data[15:0] |
| +0x8008 | 8 B | CMD_ACK | ARM→FPGA | ARM clears pending after processing |
| +0x8010 | 8 B | CSR_SHADOW | ARM→FPGA | Status bits the 68k reads back |
| +0x8018 | 8 B | INT_STATE | ARM→FPGA | bit[0] = assert INT2 |
| +0x8020 | 8 B | RAP_MIRROR | FPGA→ARM | Current RAP (so ARM knows target CSR) |
| +0x8028 | 8 B | MAC | ARM→FPGA | Dynamic MAC for autoconfig (startup) |

Avalon (f2sdram2) addresses = ARM physical byte offset >> 3 (64-bit words),
unchanged from current convention.

### 4.3 Boardram addressing

The 68k sees boardram at `card_base<<16 + 0x8000`. The FPGA window subtracts the
base and the 0x8000 offset, producing a 0x0000–0x7FFF byte offset into the DDR3
BOARDRAM region. The ARM sees the identical offset at `map + (offset)`. **Both
sides index the same bytes** — no byte-swap, no mailbox translation.

> Byte order: the 68k is big-endian, the ARM little-endian. The current word
> mailbox hides this. With flat DDR3 the daemon must apply the same big-endian
> word accessors it already uses (`get_ram_word` / `put_ram_word` byteswap), so
> this is a no-op change — the accessor layer already exists in
> `boardram_access.h`.

---

## 5. Chip Register Protocol (Doorbell)

### 5.1 FPGA register file

A small synchronous register file in the FPGA, clocked by `clk_sys`:

```
reg [6:0]  rap;            // RAP pointer, written directly by 68k
reg [15:0] csr_shadow[4];  // CSR0..CSR3 readable state, written by ARM
```

68k accesses resolve combinationally / in 1 cycle — **no DTACK stretch on
registers**:

| 68k access | FPGA action |
|-----------|-------------|
| write RAP (0x4002) | `rap <= data[6:0]`; assert DTACK |
| read RAP (0x4002) | drive `{9'b0, rap}`; assert DTACK |
| read RDP (0x4000) | drive `csr_shadow[rap]` (or chip-ID for rap 88/89); assert DTACK |
| write RDP (0x4000) | latch `{rap, data}` → CMD slot, set CMD.pending, assert DTACK |

Only the **RDP write** raises a doorbell. Everything else is local and instant.

### 5.2 Doorbell handshake

```
FPGA (on RDP write):
  if CMD.pending == 0:
    CMD <= {pending=1, rap, data}
    RAP_MIRROR <= rap
  else:
    // back-pressure: stall THIS write with DTACK only (rare —
    // driver issues one CSR write then polls). See §5.4.

ARM (main loop or HPS IRQ handler):
  if CMD.pending:
    rap  = CMD.rap; data = CMD.data
    chip_wput(rap, data)              // full LANCE semantics, may touch boardram
    csr_shadow[0..3] = csr[0..3]      // push readable state back to FPGA
    INT_STATE = (CSR0_INTR && CSR0_INEA) ? 1 : 0
    CMD.pending = 0                   // release doorbell
```

### 5.3 Why no DTACK stretch is needed for registers

The driver's behavior is always **write-then-poll**:

```
write CSR0 = INIT|STRT
loop: read CSR0 until IDON set      ← polls the shadow at full speed
```

The 68k write returns immediately (FPGA latched it). The subsequent polls read
`csr_shadow[0]` at full bus speed. The ARM updates the shadow within
microseconds — far faster than the driver completes even one poll iteration
through AmigaOS. This is exactly how real silicon behaves: IDON appears
asynchronously while the 68k spins.

The **one** ordering guarantee required: a boardram write by the 68k must be
visible to the ARM before the doorbell that depends on it. Because boardram data
accesses are DTACK-stretched DDR3 writes (§6), the write has physically landed
in DDR3 before the 68k issues the following CSR0/TDMD write. The doorbell can
never overtake the data. Same invariant as the real shared-bus LANCE.

### 5.4 Back-pressure (rare double-write)

If the driver issues two RDP writes faster than the ARM drains one (unusual —
requires no intervening poll), the FPGA holds DTACK on the *second* write until
`CMD.pending` clears. This is the **only** place a register access can stall,
bounded by ARM doorbell latency (µs), and it degrades gracefully to current
behavior. No watchdog/BERR needed — but a long-timeout BERR may be retained as
a safety net (§9).

---

## 6. Boardram Window (68k → DDR3)

### 6.1 Access path

```
68k data access to card+0x8000..0xFFFF:
  decode → offset = addr - (card_base<<16) - 0x8000   // 0x0000..0x7FFF
  assert nrdy (DTACK stretch)
  issue f2sdram read or write at DDR3_BASE + offset
  on completion → latch read data / ack write → release DTACK
```

This is the previously-abandoned `a2065_ddram.v` path (DESIGN.md Step 7,
"DDR3 boardram variant"). It must be completed and is the larger FPGA work item.

### 6.2 Latency budget

| Access | Current (BRAM) | Proposed (DDR3) |
|--------|---------------|-----------------|
| 68k word read | ~0 wait (1 cyc) | ~0.5–1 µs (arbiter + DDR3) |
| 68k word write | ~0 wait | ~0.5–1 µs |
| ARM init block read (24 B) | 12× mailbox ≈ 2–6 ms | 1 memcpy ≈ µs |
| ARM TX buffer read | per-word mailbox | flat memcpy |

The 68k pays more per access; the ARM pays dramatically less. The real A2065
board RAM was shared-bus and not zero-wait, so DTACK-stretched DDR3 is
architecturally honest. Net system effect is expected positive because the
pathological per-word ARM path (and its interrupt-race side effect) disappears.

### 6.3 Optional BRAM cache (deferred)

A write-through BRAM cache in front of DDR3 could restore near-zero 68k latency,
but reintroduces coherency complexity. **Not in scope for v1.** Start with direct
DDR3; measure; add a cache only if 68k-side latency proves a problem (e.g. PIO
copy loops in the driver).

---

## 7. Interrupt Path

Single latched line, driven from the doorbell processing, not a polled slot:

```
ARM: after chip_wput / gotfunc / do_transmit, evaluate:
       INT_STATE = (CSR0_INTR && CSR0_INEA) ? 1 : 0
     write INT_STATE (DDR3 or direct register write)

FPGA: sample INT_STATE → a2065_int2 (clk_audio)
      → 2-stage CDC to clk_sys (existing, proven in minimig.v)
      → OR-tied into Paula int2 (INTREQ bit 3, level 2)
```

The race that caps the current Interrupt test disappears: there is no window
where the FPGA is "too busy servicing RAM_REQ to poll MBX_INT," because the ARM
no longer issues RAM_REQ at all (boardram is direct), and INT_STATE is written
in the same code path that changed CSR0.

If an FPGA→HPS hardware interrupt is wired (replacing doorbell polling), the ARM
processes the CSR write and updates INT_STATE with sub-microsecond latency.

---

## 8. ARM Daemon Changes

The daemon **reverts toward the original Amiberry structure**. Deletions
outnumber additions.

### 8.1 Deletions

| Removed | Reason |
|--------|--------|
| `boardram_remote.cpp` (DDR3 per-word mailbox) | boardram is direct mmap |
| `service_bridge_safe()` + deferred-CSR0 queue | no reentrancy during boardram waits |
| `int_hold` counter, `INT_HOLD_ITER`, pre-assert logic | no MBX_INT poll race |
| MBX_INT triple-write + refresh loop | single INT_STATE write |
| `tdmd_retry` scheduling | TDMD processed synchronously in doorbell |
| RAM_REQ/RAM_RSP slot handling | gone |

### 8.2 Retained / simplified

| Component | Change |
|-----------|--------|
| `registers.cpp` (`chip_wput`/`chip_wget`/`chip_init`) | unchanged logic; `chip_init` now reads flat DDR3 |
| `rings.cpp` (`do_transmit`/`gotfunc`) | unchanged logic; flat-pointer boardram |
| `boardram_access.h` | accessors point at DDR3 mmap base; keep big-endian word swap |
| `mac.cpp`, `crc32.cpp`, `ethernet.cpp` | unchanged |
| main loop | poll CMD.pending → dispatch; or block on HPS IRQ |

### 8.3 New main loop (sketch)

```c
while (running) {
    uint64_t cmd = rd64(CMD_OFF);
    if (cmd & 1) {
        uint8_t  rap_v = (cmd >> 1) & 0x7f;
        uint16_t data  = (cmd >> 8) & 0xffff;
        registers_lock();
        chip_wput_via_rap(rap_v, data);     // applies semantics
        push_csr_shadow();                   // csr_shadow[0..3] → FPGA
        update_int_state();                  // INT_STATE = INTR&&INEA
        registers_unlock();
        wr64(CMD_OFF, 0);                    // release doorbell
        __sync_synchronize();
    }
    // RX thread still delivers frames → gotfunc → push_csr_shadow + INT
}
```

RX delivery (separate thread) and TX (inside `chip_wput` on TDMD) both end by
refreshing the CSR shadow and INT_STATE — uniform exit path, no special cases.

---

## 9. FPGA Changes

### 9.1 New / modified modules

| Module | Change |
|--------|--------|
| `a2065_registers.v` | Replace DTACK-stretch FSM with register file + doorbell latch. Delete `BRIDGE_LOCAL`, watchdog, BERR (optional keep), `bridge_done` CDC. |
| `a2065_boardram.v` | Replace BRAM with DDR3 window (`a2065_ddram.v` path): decode + nrdy + f2sdram read/write. |
| `a2065_ddr3_mailbox.v` | Drastically shrink: delete all `S_REG_*` and `S_RAM_*` states. Keep only DDR3 access for boardram window + CMD/CSR_SHADOW/INT_STATE slots. |
| `avalon_arbiter.v` | **Review required** — 68k boardram now traverses f2sdram. See §10. |
| `minimig.v` | Re-wire: regfile shadow, doorbell, INT_STATE; keep existing int2 CDC. |

### 9.2 What gets deleted

- DTACK-stretch state machine (IDLE/WAIT_ARM/RELEASE/ST_BERR)
- 25 ms watchdog + BERR generation (optionally retained as safety, §9.3)
- `bridge_done` / `bridge_result` clock-domain crossing
- `S_REG_CAPTURE…S_REG_DONE` (8 states)
- `S_RAM_CAPTURE…S_RAM_BRAM_WAIT` (8 states)
- `poll_div` throttle, `reg_budget`, bridge-health logic

### 9.3 Optional safety BERR

The doorbell back-pressure (§5.4) cannot deadlock under normal driver behavior,
but a long-timeout (e.g. 100 ms) BERR on a stuck `CMD.pending` may be retained
to guarantee the 68k is never hung indefinitely if the daemon dies. Cheaper and
simpler than the current per-access watchdog.

> **Note:** In the current build, the register module's `cpu_berr_n` output is
> **disconnected** in `minimig.v` (`.cpu_berr_n()` left unconnected). A watchdog
> timeout currently returns $0000 to the 68k, not a bus error. Any safety BERR
> in the proposal must wire this signal correctly through Gary to the 68k.

---

## 10. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Arbiter cannot absorb 68k boardram f2sdram traffic | **Medium-High** | **High** | The arbiter is the declared "do-not-touch" component. 68k boardram replaces the old RAM_REQ traffic, so net f2sdram load may *drop*. Prototype boardram window against the existing arbiter first; measure; only then consider arbiter changes. |
| 68k DDR3 window requires clk_sys→clk_audio CDC | **Medium** | **High** | The 68k runs in `clk_sys` (~7 MHz) but the arbiter and f2sdram2 are in `clk_audio` (~49 MHz). Every Avalon signal (address, data, read/write, waitrequest, readdatavalid) needs careful CDC. Current mailbox adapter avoids this because it operates in `clk_audio`. Options: (a) implement DDR3 window module in `clk_audio` with async request/response FIFOs from `clk_sys`, (b) implement in `clk_sys` with CDC on every Avalon signal. Option (a) is preferred — it matches the proven mailbox pattern. |
| 68k PIO burst traffic 34× higher than current RAM_REQ | **Medium** | **Medium** | Current throttled RAM_REQ: ~190K DDR3 ops/sec. A 68k PIO copy loop could sustain ~7M word accesses/sec. This 34× increase could starve m0 (audio/PAL 128-word bursts). Mitigation: measure actual A2065.device access patterns; Phase 4 BRAM cache as fallback. |
| 68k boardram latency too high (PIO copy loops stall driver) | Medium | Medium | Measure with `--test-boardram` equivalent on 68k side. Fall back to optional write-through BRAM cache (§6.3). |
| CSR shadow staleness window visible to driver | Low | Medium | ARM updates shadow in µs, faster than one AmigaOS poll iteration. Matches real-hw async IDON. Verify with lance-test config subtest. |
| Byte-order bug at flat DDR3 boundary | Medium | Medium | Reuse existing big-endian word accessors in `boardram_access.h`; unit-test against known init block bytes before hardware. |
| Doorbell back-pressure on rapid double-write | Low | Low | DTACK-stall second write (§5.4) + optional 100 ms safety BERR. |
| DDR3 boardram window is unfinished RTL (`a2065_ddram.v`) | High (effort) | Medium | This is the main new build work. Simulate in `fpga/sim/` before MiSTer. |

The single highest risk is the arbiter. The design deliberately **reuses the
arbiter unchanged** and routes the 68k boardram path as a replacement for the
deleted RAM mailbox traffic, so the change is load-neutral-to-favorable rather
than additive. Any arbiter modification is explicitly out of scope.

The second-highest risk is the clock domain crossing. The 68k boardram DDR3
window introduces a new `clk_sys`→`clk_audio` crossing that does not exist in
the current architecture (the mailbox adapter is already in `clk_audio`). This
is mitigated by implementing the DDR3 window state machine in `clk_audio` and
using async FIFOs for the `clk_sys` request/response path — the same pattern
used by the existing mailbox adapter for `bridge_new_req`/`bridge_done`.

---

## 11. The LANCE State Machines (Reference)

The Am7990 is five concurrent machines. The emulation faithfully implements (A),
approximates (B)/(C) at the frame level, and delegates (D)/(E) to bus plumbing.
The Step-10 failures (collision, interrupt timing, TDR) are the *timed*
behaviors of (B)/(C) that were collapsed into ad-hoc flag writes. This section
documents them so future fidelity work (explicit ARM-side sub-states) has a
reference.

### (A) Chip lifecycle — CSR0 INIT/STRT/STOP — *fully emulated*

Derived from `registers.cpp` `chip_wput` case 0:

```
   power-on / RESET
        ▼
   ┌────────┐ set INIT      ┌──────────────┐ read 24 B init
   │  STOP  │──────────────►│ INITIALIZING │ block via DMA
   │ (idle) │               └──────┬───────┘
   └────────┘                      ▼ IDON=1, IRQ
     ▲  ▲   set STOP        ┌──────────────┐
     │  └───────────────────│  INIT_DONE   │ (RX/TX off)
     │      set STOP        └──────┬───────┘
     │                             ▼ set STRT (clears STOP)
     │                      ┌──────────────┐
     └──────────────────────│   RUNNING    │ RXON/TXON, poll rings
                            └──────────────┘
   Normal driver path: INIT|STRT (0x0003) → STOP→RUNNING in one write.
   TDMD in RUNNING → kick transmitter immediately (else 1.6 ms poll).
```

### (B) Transmitter — *faked* (frame-level only)

Datasheet "Transmit Ring Buffer Management" + collision/backoff:

```
TX_IDLE ─owns TMD1?─► TX_READ(TMD0/2) ─► TX_DMA→FIFO ─► TX_DEFER(IFS,2-part)
   ▲                                                          │
   │                                                          ▼
   │                                                  TX_PREAMBLE→TX_DATA
   │  retry (≤16, RTRY after)       collision<slot? ─────┘    │
   └── TX_BACKOFF ◄─ TX_JAM ◄─────────────────────────────────┤
                                                               ▼
                       TX_CRC(FCS) ─► TX_DONE(clr OWN, TMD3+TDR, TINT, IRQ)
   then SQE/heartbeat in IFS → CERR if no CLSN within 4 µs
```

Emulation shortcut: `do_transmit()` = one `sendto()`; collision faked via
self-loopback detection; TDR / backoff / slot-time not modeled.

### (C) Receiver — *faked* (frame-level only)

Datasheet "Receive Ring Buffer Management" + receive collision:

```
RX_IDLE ─owns RMD1 & RENA?─► RX_SFD_HUNT ─► RX_ADDR(phys/LADRF-hash/bcast/prom)
   ▲                                              │ match? no → discard
   │                                              ▼
   │  collision<64 B → runt, reset FIFO     RX_DATA(FIFO≥16 B→burst DMA, lookahead)
   │                                              ▼ RENA drops, sample CRC
   └──────────────── RX_DONE(MCNT, STP/ENP, CRC/FRAM/OFLO/BUFF, clr OWN, RINT, IRQ)
```

Emulation shortcut: `gotfunc()` walks ring, appends CRC32, sets flags — no
timed bit/FIFO/collision modeling.

### (D) Bus-master DMA micro-engine — *bus plumbing, not modeled as chip*

HOLD → HLDA → 8-word burst (single-word for ring/init) → release → re-request
within 700 ns if FIFO threshold met. In the proposed design this is replaced
entirely by the flat shared-memory model.

### (E) Slave RAP/RDP access — *the only machine the FPGA intercepts*

Two-step CSR access. In the current design this is the DTACK-stretch RPC; in the
proposed design it becomes the register-file + doorbell of §5.

### Takeaway

The proposed architecture lets the ARM own (A)/(B)/(C) entirely against flat
memory — exactly as Amiberry wrote them — and reduces the FPGA's role to (D)/(E)
as memory windowing + a doorbell. If higher collision/TDR fidelity is later
desired, (B)/(C) gain explicit timed sub-states **in ARM C code**, not in
fabric.

---

## 12. Migration Plan

Incremental, each step independently testable. Keep the current build deployable
until the new path is proven. **Each phase has automated test gates that must
pass before proceeding to the next phase.**

### Phase 0 — Spec + simulation (no hardware)

Development:

- [ ] Finalize DDR3 layout (§4) in `a2065_bridge.h` (new header variant).
- [ ] `fpga/sim/tb_ddram.v` — 68k boardram read/write against DDR3 model.
- [ ] `fpga/sim/tb_regfile.v` — RAP/RDP register file + doorbell latch.
- [ ] Unit-test ARM flat-boardram accessors (byte order) against known init block.

Automated test gates:

- [ ] `make -C fpga/sim` — all existing testbenches pass (`tb_autoconfig`,
      `tb_boardram`, `tb_registers`, `tb_bridge_e2e`).
- [ ] `make -C arm test` — all existing native unit tests pass (`mac_test`,
      `csr_test`, `rings_test`, `bridge_test`).
- [ ] New `fpga/sim/tb_ddram.v` — 68k boardram DDR3 window simulation passes.
- [ ] New `fpga/sim/tb_regfile.v` — register file + doorbell latch simulation
      passes.
- [ ] New `arm/src/boardram_flat_test.cpp` — flat DDR3 accessors produce correct
      big-endian byte/word results against known init block patterns.

### Phase 1 — Flat boardram (registers still RPC)

Development:

- [ ] Implement `a2065_ddram.v` window; wire into `minimig.v`.
- [ ] Point ARM `boardram_access.h` at direct DDR3 mmap; delete
      `boardram_remote.cpp`.
- [ ] Keep existing register mailbox temporarily.

Automated test gates:

- [ ] `make -C fpga/sim` — updated `tb_boardram` against DDR3 model passes.
- [ ] `make -C arm test` — passes with `boardram_flat_test` in suite.
- [ ] MiSTer pytest: `test_boardram.py` — ARM loopback + 68k cross-domain (4
      tests, existing, unchanged target).
- [ ] MiSTer pytest: `test_integration.py::TestBoardram` — DDR3 loopback
      passes (existing).
- [ ] MiSTer pytest: `test_integration.py::TestLanceDiag::test_buffer_memory`
      — 100% (existing).
- [ ] ARM on-MiSTer: `test_deadlock` — no regression (existing).
- [ ] ARM on-MiSTer: `test_interrupt` — no regression (existing).
- [ ] **Regression gate:** `test_integration.py` — Interrupt rate **improves**
      from 88% baseline (init-block read no longer blocks MBX_INT polling).

### Phase 2 — Register file + doorbell

Development:

- [ ] Replace `a2065_registers.v` DTACK-stretch FSM with register file +
      doorbell; delete watchdog/BERR/CDC (optional safety BERR).
- [ ] Shrink `a2065_ddr3_mailbox.v` (delete `S_REG_*`, `S_RAM_*`).
- [ ] ARM: doorbell main loop (§8.3); delete `service_bridge_safe`,
      `int_hold`, `tdmd_retry`, deferred-CSR0 queue.

Automated test gates:

- [ ] `make -C fpga/sim` — `tb_registers` updated to `tb_regfile` semantics
      passes.
- [ ] `make -C arm test` — all unit tests pass.
- [ ] MiSTer pytest: `test_integration.py` — full suite green (all tests).
- [ ] MiSTer stress: `run_lance_100x.py 100` — ALL PASS ≥90%, individual tests
      ≥95%.
- [ ] New pytest: `test_doorbell.py` — doorbell round-trip latency measurement
      (CMD pending→cleared timing).
- [ ] ARM on-MiSTer: `test_deadlock` — updated for doorbell protocol, passes.
- [ ] ARM on-MiSTer: `test_interrupt` — updated for INT_STATE path, passes.

### Phase 3 — Interrupt line + polish

Development:

- [ ] Single INT_STATE write path; confirm interrupt test ≥98%.
- [ ] Optional: FPGA→HPS hardware IRQ to replace doorbell polling.

Automated test gates:

- [ ] MiSTer stress: `run_lance_100x.py 200` — ALL PASS ≥95%, 0% loss.
- [ ] New pytest: `test_stability.py` — daemon restart cycle (10 restarts, no
      core reload between, all restarts pass lance-test).
- [ ] MiSTer pytest: `test_network.py` — AddNetInterface + `show arp` suite
      (existing, must pass).
- [ ] Stress: `run_lance_100x.py` with concurrent `ping -c 10000` from another
      host — 0% packet loss.
- [ ] Confirm "daemon restart every 3 runs" workaround is no longer needed.

### Phase 4 — Optional latency cache

Development:

- [ ] Only if 68k boardram latency proves problematic: write-through BRAM cache
      in front of DDR3 window (§6.3).

Automated test gates:

- [ ] New `fpga/sim/tb_ddram_cache.v` — BRAM cache coherency testbench passes.
- [ ] MiSTer stress: `run_lance_100x.py 100` — no regression vs Phase 3.

### Rollback

Each phase leaves a working `.rbf` + daemon pair. If a phase regresses, redeploy
the prior pair. The current build (`Minimig_20260528a.rbf` + 20260601c daemon,
verified 2026-06-06) remains the baseline until Phase 2 beats it.

### 12.1 Test Infrastructure Reference

| Category | Invocation | Key Files | Phase |
|----------|-----------|-----------|-------|
| Verilog simulation | `make -C fpga/sim` | `tb_autoconfig.v`, `tb_boardram.v`, `tb_registers.v`, `tb_bridge_e2e.v` | 0+ |
| ARM native unit tests | `make -C arm test` | `mac_test.cpp`, `csr_test.cpp`, `rings_test.cpp`, `bridge_test.cpp` | 0+ |
| ARM on-MiSTer tests | `./test_deadlock`, `./test_interrupt` | `test_deadlock.cpp`, `test_interrupt.cpp` | 1+ |
| MiSTer pytest | `pytest -v -s` | `test_integration.py`, `test_boardram.py`, `test_network.py` | 1+ |
| Stress runner | `python3 run_lance_100x.py N` | `run_lance_100x.py`, `conftest.py`, `mister_ssh.py` | 2+ |

---

## 13. Success Criteria

| Metric | Current | Target |
|--------|---------|--------|
| lance-test Buffer | 100% | 100% |
| lance-test Config | 97% | ≥99% |
| lance-test Interrupt | 88% | ≥98% |
| lance-test Collision | 76% | ≥95% |
| lance-test Loopback | 53% | ≥95% |
| **ALL PASS** | **~53%** | **≥95%** |
| Daemon restart workaround | required every 3 runs | not required |
| FPGA mailbox states | 19 | ≤6 |
| `ping -c 10000` | not yet reached | 0% loss |
| Phase gate tests passing | N/A | All phase gates green before proceeding |
| `make -C fpga/sim` | 4 testbenches | 4 testbenches (updated) + 2 new |
| `make -C arm test` | 4 unit tests | 5 unit tests (+boardram_flat_test) |
| `pytest test_integration.py` | N/A | All tests green |
| Daemon restart stability | fails after 3 runs | unlimited restarts |

---

## 14. Open Questions

1. **Arbiter headroom + CDC.** Can the existing v5 arbiter absorb 68k boardram
   f2sdram traffic without modification? The 68k DDR3 window module must run in
   `clk_audio` (with async request/response FIFOs from `clk_sys`) or in
   `clk_sys` (with CDC on every Avalon signal). The current mailbox adapter
   operates entirely in `clk_audio` and avoids this crossing entirely. The
   `clk_audio` approach is preferred but needs validation in Phase 0 simulation.
   Additionally, current throttled RAM_REQ traffic is ~190K DDR3 ops/sec; a 68k
   PIO copy loop could sustain ~7M word accesses/sec (34× increase). Need to
   measure actual A2065.device access patterns to determine if m0 (audio/PAL)
   is starved. This gates the whole proposal.
2. **68k boardram latency tolerance.** Does the A2065.device do tight PIO copy
   loops that would suffer at ~1 µs/word? Determines whether Phase 4 cache is
   needed.
3. **HPS hardware IRQ availability.** Is an FPGA→HPS interrupt line free in the
   MiSTer framework, or is doorbell polling the only option? Affects interrupt
   latency floor.
4. **Safety BERR threshold.** What 68k-side timeout does AmigaOS expect before
   declaring the card dead? Sets the optional BERR window. Note: current BERR
   output is disconnected in minimig.v — any safety BERR must wire this correctly.
5. **BRAM fallback for Phase 1.** If the DDR3 window proves impractical (CDC
   complexity, latency, arbiter contention), can flat boardram still be achieved
   by giving the ARM direct mmap access to BRAM contents via a different sharing
   mechanism? This avoids all DDR3 window risks but requires a second sharing
   path. Worth investigating before committing to the DDR3 window approach.

---

## 15. References

- `docs/am79c90.md` — Am79C90 C-LANCE datasheet (converted)
- `DESIGN.md` — current architecture (DDR3 mailbox RPC)
- `IMPLEMENTATION_PLAN.md` — Steps 0–10, original boardram/register split
- `tests/lance_logs/METHODOLOGY.md` — Step-10 iterative fix methodology
- `arm/src/registers.cpp` — CSR state machine (machine A)
- `arm/src/rings.cpp` — TX/RX ring walkers (machines B/C, frame-level)
- `arm/src/main_ddr3.cpp` — current mailbox daemon (to be simplified)
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_ddr3_mailbox.v` — current 19-state FSM
- `Minimig-AGA_MiSTer/rtl/A2065/avalon_arbiter.v` — fixed 2-master arbiter
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_registers.v` — DTACK-stretch register module
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_boardram.v` — 32KB TDP BRAM
- `Minimig-AGA_MiSTer/rtl/minimig.v` — boardram/register wiring, int2 CDC
- `Minimig-AGA_MiSTer/rtl/sys_top.v` — mailbox/arbiter/f2sdram2 wiring

---

## 16. Verification Evidence (2026-06-06)

All claims in this document were verified against source code on 2026-06-06.

| Claim | Source File | Verification |
|-------|------------|--------------|
| 19-state mailbox FSM | `a2065_ddr3_mailbox.v` | Counted: 8 `S_REG_*`, 8 `S_RAM_*`, 3 idle/interrupt. `S_REG_POLL_DRAIN` (encoding 18) is dead code — defined but never entered by any transition. `MBX_MAC` localparam absent from FPGA despite ARM writing to offset `+0x8028`. |
| DDR3 +0x0000–+0x7FFF unused | `a2065_ddr3_mailbox.v`, `main_ddr3.cpp` | All FPGA Avalon accesses target `DDR3_BASE + 0x1000` or higher. All ARM accesses target offsets `+0x8000` or higher. The 64KB mmap window covers `+0x0000..+0xFFFF` — boardram region at `+0x0000..+0x7FFF` is untouched. |
| Avalon = ARM_physical >> 3 | Both sides | Confirmed for all 6 mailbox slots: `+0x8000>>3=0x1000`, `+0x8008>>3=0x1001`, `+0x8010>>3=0x1002`, `+0x8018>>3=0x1003`, `+0x8020>>3=0x1004`, `+0x8028>>3=0x1005`. `DDR3_BASE`: `0x1FF00000>>3=0x03FE0000`. |
| Arbiter 2-master, no free slots | `avalon_arbiter.v` | Fixed 2-master design with explicit port declarations, no parameterization. m0=ddr_svc (audio/PAL), m1=A2065 mailbox. Burst tracking bug confirmed load-bearing (single-word write sticks `burst_active` permanently, preventing m0 preemption). |
| Register DTACK-stretch FSM | `a2065_registers.v` | 4 states: `ST_IDLE`, `ST_WAIT_ARM`, `ST_RELEASE`, `ST_BERR`. `gen_local` (BRIDGE_LOCAL=1) path has 128-entry CSR array + 7-bit RAP + write-1-to-set/clear logic — confirmed as starting point for doorbell. Watchdog at 700000 cycles ≈ 14ms at 49MHz. |
| BERR disconnected | `minimig.v:962` | `.cpu_berr_n()` left unconnected. Watchdog timeout returns $0000 (OR-tied bus default), not bus error. |
| Boardram TDP BRAM | `a2065_boardram.v` | 32KB split hi/lo byte arrays (16384×8 each), `no_rw_check, M10K` attributes. No `card_base` input — addressing uses `cpu_addr[15]` for select and `cpu_addr[14:1]` for address. Works correctly for any Zorro II base because Gary pre-qualifies `sel`. |
| ARM daemon workarounds | `main_ddr3.cpp` | 493 lines. All §8.1 deletions confirmed present: `service_bridge_safe()` (lines 225–269), `deferred_csr0_queue[8]` + head/tail (lines 69–71), `int_hold`/`INT_HOLD_ITER=100` (lines 67–68), `assert_mbx_int()` triple-write (lines 76–87), INT refresh heartbeat (lines 473–481), `tdmd_retry` (lines 72–73, 451–465), pre-assert + usleep (lines 147–162, 171–183). |
| Big-endian accessors reusable | `boardram_access.h` | Both `BOARDRAM_REMOTE` and native paths do MSB-first word assembly: `(byte[off] << 8) \| byte[off+1]`. Flat DDR3 path reuses native accessors with pointer changed to DDR3 mmap base. |
| Clock domain: 68k=clk_sys, arbiter=clk_audio | `sys_top.v`, `minimig.v` | Boardram Port A clocked by `clk_sys` (68k domain). Mailbox adapter + arbiter in `clk_audio`. New finding: a 68k DDR3 window must cross these domains — current mailbox adapter avoids this by operating entirely in `clk_audio`. |
- Amiberry `src/a2065.cpp` (Toni Wilen, 2009) — flat-memory source model
