# A2065 lance-test Iterative Fix Methodology

## Context

You are working on the A2065 project — a full hardware emulation of the Commodore A2065 ZorroII Ethernet card for the Minimig FPGA core on MiSTer. The project is at Step 10 (Stress test & polish). The FPGA core is built and stable (build `Minimig_20260528a.rbf`). The ARM daemon (`a2065d_ddr3`) runs on the MiSTer's HPS Linux side. All fixes are ARM-daemon-side only (no FPGA rebuilds needed).

## Goal

Run the Amiga `lance-test` diagnostic binary 100 times via serial, capture pass/fail rates for 4 subtests (Buffer memory, LANCE config, Interrupt, Collision logic), identify the weakest test, fix one issue at a time, and iterate until all 4 tests pass 100/100.

## Test Infrastructure

- **100x runner:** `cd tests && A2065_CORE=Minimig_20260528a.rbf .venv/bin/python run_lance_100x.py 100`
- **Runner behavior:** Loads core once, then for each run: kills daemon, starts fresh daemon, runs lance-test via serial, parses 4 subtest results, saves daemon stderr log on failure to `lance_logs/daemon_XXX_*.log`, writes results markdown to `lance_logs/`
- **Daemon restart between runs is essential** — without it, the FPGA mailbox adapter gets stuck after ~3 runs
- **Results format:** Markdown table with PASS/FAIL/MISSING counts and pass rate per test
- **Daemon logs:** Last 500 lines of `/tmp/a2065d.log` captured on failure; debug limit is 5000 requests
- **Core env var:** `A2065_CORE` specifies which RBF to load

## Build & Deploy Cycle

```
# 1. Edit ARM source locally
# 2. Cross-compile on build server
scp arm/src/<file> root@192.168.1.97:/opt/development/minimig/A2065/arm/src/
ssh root@192.168.1.97 'cd /opt/development/minimig/A2065/arm && make ddr3'

# 3. Deploy to MiSTer (relay through dev machine)
ssh root@192.168.1.29 'killall a2065d_ddr3 2>/dev/null; sleep 1; rm -f /media/fat/trans/a2065d_ddr3'
scp root@192.168.1.97:/opt/development/minimig/A2065/arm/build/ddr3/a2065d_ddr3 /tmp/a2065d_ddr3
scp /tmp/a2065d_ddr3 root@192.168.1.29:/media/fat/trans/a2065d_ddr3

# 4. Run 100x test
cd tests && A2065_CORE=Minimig_20260528a.rbf .venv/bin/python run_lance_100x.py 100
```

## Key Files

| File | Role |
|------|------|
| `arm/src/main_ddr3.cpp` | Daemon main loop: `service_bridge()`, `service_bridge_safe()`, MBX_INT management, int_hold counter |
| `arm/src/registers.cpp` | Am7990 CSR state machine: `chip_wput()`, `chip_wget()`, `chip_init()`, `rethink()`, callback dispatch |
| `arm/src/rings.cpp` | TX/RX descriptor ring walker: `do_transmit()`, `gotfunc()`, collision detection, TX_OWN clearing |
| `arm/src/boardram_remote.cpp` | DDR3-backed boardram access: `boardram_rw()`, `boardram_ww()`, calls `service_bridge_safe` during waits |
| `arm/include/a2065_types.h` | CSR0/TX/RX bit definitions (CSR0_STOP=0x0004, CSR0_INIT=0x0001, CSR0_STRT=0x0002, etc.) |
| `arm/include/boardram_access.h` | `RAM_MASK=0x7FFF`, inline accessors with mask |
| `tests/run_lance_100x.py` | 100x runner with daemon restart, serial parsing, results markdown |

## Methodology — Strict Rules

### 1. One fix at a time
- Change exactly one thing in the ARM daemon source
- Cross-compile, deploy, run 100x
- Compare with previous run to confirm improvement or detect regression
- If regression, revert immediately

### 2. Analyze before fixing
- Before writing any code, read the daemon stderr log from a failure
- Trace the register request sequence (req numbers, addr, data, rsp)
- Identify exactly WHERE the failure occurs in the request flow
- Form a hypothesis before coding

### 3. Use diagnostic logging
- Add `fprintf(stderr, ...)` to the relevant code path
- Key patterns:
  - `[a2065] TX ...` — transmit activity
  - `[a2065] chip_init: mode=...` — init block contents
  - `[a2065d] assert_mbx_int()` — interrupt assertion
  - `[a2065d] write_mbx_int: X→Y csr0=ZZZZ` — interrupt state change
  - `[req N] ...` — register request/response trace
  - `[req N(safe)] ...` — request processed during boardram DDR3 wait
- Increase `dbg_cnt < N` limit if needed to capture more requests

### 4. Daemon log analysis patterns
- **No TX output in log:** `do_transmit()` never called → check TDMD/STRT handling in `chip_wput()`
- **INIT without STRT:** Amiga writes `data=0041` (INIT+INEA) but never `data=0002` (STRT) → check if STRT is required for TDMD
- **MBX_INT goes 1→0 too fast:** `write_mbx_int` deasserts before FPGA polls → increase `int_hold` or pre-assert
- **`(safe)` requests during INIT:** `service_bridge_safe` drops CSR0 writes → only forwards RAP
- **Mode always 0x0000:** Collision test does NOT set MODE_COLL in init block → use self-loopback detection instead
- **Repeated STOP/INIT/clear_IDON without STRT/TDMD:** Collision test's init function works but packet sends are invisible to daemon

### 5. Critical constraints
- **NEVER modify the arbiter** (`avalon_arbiter.v`) — any change breaks DDR3
- **REG_RSP must be written before `chip_wput()`** — deadlock prevention (chip_init needs boardram DDR3 which requires FPGA to be free)
- **`service_bridge_safe` must NOT forward CSR0 writes** — reentrancy in `do_transmit()` via `on_transmit()` callback
- **`am_initialized` must not be reset on STOP** — collision test does STOP → STRT+TDMD without re-INIT
- **Daemon must be restarted between runs** — FPGA mailbox adapter gets stuck after ~3 runs without restart

## Interrupt Path (most timing-sensitive)

```
ARM rethink() → on_interrupt_cb() → assert_mbx_int()
  → writes MBX_INT=1 to DDR3 (triple-write + sync barrier)
  → sets int_hold=5000

FPGA mailbox adapter (S_IDLE, poll_div==0x1F, every 32 cycles at 49MHz ≈ 0.65µs)
  → reads MBX_INT from DDR3 → latches a2065_int2
  → CDC 2-stage to clk_sys → Paula INTREQ bit 3 (PORTS)
  → 68000 level 2 interrupt acknowledge

ARM main loop:
  if int_hold > 0: int_hold--
  else: write_mbx_int()  # evaluates CSR0_INTR && CSR0_INEA
```

**Race condition:** During `chip_init()` boardram DDR3 access (~200-500µs), the FPGA adapter processes RAM_REQ/RSP and does NOT poll MBX_INT. If MBX_INT was asserted during this window, the FPGA misses it. Mitigated by pre-asserting MBX_INT before REG_RSP and holding for 5000 iterations.

## CSR0 Bit Definitions (different from typical assumptions!)

```
CSR0_INIT  = 0x0001  (bit 0)
CSR0_STRT  = 0x0002  (bit 1)
CSR0_STOP  = 0x0004  (bit 2)
CSR0_TDMD  = 0x0008  (bit 3)
CSR0_TXON  = 0x0010  (bit 4) — NOTE: not 0x0020
CSR0_RXON  = 0x0020  (bit 5) — NOTE: not 0x0010
CSR0_INEA  = 0x0040  (bit 6)
CSR0_INTR  = 0x0080  (bit 7)
CSR0_IDON  = 0x0100  (bit 8)
CSR0_TINT  = 0x0200  (bit 9)
```

Always verify against `arm/include/a2065_types.h` — the bit positions are NOT the standard Am7990 datasheet values.

## Session Progress Template

Track each fix attempt in a table:

| Version | Change | Buffer | Config | Interrupt | Collision | ALL PASS |
|---------|--------|--------|--------|-----------|-----------|----------|
| baseline | (initial) | 100% | 94% | 34% | 0% | ~0% |
| v1 | description | ... | ... | ... | ... | ... |

## Stopping Criteria

- Stop when no single fix improves any test by >5% over 3 consecutive attempts
- Or when ALL 4 tests reach 95%+ pass rate
- Or when the remaining failures are clearly FPGA-side (require a Quartus rebuild)

## Common Failure Modes

1. **Interrupt race:** Amiga clears CSR0 IDON before FPGA polls MBX_INT → test sees no interrupt
2. **Collision invisible:** Amiga writes STRT+TDMD but daemon never sees it → do_transmit not called
3. **Daemon stuck:** After 3+ runs without restart, mailbox adapter in S_REG_DONE → all subsequent runs fail
4. **LANCE config fail:** Init doesn't complete because chip_init() boardram access is slow via DDR3
5. **Stale CSR0:** service_bridge_safe drops CSR0 writes during do_transmit → Amiga reads stale flags
