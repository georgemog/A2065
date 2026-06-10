# Internal Loopback Fix — Design Document

## Problem

The ARM daemon does not implement Am7990 internal loopback mode (`MODE_LOOP`). Two bugs
prevent the Internal Loopback Test (lance-test diag 5, mode=0x04) from passing:

1. **TX self-addressed frames are treated as collisions.** `do_transmit()` detects
   DST MAC == own MAC and sets `coll=1`, causing TX_ERR+TX_RTRY and silently dropping
   the packet (lines 137-141 of `rings.cpp`).

2. **RX drops self-loopback frames.** `gotfunc()` rejects any frame where
   SRC==DST==own MAC (lines 202-203), which is exactly what internal loopback produces.

Result: the Internal Loopback Test always FAILS. The daemon never loops TX frames back
to the RX ring.

## Am7990 Loopback Behavior (Reference)

When `MODE_LOOP` (bit 2) is set in the init block mode word:

- TX data is routed internally to the RX path — nothing goes to the wire
- CRC is generated on TX and checked on RX (unless `MODE_DTCR` is set)
- Address filtering and descriptor ring protocol work normally

Mode combinations used by lance-test:

| Test | Mode | Bits | Behavior |
|------|------|------|----------|
| LANCE config | 0x44 | LOOP+DTCR | Loopback, CRC generated, 100 send/recv |
| Collision | 0x54 | LOOP+COLL+DTCR | Collision forced, TX_ERR+TX_RTRY |
| Internal loopback | 0x04 | LOOP | Loopback, CRC generated, 200 send/recv |

Key: `MODE_COLL` (0x0010) is a **separate bit** from `MODE_LOOP` (0x0004). Loopback
without collision should deliver frames normally. Loopback with collision should corrupt
them (TX_ERR+TX_RTRY).

## Test Expectations

### Internal Loopback Test (mode=0x04)

1. Init with `lance_init(board_id, mode=0x04)`
2. Build 24-byte frame: sequential bytes 0x00-0x17, DST=own MAC
3. Loop 200 times:
   - `lance_send_pkt(board_id, buf, 24, flag=1)`
   - `lance_recv_pkt(board_id, recv_buf, 256)`
   - Verify received length == 28 (24 data + 4 FCS)
4. Return 0/1=PASS, 2=FAIL

The test sends to its own MAC, so DST MAC == SRC MAC == board MAC. The frame must
traverse TX → internal loopback → RX successfully.

### LANCE Config Test (mode=0x44)

Same pattern but 100 iterations. `MODE_DTCR` is set — on real hardware this disables
TX CRC generation. However, the test still expects 28 bytes received (24+4 FCS),
suggesting the Am7990 generates CRC in loopback regardless of DTCR. This needs
verification during testing.

## Design

### Core Idea

When `MODE_LOOP` is set (without `MODE_COLL`), `do_transmit()` calls `gotfunc()`
directly with the TX frame instead of sending to the ethernet socket. `gotfunc()`
appends CRC and writes to the RX ring — identical to receiving a real frame.

### Change 1: `do_transmit()` loopback path (`rings.cpp`)

**Current flow** (simplified):
```
build frame → check self-MAC collision → if coll: error → else: ethernet_send()
```

**New flow**:
```
build frame → check mode:
  MODE_LOOP + MODE_COLL: set TX_ERR+TX_RTRY (collision)
  MODE_LOOP (no COLL):   gotfunc(frame) (loopback)
  no LOOP:               check self-MAC → ethernet_send()
```

**Pseudocode** (insert after frame build, before existing collision check):

```c
uint16_t mode = registers_mode();

if (mode & MODE_LOOP) {
    if (mode & MODE_COLL) {
        // Collision forced — set error flags, don't send or loopback
        tmd1 |= TX_ERR;
        tmd3 |= TX_RTRY;
    } else {
        // Internal loopback — deliver to RX ring
        gotfunc(transmitbuffer, outsize);
    }
} else {
    // External path — existing behavior
    if (outsize >= 6) {
        uint8_t fakemac_buf[6];
        registers_get_fakemac(fakemac_buf);
        if (memcmp(transmitbuffer, fakemac_buf, 6) == 0) {
            // Self-loopback collision (non-LOOP mode, self-addressed)
            tmd1 |= TX_ERR;
            tmd3 |= TX_RTRY;
        }
    }
    if (!(tmd1 & TX_ERR)) {
        if ((mode & MODE_DTCR) && !add_fcs)
            outsize -= 4;
        transmitlen = outsize;
        mungepacket(transmitbuffer, transmitlen);
        ethernet_send(transmitbuffer, transmitlen);
    }
}
```

**Key points:**
- For `MODE_LOOP` without `MODE_COLL`: call `gotfunc(transmitbuffer, outsize)` directly.
  `gotfunc()` appends CRC (+4 bytes) and writes to the RX ring. No ethernet socket involved.
- For `MODE_LOOP` + `MODE_COLL`: collision behavior unchanged (TX_ERR+TX_RTRY).
- For no `MODE_LOOP`: existing self-loopback collision detection preserved for
  the collision test case.
- `mungepacket()` is NOT called for loopback frames — the frame should pass through
  unmodified since both TX and RX are internal. MAC address translation is only needed
  when frames cross to/from the real ethernet.

### Change 2: `gotfunc()` self-MAC drop (`rings.cpp`)

**Current** (line 202-203):
```c
if (memcmp(dstmac, fakemac_buf, 6) == 0 &&
    memcmp(srcmac, fakemac_buf, 6) == 0) return;
```

**New**:
```c
if (!(registers_mode() & MODE_LOOP) &&
    memcmp(dstmac, fakemac_buf, 6) == 0 &&
    memcmp(srcmac, fakemac_buf, 6) == 0) return;
```

When `MODE_LOOP` is set, self-addressed frames from the internal loopback are accepted.
When not in loopback, the existing self-echo drop is preserved.

**Thread safety:** `gotfunc()` is called from two contexts:
1. Main thread: `do_transmit()` → `gotfunc()` (loopback)
2. RX thread: `rx_thread()` → `ethernet_recv()` → `gotfunc()` (real network)

The `registers_mode()` value is a uint16_t set during `chip_init()` and never modified
during runtime. Reading it is atomic on ARM. No mutex needed.

### DTCR and CRC Handling

The CRC path in loopback:

| Mode | TX frame size | gotfunc appends CRC | RX frame size |
|------|--------------|---------------------|---------------|
| 0x04 (LOOP) | 24 | +4 | 28 |
| 0x44 (LOOP+DTCR) | 24 | +4 | 28 |

`do_transmit()` currently subtracts 4 from outsize when DTCR is set (line 153-154).
This is only done in the external path (`ethernet_send`). For loopback, we use `outsize`
directly — no subtraction. `gotfunc()` always appends CRC, giving the correct 28 bytes
for both cases.

**Verification needed:** If the config test expects 24 bytes (not 28) when DTCR is set,
we may need to skip CRC in `gotfunc()` for DTCR loopback. This can be tested by running
the config test and checking the received length.

### mungepacket() for Loopback

`mungepacket()` swaps fakemac↔realmac in packet headers. For internal loopback, both
TX and RX are within the emulated LANCE — no real ethernet involved. MAC translation
should NOT be applied.

In the new design, `mungepacket()` is only called in the external (non-loopback) path.
`gotfunc()` still calls `mungepacket()` internally (line 212), which would incorrectly
translate MACs for loopback frames.

**Fix:** Check `MODE_LOOP` in `gotfunc()` and skip `mungepacket()` when loopback is
active:

```c
if (!(registers_mode() & MODE_LOOP))
    mungepacket(d, len);
```

## Summary of Changes

| File | Function | Change |
|------|----------|--------|
| `rings.cpp` | `do_transmit()` | Add MODE_LOOP branch: loopback → gotfunc(), coll → error, else → ethernet |
| `rings.cpp` | `gotfunc()` | Skip self-MAC drop when MODE_LOOP set |
| `rings.cpp` | `gotfunc()` | Skip mungepacket() when MODE_LOOP set |

No FPGA changes required. No new files. No new threading concerns.

## Testing

1. Cross-compile, deploy to MiSTer
2. Run `lance-test diags` — Internal Loopback Test should PASS
3. Run 100x lance-test — verify no regression in other tests (Buffer, Config, Interrupt, Collision)
4. Verify LANCE Config Test still PASS (CRC handling with DTCR)

## Risk Assessment

- **Low risk:** Changes are confined to the loopback path. External TX/RX is unchanged.
- **Threading:** `gotfunc()` from loopback runs on main thread, same as existing
  `service_bridge_safe()` → `gotfunc()` path. RX thread is blocked on `ethernet_recv()`
  and won't interfere.
- **Regression:** The self-MAC collision check (for collision test) is preserved in the
  non-LOOP path. Collision test (MODE_COLL) behavior unchanged.
