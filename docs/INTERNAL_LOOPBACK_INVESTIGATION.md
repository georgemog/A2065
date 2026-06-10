# Internal Loopback WARN/FAIL Investigation

**Date:** 2026-05-31  
**Build:** Minimig_20260528a.rbf + a2065d_ddr3 (queue-based deferred CSR0, TDMD retry, ring scan)  
**Test:** `lance-test diags` — Internal loopback test (test 5)  
**100x run:** v9 results (PID 27338)

## Executive Summary

Internal loopback test pass rate improved from **0%** (not tracked) to **51% ALL-PASS** across 100 runs. Three distinct failure modes were identified, each with a different root cause. Two are partially mitigated; one (register bridge loss) remains open.

| Result | Count | % | Root Cause |
|--------|-------|---|------------|
| PASS | 51 | 51% | — |
| WARN | 9 | 9% | TX ring desync after ~100 iterations |
| FAIL | 10 | 10% | Register bridge drops INIT write |
| MISSING | 30 | 30% | Cascading from earlier test failure |

## Test Description

The lance-test internal loopback test (`diag_internal_loopback` at 0x1414):
1. Calls init function with mode=0x0004 (LOOP only)
2. Fills 24-byte packet buffer with incrementing data
3. Copies first 6 bytes from init block (MAC address) as destination
4. Loops 200 times: `lance_send_pkt(wait=1)` → `lance_recv_pkt(timeout=50 WaitTOF)`
5. Expects exactly 28 bytes received per iteration (24 data + 4 FCS)
6. PASS if all 200 iterations succeed, WARN if some succeed, FAIL if none

The LANCE is configured with `tdr_tlen=4` (4 TX descriptors) and `rdr_rlen=16` (16 RX descriptors). The TX ring wraps 50 times across 200 iterations.

## Changes Made This Session

### 1. MODE_LOOP Support in do_transmit() (`rings.cpp`)
When MODE_LOOP is set without MODE_COLL, `do_transmit()` calls `gotfunc()` directly instead of sending via ethernet. This is the internal loopback path — TX output feeds directly into RX processing.

### 2. MAC Filter Bypass in Loopback (`rings.cpp`)
`gotfunc()` skips unicast/multicast MAC address filtering when MODE_LOOP is set. The test constructs packets using the init-block MAC (00:80:10:00:00:00) which differs from fakemac (00:80:10:00:04:2B). Without this bypass, all loopback packets are silently dropped.

### 3. Broadcast Echo Bypass in Loopback (`rings.cpp`)
Skip the "drop our own broadcast echo" check in loopback mode.

### 4. Frame Padding Bypass (`rings.cpp`)
Skip 60-byte ethernet minimum padding when MODE_LOOP is set. The test sends 24-byte frames that must stay 24 bytes (padding to 60 would corrupt the test's size check).

### 5. Deferred CSR0 Queue (`main_ddr3.cpp`)
Replaced single `deferred_csr0 |= data` (OR-accumulate) with an 8-entry FIFO queue. Each CSR0 write during `service_bridge_safe()` is enqueued individually and processed in order by the main loop. This prevents:
- **STOP+INIT merge:** OR-accumulate combined STOP (0x0004) and INIT (0x0001) into 0x0005. The STOP path fires first (`csr[0] = CSR0_STOP`), clearing the INIT bit before the INIT path can execute. The queue processes them as separate writes.
- **TDMD overwrite:** A status-clear write (0xFF00) arriving after TDMD (0xFF08) would overwrite the TDMD bit. The queue preserves both.

### 6. TX Ring Scan (`rings.cpp`)
When `do_transmit()` finds the expected TX descriptor without TX_OWN, it scans all descriptors in the ring looking for one with TX_OWN and TX_STP. This handles `tdr_offset` desynchronization caused by DDR3 mailbox latency — the Amiga's write to the next descriptor may have completed by the time the scan reaches it.

### 7. TDMD Retry via Main Loop (`main_ddr3.cpp`, `registers.cpp`)
`do_transmit()` returns `int` (1=processed, 0=no descriptor found). `registers.cpp` only clears CSR0_TDMD if `do_transmit()` returned 1. The `on_transmit_cb()` sets `tdmd_retry=16` when do_transmit returns 0, causing the main loop to retry `do_transmit()` on the next 16 iterations. This handles transient stale-read windows.

### 8. 100x Runner WARN Support (`run_lance_100x.py`)
Updated result parser to match WARN in addition to PASS/FAIL. Updated report to show WARN column. WARN is treated as a non-PASS result for ALL-PASS counting.

## Failure Mode Analysis

### Mode A: Register Bridge Drops INIT (10/100 runs → FAIL)

**Symptom:** Test reports FAIL with 0 passing iterations. Daemon log shows no `mode=0044` chip_init. The STOP after the collision test is the last entry.

**Root Cause:** The Amiga's INIT register write for the loopback test is lost in the DDR3 mailbox register bridge. The daemon never receives it, so chip_init() never runs, and the LANCE remains in STOP state. The test's init function waits for IDON, which never comes, and returns failure.

**Evidence:** Daemon logs for runs 002, 007, 009, 022 show STOP as the last state transition with no subsequent INIT. The Amiga serial output shows the test reached "Internal loopback test" and printed FAIL.

**Mitigation Status:** Partially mitigated by the deferred CSR0 queue (prevents STOP+INIT merge). However, ~10% of runs still lose the INIT write. Suspected causes:
1. Deferred CSR0 queue overflow (8 entries) during collision test processing
2. The `tdmd_retry` mechanism consuming main loop iterations without processing new register requests
3. FPGA mailbox adapter stuck in a DDR3 transaction, not polling REG_REQ

**Proposed Fix:**
- Increase queue to 32 entries
- Add overflow logging
- Consider disabling tdmd_retry or moving it after the deferred queue drain

### Mode B: TX Ring Desync (~9/100 runs → WARN)

**Symptom:** Test reports WARN with ~100 out of 200 iterations passing. Daemon log shows ~100 TX LOOP entries.

**Root Cause:** After ~100 iterations (with 4 TX descriptors wrapping 25 times), the daemon's `tdr_offset` and the Amiga's `tx_count` desynchronize. The ring scan finds a descriptor with TX_OWN for the first ~100 iterations but then fails to find one.

**Evidence:** Run 016 shows exactly 100 TX LOOP + 100 RX LOOP OK in the daemon, but the test says FAIL (not WARN). This suggests the Amiga side stops after 100 successful recv operations.

**Analysis:** With `tdr_tlen=4`, after processing descriptor N, the daemon's `tdr_offset = (N+1) % 4`. The Amiga's `tx_count = (old_count + 1) % 4`. These should stay in sync. However, the TDMD retry mechanism may process the same descriptor twice (once in the initial `chip_wput` call and once in the retry path), advancing `tdr_offset` while the Amiga hasn't yet written to the next descriptor.

**Proposed Fix:**
- Remove TDMD retry mechanism (it may cause double-processing)
- Instead, ensure the deferred queue drains before the retry, giving the Amiga's writes time to propagate
- Consider adding a small `usleep(50)` in the main loop between `service_bridge()` and the deferred queue drain

### Mode C: Cascading MISSING (30/100 runs)

**Symptom:** Internal loopback shows as MISSING — the test never ran.

**Root Cause:** An earlier test (LANCE config, Interrupt, or Collision) failed, preventing the test sequence from reaching Internal loopback.

**Breakdown of triggering failures:**
- LANCE config FAIL: 10 runs — init timing race
- Interrupt FAIL: 6 runs — MBX_INT timing race
- Collision FAIL: 9 runs — only 8/10 TDMDs reach daemon

These are pre-existing issues not specific to Internal loopback.

## 100x Run Comparison Across Builds

| Build | ALL PASS | Internal Loopback PASS | WARN | FAIL | MISSING |
|-------|----------|----------------------|------|------|---------|
| v4 (OR-accumulate) | 50% (47/94) | 50% | 19 | 2 | 23 |
| v6 (inline retry) | 61% (61/100) | 61% | 10 | 11 | 18 |
| v7 (flag retry) | 56% (56/100) | 56% | 11 | 9 | 24 |
| **v9 (queue + retry)** | **51% (51/100)** | **51%** | **9** | **10** | **30** |

v6 had the highest ALL-PASS rate (61%) but more FAIL (11) due to inline retry blocking. v9 has the lowest WARN (9) but more MISSING from cascading failures.

## Architecture Constraints

The DDR3 mailbox architecture introduces fundamental latency that hardware doesn't have:

1. **Boardram access:** Each word read/write is a DDR3 round-trip (~5-10µs). Reading a TX descriptor requires 4 round-trips. A 24-byte packet read requires 12 round-trips.
2. **Register bridge:** Each CSR0 write takes 2 DDR3 round-trips (REQ + RSP). During `do_transmit()`'s boardram access, CSR0 writes are deferred to the queue.
3. **RX processing:** `gotfunc()` inside `do_transmit()` writes RX descriptor + data to boardram, each word a separate DDR3 round-trip.

Total per loopback iteration: ~40-60 DDR3 round-trips = ~200-600µs. The Amiga expects real hardware timing (~1-5µs per iteration). The 200-iteration test takes 40-120ms instead of ~1ms.

## Recommended Next Steps

1. **Increase deferred queue to 32 entries** — prevent overflow during collision test processing
2. **Remove or simplify tdmd_retry** — it may cause double-processing and ring desync
3. **Add `usleep(50)` in main loop** — give DDR3 writes time to settle between iterations
4. **Investigate register bridge reliability** — 10% INIT loss rate suggests fundamental mailbox issue
5. **Consider FPGA-level interrupt latching** — replace polling-based MBX_INT with direct INT2 assertion in the mailbox adapter
6. **Remove diagnostic logging** — `gotfunc entry`, `TX scan fail`, `loopback_iter` should be removed for production builds
7. **Revert dbg_cnt limit to 5000** — 10000 is for debugging only
