# A2065 DDR3 Mailbox — Source Review

**Date:** 2026-06-04
**Files reviewed:**
- `Minimig-AGA_MiSTer/rtl/A2065/a2065_ddr3_mailbox.v`
- `Minimig-AGA_MiSTer/rtl/A2065/avalon_arbiter.v`

> **Note:** Source is **Verilog**, not VHDL. Request said "VHDL" — assuming the mailbox HDL was meant.

This is a static source review. Findings are ranked by likelihood of causing the
intermittent failures documented in `CLAUDE.md` (LANCE config ~10%, interrupt
~10–16%, collision/loopback).

---

## HIGH — Register path has no timeout (permanent hang)

`reg_timeout` is declared (`a2065_ddr3_mailbox.v:73`) but **never assigned or
read**. The register transaction states `S_REG_POLL` / `S_REG_POLL_W`
(`:160–200`) poll `MBX_REG_RSP` waiting for `avl_readdata[0]` (RSP ready). If the
ARM daemon never sets that bit — daemon dead, crashed, mid-restart, or it dropped
the request — the FSM **loops forever** with no escape.

Compare: the RAM path (`S_RAM_CAPTURE`/`S_RAM_WAIT`) and INT path
(`S_INT_CAPTURE`/`S_INT_WAIT`) both carry `timeout_cnt` and bail to `S_IDLE`. The
register path — the one the 68k actually stalls on via DTACK-stretch — has **none**.

**Impact:** matches the documented "register path dies / mailbox stuck" class of
failures. A single missed RSP wedges the bridge until core reload. With DTACK
stretched, the 68k hangs too.

**Fix:** wire `reg_timeout` as a 16-bit countdown in `S_REG_POLL`/`S_REG_POLL_W`.
On expiry, return a sentinel `bridge_result` (e.g. `0xFFFF`) and advance to
`S_REG_CLR_REQ`/`S_REG_DONE` so the 68k gets DTACK and unblocks. Consider also
driving `cpu_berr_n` (already noted unconnected in `CLAUDE.md`).

---

## MED — Dead states / signals from abandoned fixes

Three declared-but-unused artifacts. Each is a hole where a safety mechanism was
started and never finished:

| Artifact | Location | Status |
|----------|----------|--------|
| `wire req_edge` | `:64` | computed, never used (FSM uses `req_sync1` level) |
| `S_REG_POLL_DRAIN` | `:56`, `:202` | reachable only by no-op; just falls to `S_REG_POLL` |
| `reg reg_timeout` | `:73` | never assigned (see HIGH above) |

`S_REG_POLL_DRAIN` is telling — it looks like an abandoned attempt to drain a
stale `readdatavalid` before re-polling. The drain is exactly the missing
mechanism in finding **MED — stray readdatavalid** below.

**Fix:** delete `req_edge`. Either finish `S_REG_POLL_DRAIN` (route the
post-accept wait through it and consume one `readdatavalid`) or remove it.

---

## MED — Cross-domain data bus captured without explicit hold contract

This module runs in `clk_audio`. `bridge_new_req` is correctly 2-FF synchronized
(`req_sync0`/`req_sync1`, `:111–112`). But the **data** buses `bridge_data[15:0]`,
`bridge_addr_off[7:0]`, `bridge_rw` (from `clk_sys`) are sampled in
`S_REG_CAPTURE` (`:120–122`) with **no synchronizer**.

This is the standard MCP handshake pattern and is *safe only if* the `clk_sys`
register module holds those buses stable from before `bridge_new_req` asserts
until it sees `bridge_done`. If the register side ever deasserts/changes them
early, this samples metastable/torn data → wrong RAP/RDP address or data silently
sent to the daemon.

**Action:** the stable-hold requirement is an implicit contract not documented in
this module. Confirm `a2065_registers.v` holds `bridge_data`/`addr`/`rw` until
`bridge_done` returns, and add a comment here stating the dependency.

---

## MED — Interrupt sampled only in idle → known timing race

`a2065_int2` is updated only in `S_INT_WAIT` (`:340`), reached only from `S_IDLE`
when `&poll_div[4:0]` (`:130`). During any register or RAM transaction the FSM is
out of idle and **does not poll `MBX_INT`**. `poll_div` free-runs (`:114`) but is
ignored outside idle.

This is the documented ~10–16% interrupt race: during `chip_init()` boardram
traffic the adapter is busy in RAM states and misses the assert window while the
68k clears CSR0.

**Architectural fix:** make INT assert event-driven, not poll-gated. Either give
`MBX_INT` poll priority that can interrupt RAM idle slots more often, or latch the
interrupt in the daemon→FPGA path so a transient assert isn't lost between polls.
Poll-based sampling cannot close this race fully.

---

## MED — Stray `readdatavalid` has no global drain

Reads are issued one-at-a-time and guarded by `read_outstanding` (register path)
or `timeout_cnt` (RAM/INT), so the common case is safe. But the **timeout exits
do not account for an in-flight read**:

- `S_RAM_CAPTURE`/`S_RAM_WAIT` and `S_INT_CAPTURE`/`S_INT_WAIT` can bail to
  `S_IDLE` on `timeout_cnt==0` (`:237,251,323,337`) **after** a read was accepted
  by the slave but before its data returned.
- That orphaned `readdatavalid` arrives later in `S_IDLE` (ignored) or, worse, in
  the next transaction's poll state where it can be **misread as that state's
  result**.

Through the arbiter this is delivered per `rd_grant` (`:123,127`), so a late beat
routed to m1 lands in whatever poll state is then active.

**Impact:** rare data corruption — a stale RSP/RAM word consumed as the next
transaction's result. Plausible contributor to intermittent collision/loopback
descriptor mismatches.

**Fix:** track outstanding-read count; on any timeout, transition through a drain
state that swallows exactly the pending `readdatavalid` before returning to idle
(reuse `S_REG_POLL_DRAIN`).

---

## MED — Tight coupling to arbiter's "stuck grant" behaviour

Per `CLAUDE.md`, the v5 arbiter intentionally keeps `grant=m1` latched (the
`burst_active` quirk). The mailbox's read correctness **depends** on this:
`readdatavalid` is routed by `rd_grant` (`avalon_arbiter.v:97–99,123,127`), which
is updated on each accepted read. If grant ever flips to m0 while a mailbox read
is in flight, the returning beat is routed to m0 and lost (or m0's beat is
delivered to the mailbox).

The whole bridge thus rests on an undocumented-in-source arbiter side effect. Any
future arbiter timing change silently breaks the mailbox.

**Action:** add a prominent comment in *both* files noting the dependency, and the
`CLAUDE.md` "DO NOT modify the arbiter" rule. Long-term, make the mailbox robust
to grant changes (tag transactions) rather than relying on the quirk.

---

## LOW — Avalon read-deassert across the idle→capture boundary

In `S_IDLE` a poll read schedules `avl_read<=1` with the target address
(`:124–135`); the registered output appears the next cycle in `S_RAM_CAPTURE`/
`S_INT_CAPTURE`, where `!avl_waitrequest` is checked and the read held via re-assert
on wait. Because outputs are registered the protocol holds, but the pattern is
fragile: the address and `read` are set in one state and validated in another, so
any added cycle between them would drop the hold. Add a comment, or assert the
read and check `waitrequest` in the same state.

---

## LOW — Reset polarity differs between the two modules

`a2065_ddr3_mailbox` uses **async active-low** reset (`negedge rst_n`, `:81`).
`avalon_arbiter` uses **async active-high** reset (`posedge rst`, `:56`). Not a bug
in isolation, but the top-level must invert between them. Verify the wiring in
`sys_top.v`; a mismatch leaves one module un-reset at power-on.

---

## Summary

| # | Severity | Issue | Likely symptom |
|---|----------|-------|----------------|
| 1 | **HIGH** | Register path has no timeout (`reg_timeout` unused) | Permanent bridge hang; 68k DTACK stall |
| 2 | MED | Dead states/signals (`req_edge`, `S_REG_POLL_DRAIN`, `reg_timeout`) | Abandoned safety nets; missing drain |
| 3 | MED | CDC data bus captured on implicit stable-hold contract | Torn RAP/RDP addr or data |
| 4 | MED | INT sampled only in idle | ~10–16% interrupt race |
| 5 | MED | No drain for stray `readdatavalid` after timeout | Rare wrong-result corruption |
| 6 | MED | Correctness depends on arbiter stuck-grant quirk | Silent break on any arbiter change |
| 7 | LOW | Read assert/validate split across states | Fragile, breaks if a cycle added |
| 8 | LOW | Reset polarity mismatch between modules | Un-reset module at power-on if mis-wired |

**Top priority:** #1 — finishing `reg_timeout` directly addresses the documented
register-path hang and would convert a wedge-until-reload failure into a recoverable
one. #4 and #5 are the most plausible sources of the remaining intermittent
interrupt and collision/loopback failures.

---

## Proposed Code Changes

All snippets target `a2065_ddr3_mailbox.v`. Line numbers refer to the reviewed
revision. Snippets are illustrative — apply, then re-sim `tb_bridge_e2e` and
rebuild before deploy.

> Verification gate per `CLAUDE.md`: timing slack on the emu PLL is only **+0.003ns**.
> Any added logic risks a setup violation. Re-run the Quartus timing report and
> confirm positive slack before flashing.

### Fix 1 (HIGH) — Register-path watchdog timeout

Use the already-declared `reg_timeout`. Arm it on capture, count down while
polling, and on expiry return a sentinel and unblock the 68k via the normal clear
path.

**`S_REG_CAPTURE` (`:139`) — arm the watchdog:**
```verilog
            S_REG_CAPTURE: begin
                read_outstanding <= 0;
                reg_timeout    <= 16'hFFFF;          // ADD: ~1.3ms @ 49MHz
                avl_address    <= DDR3_BASE + MBX_REG_REQ;
                avl_writedata  <= {14'b0, saved_data,
                                   saved_addr,
                                   saved_rw, 1'b1};
                avl_byteenable <= 8'hFF;
                avl_burstcount <= 1;
                avl_write      <= 1;
                state          <= S_REG_WR_REQ;
            end
```

**`S_REG_POLL` (`:160`) — count down, bail on zero:**
```verilog
            S_REG_POLL: begin
                if (reg_timeout == 0) begin          // ADD: watchdog expired
                    bridge_result  <= 16'hFFFF;      // sentinel (also see berr note)
                    avl_address    <= DDR3_BASE + MBX_REG_REQ;
                    avl_writedata  <= 64'b0;          // clear stale REQ for daemon
                    avl_byteenable <= 8'hFF;
                    avl_burstcount <= 1;
                    avl_write      <= 1;
                    state          <= S_REG_CLR_REQ;  // -> bridge_done -> DTACK
                end else if (!read_outstanding) begin
                    reg_timeout      <= reg_timeout - 1'b1;   // ADD
                    avl_address      <= DDR3_BASE + MBX_REG_RSP;
                    avl_burstcount   <= 1;
                    avl_read         <= 1;
                    read_outstanding <= 1'b1;
                    state            <= S_REG_POLL_W;
                end else if (avl_readdatavalid) begin
                    read_outstanding <= 1'b0;
                    if (avl_readdata[0]) begin
                        bridge_result  <= avl_readdata[16:1];
                        avl_address    <= DDR3_BASE + MBX_REG_RSP;
                        avl_writedata  <= 64'b0;
                        avl_byteenable <= 8'hFF;
                        avl_burstcount <= 1;
                        avl_write      <= 1;
                        state          <= S_REG_CLR_RSP;
                    end
                end else begin
                    reg_timeout <= reg_timeout - 1'b1;        // ADD: count while waiting
                end
            end
```

**`S_REG_POLL_W` (`:181`) — count down on the wait branch:**
```verilog
            S_REG_POLL_W: begin
                if (avl_readdatavalid) begin
                    read_outstanding <= 1'b0;
                    if (avl_readdata[0]) begin
                        bridge_result  <= avl_readdata[16:1];
                        avl_address    <= DDR3_BASE + MBX_REG_RSP;
                        avl_writedata  <= 64'b0;
                        avl_byteenable <= 8'hFF;
                        avl_burstcount <= 1;
                        avl_write      <= 1;
                        state          <= S_REG_CLR_RSP;
                    end else begin
                        state <= S_REG_POLL;
                    end
                end else if (!avl_waitrequest) begin
                    state <= S_REG_POLL;
                end else begin
                    reg_timeout <= reg_timeout - 1'b1;        // ADD
                    avl_read    <= 1;
                end
            end
```

**Note:** `bridge_result = 0xFFFF` is the watchdog sentinel the 68k reads on a
dead daemon. Long-term, also assert `cpu_berr_n` (currently unconnected per
`CLAUDE.md`) so a watchdog timeout raises a bus error rather than returning
`0xFFFF` as if it were real data.

### Fix 2 (MED) — Remove dead code

```verilog
// DELETE line 64 — never referenced (FSM uses req_sync1 level):
wire req_edge = req_sync1 && !req_prev;
```
`S_REG_POLL_DRAIN` should be either removed or repurposed for Fix 5 below. If
Fix 5 is adopted, keep the localparam and give it a body; otherwise delete the
localparam (`:56`) and its empty case arm (`:202–204`).

### Fix 5 (MED) — Drain stray `readdatavalid` on timeout

Add a small bounded drain so a read accepted-but-not-yet-returned cannot leak its
data beat into the next transaction.

**New register (near `:74`):**
```verilog
    reg [3:0] drain_cnt;
```
Reset it with the others in the `!rst_n` block:
```verilog
            drain_cnt <= 0;
```

**Route the RAM/INT timeout exits through a drain state instead of straight to
`S_IDLE`.** Example for `S_RAM_WAIT` (`:250`) and `S_RAM_CAPTURE` (`:236`) —
replace `state <= S_IDLE;` on the `timeout_cnt == 0` branches with:
```verilog
                    drain_cnt <= 4'd8;
                    state     <= S_REG_POLL_DRAIN;   // reused as generic drain
```
Give the drain state a real body (replace `:202–204`):
```verilog
            S_REG_POLL_DRAIN: begin
                // swallow at most one stray readdatavalid, bounded wait
                if (avl_readdatavalid || drain_cnt == 0)
                    state <= S_IDLE;
                else
                    drain_cnt <= drain_cnt - 1'b1;
            end
```
Apply the same redirect to the `S_INT_CAPTURE`/`S_INT_WAIT` timeout exits
(`:323,337`).

### Fix 4 (MED) — Give the interrupt poll priority in idle

Poll-based sampling cannot fully close the race (the real fix is daemon-side
latching), but raising INT priority shrinks the window. Currently RAM wins when
both are due because `&poll_div` is tested first (`:124`). Reorder so a due INT
poll is serviced first, and poll it more often via a shorter mask:

```verilog
            S_IDLE: begin
                bridge_done <= 0;
                if (req_sync1) begin
                    saved_data <= bridge_data;
                    saved_addr <= bridge_addr_off;
                    saved_rw   <= bridge_rw;
                    state      <= S_REG_CAPTURE;
                end else if (&poll_div[3:0]) begin   // INT first, every 16 cycles
                    timeout_cnt    <= 8'd255;
                    avl_address    <= DDR3_BASE + MBX_INT;
                    avl_burstcount <= 1;
                    avl_read       <= 1;
                    state          <= S_INT_CAPTURE;
                end else if (&poll_div) begin          // RAM every 256 cycles
                    timeout_cnt    <= 8'd255;
                    avl_address    <= DDR3_BASE + MBX_RAM_REQ;
                    avl_burstcount <= 1;
                    avl_read       <= 1;
                    state          <= S_RAM_CAPTURE;
                end
            end
```
Trade-off: more DDR3 INT polls raise arbiter traffic. Keep RAM at `&poll_div`
(256) to stay within the throttle budget that `CLAUDE.md` records as DDR3-stable.
Validate with the boardram-timeout count after the change.

### Fix 3 (MED) — Document the CDC stable-hold contract

No logic change; add a comment at the capture point (`:139`) and verify the
producer side:
```verilog
            // CDC: bridge_data/addr/rw cross clk_sys->clk_audio WITHOUT a
            // synchronizer. Safe ONLY because a2065_registers.v holds them
            // stable from before bridge_new_req asserts until bridge_done
            // returns. Do not change that hold without adding a sync here.
            S_REG_CAPTURE: begin
```

### Fix 6 (MED) — Document the arbiter dependency

Add to both files:
```verilog
    // NOTE: mailbox read correctness depends on avalon_arbiter keeping grant=m1
    // latched (the burst_active quirk). readdatavalid is routed by rd_grant; if
    // grant ever flips mid-read the returning beat is lost/misrouted.
    // See CLAUDE.md "DO NOT modify the arbiter".
```

### Fix 7 (LOW) — Comment the split read assert/validate

At the idle poll reads (`:124–135`), note that `avl_read`/`avl_address` are set
one state early and validated in `S_*_CAPTURE`; adding a cycle between would drop
the Avalon hold.

### Fix 8 (LOW) — Verify reset wiring

No change here; confirm in `sys_top.v` that the active-high arbiter `rst` is the
inversion of the mailbox active-low `rst_n` (both fed from the same reset source).
Add an assertion or comment at the instantiation.

### Suggested apply order

1. Fix 2 (delete `req_edge`) — trivial, no risk.
2. Fix 1 (register watchdog) — highest payoff; re-sim `tb_bridge_e2e`.
3. Fix 5 (drain) — depends on whether `S_REG_POLL_DRAIN` is kept.
4. Fix 4 (INT priority) — re-check boardram-timeout count and DDR3 stability.
5. Fixes 3/6/7/8 — comments + wiring verification, batch into one commit.

Re-run the full Quartus timing report after Fixes 1/4/5 (added logic vs +0.003ns
slack) before any MiSTer flash.
