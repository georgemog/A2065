# A2065 Code Review — Flaws & Vulnerabilities

**Date:** 2026-05-31  
**Scope:** ARM daemon (`arm/`), FPGA RTL (`fpga/rtl/`, `Minimig-AGA_MiSTer/rtl/A2065/`), tests (`tests/`)  
**Reviewer:** Claude Code (automated static analysis)

---

## Summary

| Severity | Count |
|----------|-------|
| Critical | 4 |
| High | 6 |
| Medium | 6 |
| Low | 4 |

---

## Critical

### C1 — Incomplete Port B in standalone `a2065_boardram.v`
**File:** `fpga/rtl/a2065_boardram.v:33–36`

The ARM bridge (Port B) signals `arm_addr`, `arm_data_in`, `arm_wr`, `arm_sel` are declared as inputs but never wired into any RAM read/write logic. Output `arm_data_out` is missing entirely. Only Port A (68k side) is implemented.

**Impact:** ARM cannot access boardram via the standalone RTL module. Rings, descriptors, and packet data unreachable.

**Fix:** Infer TDP BRAM properly or add explicit Port B logic:
```verilog
output reg [15:0] arm_data_out,

always @(posedge clk) begin
    if (arm_sel && arm_wr)  ram[arm_addr] <= arm_data_in;
    if (arm_sel && !arm_wr) arm_data_out  <= ram[arm_addr];
end
```

---

### C2 — No mutex between `rx_thread` and main loop for CSR0 / ring state
**File:** `arm/src/main_ddr3.cpp` — `rx_thread`, `service_bridge`, `write_mbx_int`

`rx_thread()` calls `gotfunc()` which modifies CSR0 flags and RX ring state. Concurrently, the main loop calls `service_bridge()` → `chip_wput()` → `rethink()` → `assert_mbx_int()`, all touching the same CSR0 and ring offsets. No mutex anywhere. `registers_csr0()` reads a `uint16_t` (aligned, so the read itself is atomic on ARMv7), but every read-modify-write sequence — e.g., `registers_csr0_set(CSR0_RINT)` inside `gotfunc()` racing with `registers_csr0_clr(CSR0_INTR)` in the main loop — is a data race. Additionally, `service_bridge_safe()` (called during boardram transactions) consumes register requests but silently drops CSR0 writes (addr==0), while forwarding only RAP writes (addr==2). Those dropped writes are never queued or retried, so Amiga CSR0 clears (IDON, TX status) are permanently lost.

**Impact:** Torn CSR0 state; lost interrupt clears; stale ring offsets. Directly contributes to the observed 16% interrupt failure rate and 24% collision test TDMD visibility failure.

**Fix — Step 1: add a global register lock**

In `arm/src/main_ddr3.cpp`, near the other `static` globals:
```cpp
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
```

Wrap all CSR0 mutations in both threads:
```cpp
// gotfunc / rx_thread path (registers.cpp rethink callback):
pthread_mutex_lock(&reg_lock);
registers_csr0_set(CSR0_RINT | CSR0_INTR);
pthread_mutex_unlock(&reg_lock);

// main loop / service_bridge path:
pthread_mutex_lock(&reg_lock);
chip_wput(addr, data);   // already after REG_RSP is sent
pthread_mutex_unlock(&reg_lock);
```

**Fix — Step 2: don't silently drop CSR0 writes in `service_bridge_safe()`**

```cpp
static void service_bridge_safe(void)
{
    uint64_t req = rd64(MBX_REQ_OFF);
    if (!(req & 1)) return;

    int      rw   = (req >> 1) & 1;
    uint16_t addr = (req >> 2) & 0xFF;
    uint16_t data = (req >> 10) & 0xFFFF;

    uint16_t result = rw ? 0 : chip_wget(addr);
    uint64_t rsp = 1 | ((uint64_t)result << 1);
    wr64(MBX_RSP_OFF, rsp);
    __sync_synchronize();
    wr64(MBX_REQ_OFF, 0);

    if (rw) {
        // Forward ALL writes, not just RAP — prevents stale CSR0 state
        pthread_mutex_lock(&reg_lock);
        chip_wput(addr, data);
        pthread_mutex_unlock(&reg_lock);
    }
}
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Mostly incorrect — the review misidentifies which variables have a problem.**

Variable-by-variable analysis of the actual code:

| Variable | Declared As | Accessed From | Actually volatile? | Needs volatile? |
|---|---|---|---|---|
| `running` | `volatile sig_atomic_t` (line 45) | Signal handler, main loop, rx_thread | **Yes** | Already correct |
| `last_mbx_int` | `static int` (line 63) | Main thread only | No | **No** — only accessed from `assert_mbx_int()`, `write_mbx_int()`, and main loop, all in main thread |
| `int_hold` | `static int` (line 64) | Main thread only | No | **No** — only accessed from `assert_mbx_int()`, `write_mbx_int()`, and main loop, all in main thread |
| `dbg_cnt` | `static int` (line 62) | Main thread only | No | **No** — only accessed from main thread functions |
| `tdmd_retry` | `static int` (line 69) | Main thread only | No | **No** |

**The only variable that was correctly declared is `running`** — it's already `volatile sig_atomic_t`, which is the standard POSIX way to share a flag between a signal handler and the rest of the program. The review incorrectly states it's a "plain `static int`".

**Why the other variables don't need volatile:** `last_mbx_int`, `int_hold`, `dbg_cnt`, and `tdmd_retry` are accessed exclusively from the main thread. `assert_mbx_int()` is called from `on_interrupt_cb()` which fires from `rethink()` which is called from `chip_wput()` — all in main thread context (or under the new mutex in rx_thread, but `assert_mbx_int` only writes DDR3 and local main-thread variables). GCC cannot hoist a variable across a function call boundary that might modify it (and `wr64`, `rd64` are extern calls the compiler can't see through).

**The real signal handler issue** (discovered during C2 fix implementation): The original `handle_signal()` called `ethernet_close()`, which could deadlock if `rx_thread` held the mutex inside `gotfunc()`. This is not a volatile issue — it's a signal-safety issue. Fixed by removing `ethernet_close()` from the signal handler.

---

### C3 — Boardram offset shift-by-2 produces word address × 2
**File:** `arm/src/boardram_remote.cpp:38–42`

Actual code:
```cpp
static uint16_t boardram_xfer(uint16_t off, int write, uint16_t wdata)
{
    uint64_t req = 1 | ((uint64_t)write << 1)
                 | ((uint64_t)(off & 0x7FFE) << 2)
                 | ((uint64_t)wdata << 17);
```

CLAUDE.md documents the RAM_REQ bit layout as:
```
bit[0]=pending, bit[1]=rw, bits[16:2]=offset, bits[32:17]=write_data
```

The FPGA reads the offset from `req[16:2]`. Tracing what ARM encodes for a given `off`:

| ARM `off` (byte) | word index | `(off & 0x7FFE) << 2` | FPGA reads `req[16:2]` |
|---|---|---|---|
| 0 | 0 | 0 | 0 ✓ |
| 2 | 1 | 8 | 2 ✗ (expect 1) |
| 4 | 2 | 16 | 4 ✗ (expect 2) |
| 8 | 4 | 32 | 8 ✗ (expect 4) |

FPGA receives `off / 2` instead of the correct word index. Every boardram access lands at double the intended address. Offset 0 works by coincidence; all other addresses are wrong.

**Why tests may still pass:** The Am7990 init block starts at boardram offset 0 (the ARM writes it there). If all tested structures reside near offset 0 and the FPGA BRAM has mirrored or aliased behavior, the error is masked at small offsets. Ring descriptors beyond the init block will silently access the wrong BRAM words.

**Fix:** Change the shift from `<< 2` to `<< 1` so the encoded word address matches `off / 2`:

```cpp
// boardram_remote.cpp:40-42
uint64_t req = 1 | ((uint64_t)write << 1)
             | ((uint64_t)(off & 0x7FFE) << 1)   // << 1, not << 2
             | ((uint64_t)wdata << 16);            // wdata now at bit 16
```

**Note:** Changing the offset shift requires adjusting `wdata` shift from 17 → 16, and verifying the FPGA `a2065_ddr3_mailbox.v` decodes `req[16:2]` for address and `req[32:17]` for data (or updating both sides to match a cleaner layout).

**Verification:** Add a startup self-test in `boardram_remote_init()`:
```cpp
// Write sentinel at word 1 (byte offset 2), verify round-trip
boardram_ww(2, 0xA55A);
uint16_t v = boardram_rw(2);
if (v != 0xA55A)
    fprintf(stderr, "[boardram] OFFSET ENCODING BUG: wrote 0xA55A at off=2, got 0x%04X\n", v);
boardram_ww(2, 0x0000);
```

#### Reviewer Assessment (2026-05-31)

**Verdict: FALSE ALARM — the encoding is correct.**

The review's analysis contains a critical error: it assumes the FPGA extracts the offset from `req[16:2]`, but the actual FPGA code (`a2065_ddr3_mailbox.v:232`) reads `avl_readdata[16:3]` — one bit narrower.

Tracing the actual bit layout:

| ARM `off` (byte) | `(off & 0x7FFE) << 2` | Bit position in `req` | FPGA `req[16:3]` | Word index |
|---|---|---|---|---|
| 0 | 0 | — | 0 | 0 ✓ |
| 2 | 8 | bit 3 | 1 | 1 ✓ |
| 4 | 16 | bit 4 | 2 | 2 ✓ |
| 8 | 32 | bit 5 | 4 | 4 ✓ |

The ARM `<< 2` places the byte offset at bits `[16:2]`. The FPGA reads `[16:3]` which extracts bits 3–16, giving `(byte_offset >> 1)` = word index. This is correct — byte offset divided by 2 equals the BRAM word address.

The review's table incorrectly computed `req[16:2]` values (off=2 → 2, off=4 → 4, off=8 → 8), which would only be correct if the FPGA extracted the raw byte offset. But `req[16:3]` extracts `byte_offset / 2`, which is the intended word address.

**No code change needed.** The boardram DDR3 loopback test (6/6 PASS at offsets 0x0000, 0x0002, 0x0100, 0x0102, 0x0200, 0x7FFE) empirically confirms correct addressing across the full 32KB range.

---

### C4 — `running`, `last_mbx_int`, `int_hold` not `volatile` — signal visibility
**File:** `arm/src/main_ddr3.cpp` — `handle_signal`, `assert_mbx_int`, `write_mbx_int`, main loop

Actual code:
```cpp
static void handle_signal(int sig) { (void)sig; running = 0; ethernet_close(); }

static void assert_mbx_int(void)
{
    ...
    last_mbx_int = 1;
    int_hold = INT_HOLD_ITER;
    ...
}
```

`running`, `last_mbx_int`, `int_hold`, and `dbg_cnt` are all plain `static int`. With `-O2` on ARMv7, GCC is free to hoist any of these into registers for the duration of a loop. The signal handler's `running = 0` write and `assert_mbx_int()`'s `int_hold = INT_HOLD_ITER` write may never become visible to the main loop until a function call forces a register spill.

**Impact:**
- Daemon ignores SIGINT/SIGTERM — only killable with SIGKILL
- `int_hold` decrement loop in `write_mbx_int()` may read a stale zero and prematurely deassert `MBX_INT`
- `last_mbx_int` comparison in `write_mbx_int()` may use cached value, suppressing a needed DDR3 write

**Fix:** Mark all signal-shared and cross-function state variables `volatile`:

```cpp
// arm/src/main_ddr3.cpp — change these declarations:
static volatile int running      = 1;
static volatile int last_mbx_int = -1;
static volatile int int_hold     = 0;
static volatile int tdmd_retry   = 0;
static volatile int dbg_cnt      = 0;
```

Also add a full memory barrier after `ethernet_close()` in the signal handler to ensure the close completes before the main loop sees `running == 0`:
```cpp
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
    __sync_synchronize();
    ethernet_close();
}
```

---

## High

### H1 — No mutex between RX thread and main loop for CSR0 / ring state
**File:** `arm/src/main_ddr3.cpp:92–102, 390–425`

`rx_thread()` calls `gotfunc()` which modifies CSR0 flags and RX ring offsets. The main loop simultaneously calls `registers_csr0_set/clr()` and walks TX rings. No locking. `registers_csr0()` reads a `uint16_t` — atomic on ARMv7 for aligned access, but the compound read-modify-write in `rethink()` is not atomic.

**Impact:** CSR0 torn reads/writes; ring offset corruption; lost interrupts.

**Fix:** Add a single `pthread_mutex_t` protecting all CSR0 and ring-offset mutations.

#### Reviewer Assessment (2026-05-31)

**Verdict: Duplicate of C2 — already fixed.**

H1 is identical to C2 (same data race, same files, same root cause). The C2 mutex fix (recursive `pthread_mutex_t` wrapping all CSR0/ring-offset mutations in both threads) addresses this completely. H1 can be considered resolved by the C2 fix.

---

### H2 — TX/RX descriptor address uses only 8 bits of upper address byte
**File:** `arm/src/rings.cpp:97–98`

```cpp
addr = (uint32_t)tmd0 | ((uint32_t)(tmd1 & 0xff) << 16);
```

Per Am7990 spec the buffer address is 24-bit: `tmd0` = bits [15:0], `tmd1[7:0]` = bits [23:16]. The mask `& 0xff` is correct for 24-bit, but `tmd1` is a `uint16_t` read from boardram. If the upper byte of `tmd1` contains flags (TX_OWN, TX_STP etc.), masking to `0xff` is right — but verify the word layout matches the actual boardram word order. A byte-swap error here would cause all packets to read from address 0.

**Impact:** Wrong packet buffer address → read/write garbage → dropped or corrupted frames.

**Fix:** Add a static test that encodes a known descriptor and decodes the address, asserting correctness.

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid observation but not a bug — the code is correct.**

The Am7990 TX descriptor word layout is:
- **TMD0** (word 0): Buffer address bits [15:0]
- **TMD1** (word 1): bits [7:0] = address bits [23:16], bits [15:8] = flags (TX_OWN, TX_STP, TX_ENP, etc.)
- **TMD2** (word 2): Buffer byte count (two's complement)
- **TMD3** (word 3): Status and error flags

The code `addr = (uint32_t)tmd0 | ((uint32_t)(tmd1 & 0xff) << 16)` correctly extracts the 24-bit buffer address: lower 16 bits from TMD0, upper 8 bits from TMD1's lower byte. The `& 0xff` mask strips the flag bits from TMD1's upper byte (TX_OWN etc.), which is exactly right.

The review's suggestion to "verify the word layout matches" is sensible defensiveness, and the suggested static test would be good defensive programming. But the existing code matches the Am7990 spec correctly.

**No code change needed.** The `addr &= RAM_MASK` on the next line further constrains to 15-bit boardram addressing, which is also correct for this implementation.

---

### H3 — CDC missing on `mac_byte*` inputs in `a2065_autoconfig.v`
**File:** `fpga/rtl/a2065_autoconfig.v:66–73`

`mac_byte2`–`mac_byte5` are driven by ARM (HPS domain or async) and consumed directly in combinational ROM logic clocked by `clk_sys`. No synchronizer flip-flops. Torn reads across nibble pairs (two separate array entries from the same byte) can yield one stale nibble and one new nibble simultaneously.

**Impact:** Autoconfig serial number corruption during MAC write; Amiga driver rejects card or assigns wrong address.

**Fix:** Add 2-stage synchronizer registers for each `mac_byte*` input, same pattern as `a2065_int2` in `minimig.v`.

#### Reviewer Assessment (2026-05-31)

**Verdict: Partially valid, but overstated risk.**

**What's wrong with the review's claim:** The review states "No synchronizer flip-flops" — but `cpu_wrapper.v:375-383` already has a registered stage for the MAC nibbles:
```verilog
always @(posedge clk) begin
    mac_nibble_2h <= ~a2065_mac_byte2[7:4];
    mac_nibble_2l <= ~a2065_mac_byte2[3:0];
    ...
end
```
This is a 1-stage synchronizer in `clk_sys` domain. The review's claim of "no synchronizer" is incorrect — there IS a register stage, just not a 2-stage one.

**The actual CDC risk:** `a2065_mac_byte*` inputs come from the ARM daemon via the DDR3 mailbox (`clk_audio` domain). In the current Minimig integration, these signals are wired as `A2065_MAC_BYTE*` input ports on `Minimig.sv`, passed through to `cpu_wrapper.v`. The 1-stage register in `cpu_wrapper` provides some protection, but a 2-stage synchronizer would be more robust.

**Why the risk is negligible in practice:** The ARM daemon writes MAC bytes once at startup (`write_mbx_mac()` in `main_ddr3.cpp`), before the Amiga boots and before autoconfig reads begin. The MAC bytes never change during operation. The time between MAC write and first autoconfig read is seconds (boot time), making metastability practically impossible. This is not a dynamic signal that changes during normal operation.

**Priority: Low.** A 2-stage synchronizer would be cleaner engineering practice and would be needed for portability, but the current implementation works reliably because the MAC is written once during a long static window.

---

### H4 — FPGA boardram not initialized — powers up with unknown state
**File:** `fpga/rtl/a2065_boardram.v:45–46`

`reg [15:0] ram [0:16383]` has no `initial` block. In simulation it starts as `'x`; on Cyclone V M10K it powers up as all-zeros (guaranteed by Intel), but this is device-specific behavior not guaranteed by the Verilog standard.

**Impact:** Simulation failures with X-propagation; on non-Cyclone-V targets, boardram powers up with garbage → `chip_init()` reads corrupt init block.

**Fix:**
```verilog
initial begin : ram_init
    integer i;
    for (i = 0; i < 16384; i = i + 1)
        ram[i] = 16'h0000;
end
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid for simulation correctness, no hardware impact.**

**On Cyclone V (target hardware):** Intel (Altera) M10K embedded memory blocks power up with deterministic all-zeros state. This is documented in the Cyclone V Device Handbook and is a guaranteed behavior, not an implementation detail. The 32KB boardram will always be zero-initialized on the DE10-Nano.

**In simulation:** Without an `initial` block, the `ram` array starts as `'x` (unknown), causing X-propagation in testbenches. This makes `tb_boardram.v` and `tb_bridge_e2e.v` simulations unreliable for testing boardram-dependent paths.

**Recommendation:** Add the `initial` block — it costs nothing in hardware (M10K already initializes to zero) and fixes simulation. Quartus synthesizes `initial` blocks on M10K as power-up values, so there's no area or timing penalty. This is good Verilog hygiene.

---

### H5 — No bounds check on TX/RX ring length from untrusted boardram
**File:** `arm/src/rings.cpp:57–63`

`tdr_tlen` and `rdr_rlen` are read from boardram (written by Amiga). If a buggy driver sets `tlen = 0`, the modulo `offset % 0` is undefined behavior. If it sets `tlen = 0x8000`, the descriptor address `tdr_tdra + offset * 8` overflows.

**Impact:** UB / crash in daemon; potential read of arbitrary host memory.

**Fix:**
```cpp
if (!tdr_tlen || tdr_tlen > 512) { /* log error */ return 0; }
if ((uint32_t)tdr_tdra + ((uint32_t)tdr_tlen - 1) * 8 >= BOARDRAM_SIZE) return 0;
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid — real edge case that could crash the daemon.**

**Analysis of the two init paths:**

1. **`chip_init()` path** (registers.cpp:116-117): `am_rdr_rlen = 1u << ((rdr >> 29) & 7)`. This always produces powers of 2 in range [1, 128]. Cannot produce 0. This path is safe.

2. **CSR76/78 direct write path** (registers.cpp:242-243): `am_tdr_tlen = (uint32_t)(-(int16_t)v) & 0xffff`. This CAN produce 0 (when `v=0`) or large values up to 65535. The `do_transmit()` function has `if (!tdr_tlen) return 0;` at line 60 which guards against zero for the modulo, but there's no guard against very large values that could cause `tdr_tdra + offset * 8` to overflow the 32KB boardram.

**Impact in practice:** The Amiga A2065 driver always initializes rings through `chip_init()`, never through direct CSR76/78 writes. The `& 7` mask in the init path limits ring lengths to 128. A buggy or malicious Amiga program could theoretically write CSR78 directly, but this would require custom Amiga-side software.

**Recommendation:** Add bounds checking in `do_transmit()` and `gotfunc()` for defensive programming. The suggested fix is reasonable and low-risk.

---

### H6 — `pthread_create` return value unchecked
**File:** `arm/src/main_ddr3.cpp:383–384`

`pthread_create(&rx_tid, NULL, rx_thread, NULL)` return value is discarded. If thread creation fails, `pthread_join()` at line 429 is called on an uninitialised `rx_tid`, which is undefined behavior and likely hangs.

**Impact:** Silent RX path failure; daemon appears to run but receives no packets.

**Fix:**
```cpp
if (pthread_create(&rx_tid, NULL, rx_thread, NULL) != 0) {
    perror("[a2065d] pthread_create");
    goto cleanup;
}
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid — real risk of undefined behavior on resource exhaustion.**

`pthread_create()` can fail with `EAGAIN` (insufficient resources) or `EINVAL` (invalid attributes). On a resource-constrained embedded system like the DE10-Nano ARM core, thread creation failure is plausible under memory pressure.

If `pthread_create()` fails, `rx_tid` remains uninitialized (stack garbage). The subsequent `pthread_join(rx_tid, ...)` at shutdown is undefined behavior — it could hang, crash, or corrupt memory.

The suggested fix (check return value and goto cleanup) is correct. However, the cleanup path needs to be verified — a `goto cleanup` would skip `pthread_join()` which is the right behavior when the thread was never created.

**Priority: Medium.** In practice, thread creation rarely fails on Linux, but the fix is trivial and prevents a potential hang on shutdown.

---

## Medium

### M1 — TX packet size can exceed output buffer
**File:** `arm/src/rings.cpp:112–116`

`size = (int)(65536 - tmd2)`. If `tmd2 == 0`, size = 65536. The cap `if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE` catches this for single-descriptor frames, but multi-descriptor chains accumulate into `outsize`; if the `TX_ENP` flag is missing, the loop continues until `tdr_tlen` wraps, repeatedly accumulating up to `MAX_PACKET_SIZE * tdr_tlen` bytes.

**Impact:** Potential heap buffer overrun in `txbuf`.

**Fix:** Break the chain loop immediately if `outsize >= MAX_PACKET_SIZE`, regardless of `TX_ENP`.

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid — real heap buffer overrun potential.**

The TX chain loop in `do_transmit()` (rings.cpp:90-127) accumulates data into `transmitbuffer[]` across multiple descriptors. The per-descriptor cap `if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE` limits each iteration, and `if (outsize > MAX_PACKET_SIZE) outsize = MAX_PACKET_SIZE` caps the running total — but only after the `ram_read_block()` write has already happened. If `TX_ENP` is missing from all descriptors (malformed or buggy driver), the loop walks the entire ring (`tdr_tlen` descriptors), each adding up to `MAX_PACKET_SIZE` bytes. The `outsize` cap at line 117 prevents the accumulation variable from wrapping, but the actual `transmitbuffer[]` write at line 115 uses `outsize` *before* the cap, so it writes past the buffer.

The suggested fix (break on `outsize >= MAX_PACKET_SIZE`) is correct. Should be placed before the `ram_read_block()` call.

---

### M2 — `mmap` pointer not checked for NULL before use in `boardram_remote.cpp`
**File:** `arm/src/boardram_remote.cpp:18–25`

`rd64()`/`wr64()` dereference `map + off` without verifying `map != NULL`. If `boardram_xfer()` is called before `ddr3_mmap_init()`, the NULL deref crashes the daemon.

**Fix:**
```cpp
static inline uint64_t rd64(volatile uint8_t *map, size_t off) {
    if (!map) return 0;
    ...
}
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid but low practical risk.**

`boardram_remote_init()` is called in `main()` at line 373 before any boardram access. The `map` pointer is set there. There is no code path that calls `boardram_xfer()` before initialization in the current daemon. However, defensive programming dictates the NULL check.

**Note:** The review's suggested fix changes the function signature to pass `map` as a parameter, which doesn't match the current architecture where `map` is a file-static variable. A simpler fix is an early return in `boardram_xfer()`:

```cpp
if (!map) return 0xFFFF;
```

---

### M3 — Interface name not null-terminated in `ethernet.cpp`
**File:** `arm/src/ethernet.cpp:38–43`

`strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1)` does not guarantee null termination if `iface` is exactly `IFNAMSIZ - 1` bytes. Subsequent `ioctl()` with an unterminated string causes kernel to read beyond buffer.

**Fix:**
```cpp
strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
ifr.ifr_name[IFNAMSIZ - 1] = '\0';
```

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid — classic strncpy pitfall.**

`strncpy(dst, src, n)` writes at most `n` bytes. If `src` is exactly `n` bytes long (no null within the first `n` characters), the destination is NOT null-terminated. With `IFNAMSIZ` typically 16 on Linux, an interface name like `"123456789012345"` (15 chars) would fill `ifr_name` without a null terminator.

In practice, the daemon uses `--iface eth0` or `eth1` (4-5 chars), so this won't trigger. But the fix is a one-liner and prevents a latent bug if someone passes a long interface name.

---

### M4 — Watchdog BERR assertion without checking AS# still active
**File:** `Minimig-AGA_MiSTer/rtl/A2065/a2065_registers.v` (ST_BERR state)

BERR is asserted after watchdog expires, but if `cpu_as_n` has already gone high (68k released the bus), asserting BERR is a spurious bus-error that corrupts an unrelated cycle.

**Fix:** Gate BERR assertion on `!cpu_as_n`; if AS# already high, return to ST_IDLE silently.

#### Reviewer Assessment (2026-05-31)

**Verdict: Already fixed in current code — the review's suggested fix is already implemented.**

`a2065_registers.v:195-201` (ST_BERR state):
```verilog
ST_BERR: begin
    cpu_berr_n <= 1'b0;
    if (cpu_as_n) begin
        cpu_berr_n <= 1'b1;
        state      <= ST_IDLE;
    end
end
```

The code already checks `cpu_as_n` and returns to ST_IDLE with BERR deasserted if AS# has gone high. The fix the review suggests is already present. Note also that `cpu_berr_n` is not connected in the current Minimig integration (documented in CLAUDE.md as a known issue), so BERR is never seen by the 68000 regardless.

---

### M5 — RAP register unbounded `int` used as array index
**File:** `arm/src/registers.cpp:20–21, 135, 148`

`rap` is declared as `int`. In `chip_wput()` it is set directly from the 16-bit write value with only a `>= RAP_SIZE` check after use. Between write and check, `csr[rap]` is not accessed, so it's safe currently — but a future refactor that reorders the check would allow out-of-bounds access.

**Fix:** Declare `static uint8_t rap = 0` and enforce `rap = (uint8_t)(v & 0x7f)` on write.

#### Reviewer Assessment (2026-05-31)

**Verdict: Valid observation but no actual bug — current code is safe.**

The review correctly notes `rap` is `int`, but the actual code paths are safe:

1. **RAP write** (`chip_wput` line 148): `rap = v & 0x7f` — masked to 7 bits (0-127). `RAP_SIZE` is 128. So `rap` is always `< RAP_SIZE` after a RAP write.
2. **RDP access** (`chip_wget/wput`): Both check `if (rap >= RAP_SIZE) return` before using `rap` as an array index (lines 153, 135). After the `& 0x7f` mask, `rap` can be at most 127, which equals `RAP_SIZE - 1`.
3. **No window between write and check:** The mask is applied in the RAP write path. The RDP path reads `rap` and checks bounds before any array access. There is no code path where an unmasked `rap` value is used as an array index.

Narrowing to `uint8_t` would be marginally safer (prevents negative values from memory corruption), but the current `int` is not a practical risk.

---

### M6 — `service_bridge_safe` silently drops CSR0 writes
**File:** `arm/src/main_ddr3.cpp` (`service_bridge_safe` function)

`service_bridge_safe()` is called during boardram DDR3 transactions. It sends REG_RSP for any pending request but only calls `chip_wput()` for RAP writes (addr==2). CSR0 writes (addr==0) receive a response but are NOT forwarded to the state machine. This is documented in CLAUDE.md as a known issue but the silently-dropped write is invisible to callers.

**Impact:** Amiga CSR0 clears (e.g. IDON clear, TX status clear) are lost during `chip_init()` / `do_transmit()`. Causes stale interrupt state and Amiga polling timeouts.

**Fix:** Queue the dropped CSR0 write for processing after the boardram operation completes (the deferred queue in C2 above is meant for this — but it's unsynchronised).

#### Reviewer Assessment (2026-05-31)

**Verdict: Already fixed — the deferred CSR0 queue exists and is now mutex-protected.**

The current `service_bridge_safe()` (main_ddr3.cpp:246-251) enqueues CSR0 writes into an 8-entry FIFO queue (`deferred_csr0_queue[8]`). The main loop drains this queue after each `service_bridge()` call (lines 424-429), processing each deferred CSR0 write via `chip_wput()`.

The C2 mutex fix added `registers_lock()/registers_unlock()` around the deferred queue drain, so the "unsynchronised" concern from the review is also addressed.

The only remaining risk is queue overflow (8 entries) during heavy collision test processing — if 9+ CSR0 writes arrive during a single `do_transmit()` boardram sequence, the 9th is silently dropped. This is unlikely but could be mitigated by increasing the queue size to 32.

---

## Low

### L1 — Hardcoded MAC serial bytes in `cpu_wrapper.v` autoconfig nibbles
**File:** `Minimig-AGA_MiSTer/rtl/cpu_wrapper.v` (A2065 autoconfig ROM)

Serial number nibbles for the Zorro II config are hardcoded (0x70, 0x70, 0x70). The ARM daemon writes a proper MAC via `MBX_MAC`, but the autoconfig serial number seen during board config is fixed. Two A2065 cards in the same system would have identical serial numbers, causing Zorro II resource conflict.

**Fix:** Derive serial nibbles from last 3 bytes of MAC, driven by the same `MBX_MAC` mechanism.

---

### L2 — MAC byte ordering in `write_mbx_mac()` undocumented
**File:** `arm/src/main_ddr3.cpp:157–168`

`write_mbx_mac()` packs bytes with specific shifts into a 64-bit DDR3 word. No comment explains the bit layout or whether it matches the FPGA `MBX_MAC` decoder endianness. A discrepancy here produces a subtly wrong MAC (nibble-swapped or byte-reversed) that passes basic tests but fails ARP on real networks.

**Fix:** Add an explicit bit-field diagram comment and a startup log that prints the decoded MAC to verify round-trip.

---

### L3 — `a2065_test.c` uses fixed physical address without validation
**File:** `tests/a2065_test.c`

Test opens `/dev/mem` and maps the A2065 card base at a hardcoded address. No check that the card actually autoconfigured to that address. If Zorro II assigns a different base (different slot, different connected cards), the test silently accesses wrong memory.

**Fix:** Read the configured base from the OS (`FindConfigDev()` on AmigaOS) rather than using a hardcoded constant.

---

### L4 — `cpu_berr_n` not connected in register bridge
**File:** `CLAUDE.md` / `arm/include/a2065_bridge.h`

Documented in CLAUDE.md: watchdog timeout returns `$0000` instead of asserting `BERR`. A well-behaved card must bus-error on timeout so AmigaOS can recover. Currently the Amiga sees a silently-zero register value and may interpret it as valid data (e.g., CSR0=0 = STOP).

**Fix:** Wire `cpu_berr_n` from `a2065_registers` output to `minimig.v` bus error input.

---

## Findings Not Verified (Require Hardware Test)

- **Collision test TDMD visibility (24% failure rate):** Daemon never sees TDMD writes during collision test's `lance_send_pkt`. Root cause unclear — possibly mailbox adapter stuck during concurrent RAM_REQ, or register bridge intermittent. Requires logic-analyser trace or FPGA debug counter.

- **Interrupt timing race (16% failure rate):** FPGA mailbox adapter busy with RAM_REQ during `chip_init()` and not polling `MBX_INT`. The pre-assert + hold-counter mitigation reduces but does not eliminate. An FPGA-side fix (latch INT2 immediately when `MBX_INT` is written, not poll-based) would be more robust.

- **Thread safety of `boardram_xfer()`:** Called from main thread only currently, but if `gotfunc` ever triggers a boardram write path, concurrent `boardram_xfer()` calls will corrupt the `RAM_REQ`/`RAM_RSP` mailbox handshake. No mutex protection exists.

---

*End of review.*
