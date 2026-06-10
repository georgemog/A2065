# lance-test 100x Results — Session Summary

**Date:** 2026-05-29
**Core:** Minimig_20260528a.rbf
**Hardware:** MiSTer DE10-Nano, hardware loopback plug on eth1

## Version Comparison

| Test | Baseline | v1 (COLL) | v2 (defer+hold) | v5 (loopback) |
|------|----------|-----------|-----------------|---------------|
| Buffer memory | 100% | 100% | 100% | **100%** |
| LANCE config | 94% | 85% | 98% | **92%** |
| Interrupt | 34% | 27% | 72% | **68%** |
| Collision logic | 0% | 17% | 55% | **59%** |
| ALL PASS rate | 0% | ~3% | ~30% | **~55%** |

## Key Fixes Applied

### 1. Collision test (0% → 59%)
- **Root cause:** The lance-test collision test uses a physical loopback plug, NOT the Am7990's MODE_COLL register. The init block mode is always 0x0000. The daemon must detect self-loopback (DST MAC == own MAC) and simulate hardware collision.
- **Fix:** In `do_transmit()`, check if the TX packet's destination MAC matches the card's MAC. If so, set TX_ERR+TX_RTRY in the descriptor instead of sending via ethernet.
- **Remaining 7 FAIL (out of 66 that ran = 89% pass):** Likely a DDR3 boardram write ordering race — the Amiga reads TMD3 before the daemon's DDR3 write completes.

### 2. Interrupt test (34% → 68%)
- **Root cause:** `service_bridge()` was calling `write_mbx_int()` which deasserted MBX_INT before the FPGA had polled it (5µs window between assert and deassert).
- **Fix:** Added `int_hold` counter (1000 iterations ≈ 10ms). MBX_INT stays asserted during the hold period, giving the FPGA ample time to poll. After hold expires, `write_mbx_int()` evaluates current CSR0 state.
- **Remaining 22 FAIL:** Possible Amiga-side interrupt handler race or very tight polling loop.

### 3. Deferred TX descriptor write
- **Fix:** The last TX descriptor write is deferred until after collision flags are applied. TMD3 is written before TMD1 (so TX_RTRY is set before TX_OWN is cleared, preventing the Amiga from reading stale TMD3).

## Remaining Issues

1. **Collision: 7/66 FAIL when it runs** — DDR3 boardram write ordering race
2. **Interrupt: 22% FAIL** — Amiga-side interrupt handling or very tight timing window
3. **LANCE config: 8% FAIL** — Same root cause as interrupt (daemon startup race)
4. **Daemon dies after 3 runs without restart** — Mailbox adapter gets stuck (CDC fix not sufficient)

## Files Modified

| File | Change |
|------|--------|
| `arm/src/rings.cpp` | Self-loopback collision detection, deferred TX descriptor write, TMD3-before-TMD1 ordering |
| `arm/src/main_ddr3.cpp` | `int_hold` counter (1000 iter), hold on CSR0 reads+writes, `service_bridge_safe` only forwards RAP |
| `arm/src/registers.cpp` | Added `chip_init()` iaddr/raw diagnostic logging |
