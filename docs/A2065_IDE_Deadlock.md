# Bug summary: Main_MiSTer A2065 / IDE deadlock

**Component:** Main_MiSTer, Minimig support (`ide.cpp`, `support/minimig/minimig_a2065.cpp`)
**Severity:** hard hang of the emulated Amiga; recovery needs a core reload, and with a deadlocked Main only a MiSTer reboot works
**Status:** fixed, verified on hardware, and **merged upstream on 2026-10-14** by Alexey Melnikov (sorgelig):
- [MiSTer-devel/Main_MiSTer#1357](https://github.com/MiSTer-devel/Main_MiSTer/pull/1357), merged into `master` as `00bdff0d`
- [MiSTer-devel/Minimig-AGA_MiSTer#256](https://github.com/MiSTer-devel/Minimig-AGA_MiSTer/pull/256), the companion RTL change, merged into `MiSTer` as `d2413386`

## Symptom

With the A2065 Ethernet card active and Roadshow running, heavy IDE writes hard-hang
the Amiga. The first report was a FileMaster copy of an extracted archive
(ChoRenSha) from `RAM:` to DH0, on 2026-10-09. The screen freezes, the serial CLI
stops answering and nothing recovers on its own.

A dump of the A2065 DDR3 mailbox on the hung machine showed:

- CSR0 stuck at `04F3`: INTR, RINT and INEA set
- the CMD ring write index climbing by tens of thousands a second, with every entry
  a write of `0x0440` (RINT | INEA) to CSR0

## Root cause

Main is single-threaded. Two pieces of it each waited on the Amiga, and the Amiga
was waiting on one of them.

1. **IDE PIO handshake.** `process_read()` and `process_write()` in `ide.cpp`
   prepare a block and then spin on the core until the Amiga takes or supplies it:

   ```c
   ide_req = 0;
   while (!ide_req) ide_req = (ide_check() >> ide->bitoff) & 7;
   ```

   Nothing else in Main runs during this loop, `a2065_poll()` included.

2. **A2065 register writes.** The LANCE emulation lives in Main. A 68k write to a
   LANCE register is posted by the FPGA into a DDR3 CMD ring. Main applies it only
   when `a2065_poll()` drains the ring, and only then republishes the CSR shadow
   that the 68k reads back.

3. **The driver's interrupt server.** `a2065.device`'s level 2 server acknowledges
   CSR0 by writing the W1C interrupt bits back, then loops until they read clear.

When a receive interrupt arrives during an IDE transfer, the Amiga enters its level
2 server and writes `0x0440` to CSR0. That write sits in the ring, because Main is
in the IDE loop. CSR0 still reads `04F3`, so the server writes again, and keeps
doing so (the ring traffic seen in the dump). The CPU never leaves interrupt level
to move the next IDE block, and Main never leaves the IDE loop to drain the ring.
Neither side can proceed.

Nothing in the AP030 core or the FPGA mailbox was wrong as such; the 68030 behaved
correctly. The hang needs both IDE PIO traffic and A2065 interrupts at the same
moment, which is why ordinary use and plain network tests did not show it.

## Fix

**Main_MiSTer** (PR #1357, merged):

- `ide_wait_req()` replaces the bare loop and calls `a2065_poll()` while waiting
  (Adam Polkosnik, `apolkosnik/Main_MiSTer@18fa4fb`). It polls on every 16th miss
  only, because `a2065_poll()` costs an uncached DDR3 read plus a `recv()` syscall
  and would otherwise add latency to every IDE block. This breaks the deadlock
  within microseconds.
- `INT_STATE` also publishes Main's CMD ring read index (bits 63:32, valid with
  bit 1). It is written after the CSR shadow, with `__sync_synchronize()` between
  the two stores. This lets the core apply acknowledges itself. Cores that only
  read bit 0 are unaffected.

**Minimig-AGA_MiSTer RTL** (PR #256, merged; companion and optional):

- The DDR3 mailbox applies a CSR0 acknowledge to its own shadow at once and masks
  the cleared bits until Main's published read index has passed the command
  (Adam Polkosnik, `4409836`). The Amiga therefore leaves its interrupt server even
  if Main is busy anywhere, not just in IDE.
- **Race found in that change and fixed** (`478db52`). The mailbox polled CSR first
  and INT second, while Main writes CSR first and INT second. A ring drain landing
  between the two reads unmasked a stale CSR0 with RINT still set, and a second
  acknowledge could then clear a genuinely new RINT. The fix reads INT_STATE first.
  A testbench case reproduces the race: it fails on the old order and passes on
  the new one.

Either half alone breaks this deadlock. They are compatible in any combination of
old and new Main and core.

## Verification

Hardware: DE10-Nano, AP030 Minimig core (AP68030 d8dda39 + mailbox fixes),
Ethernet on eth0. A soak test loops RAM: → DH0: → RAM: copies of
34 MB (194 files) with Roadshow up, while a UDP flood (1500 pps, 20 s on / 5 s
off) keeps receive interrupts coming. It watches a serial-CLI heartbeat and the
DDR3 mailbox from Linux.

| Main binary | Result |
|---|---|
| without fix (`1f3c50d2`) | **Hang** about 1 s into the first DH0 copy: CSR0 `04F3`, ring of `0x0440` writes at ~30 000/s, no heartbeat for 46 s |
| with fix (`206f67f1`) | 33 iterations over 24 min with no heartbeat miss; then 5 iterations, read-back md5 194/194 |

Also clean with the fix: `addnetinterface` + DHCP, ping 10/10, 100 MB wget to disk
at 381 KB/s, AmiSpeedTest 4.5 / 4.2 Mbit/s. Simulation: `tb_cmd_ring` 22/22,
`tb_boardram_cdc` 3/3, `tb_ddram_arbiter` 14/14.

## Notes

- A Main stuck in this loop also ignores `/dev/MiSTer_cmd`, so loading another core
  from Linux does not work. It has to be killed and the MiSTer rebooted, because
  Main is started from inittab `sysinit`, not `respawn`.
- Both fixes are now in the official trees, so the next official Main_MiSTer
  build and Minimig-AGA_MiSTer release carry them. Older Main binaries remain
  exposed to the hang whenever A2065 and IDE traffic overlap.
