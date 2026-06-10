# lance-test 100x Results — Baseline

**Date:** 2026-05-29 01:32
**Core:** Minimig_20260528a.rbf
**Daemon:** a2065d_ddr3 (loopback RX block removed)
**Total runs:** 100
**Valid runs:** 100
**Note:** Hardware loopback plug on eth1

## Summary

| Test | PASS | FAIL | MISSING | Pass Rate |
|------|------|------|---------|-----------|
| Buffer memory | 100 | 0 | 0 | 100% |
| LANCE config | 94 | 6 | 0 | 94% |
| Interrupt | 34 | 55 | 11 | 34% |
| Collision logic | 0 | 33 | 67 | 0% |

## Analysis

**Priority 1: Interrupt test (34% pass rate)**
- 55 FAIL + 11 MISSING (MISSING = serial truncation due to slow interrupt test)
- When interrupt fails, the test never reaches collision → shows as MISSING
- Root cause: CSR0 IDON+INTR flag timing race. The Amiga writes INEA (0x4000) and polls for
  CSR0=0x01C1 (IDON+INIT+INEA+INTR). The daemon's `assert_mbx_int()` fires via DDR3 mailbox poll,
  but the 40s serial window expires if the interrupt takes too long to assert.

**Priority 2: Collision logic test (0% pass rate)**
- 0 PASS out of 33 runs where it actually executed
- The daemon's `do_transmit()` sends packets via `ethernet_send()` to eth1
- With the loopback plug, the packet should come back via `ethernet_recv()` → `gotfunc()`
- But the daemon doesn't set TX_ERR/TX_RTRY in the TX descriptor for collision detection
- The lance-test checks TMD3 bit 10 (TX_RTRY) — expects all 10 sends to show collision

**Priority 3: LANCE config (94% pass rate)**
- 6 FAIL — likely same root cause as interrupt (daemon startup race)
- When config fails, subsequent tests are also affected

## Fix Plan

1. **Interrupt test:** Investigate why CSR0 INTR flag is sometimes not set after INIT+INEA
2. **Collision logic:** Add TX_ERR/TX_RTRY handling in `do_transmit()` collision/loopback path
3. **LANCE config:** Fix daemon startup race (likely same fix as #1)
