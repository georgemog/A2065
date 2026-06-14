# A2065 Ethernet Emulation for Minimig MiSTer

Full hardware emulation of the Commodore A2065 Zorro II Ethernet card on the
Minimig FPGA core (DE10-Nano / Cyclone V). Runs stock AmigaOS A2065 drivers —
no custom Amiga-side software.

> **Status:** Steps 0–11 Complete · Branch `simplification/flat-ddr3-doorbell` · Updated 2026-06-13
>
> **Latest build `Minimig_20260613a.rbf` — merged onto upstream Minimig Release 20260603 (`eb7a26e`) and hardware-tested: lance-test 5/5 PASS + DHCP + ping.** See [`releases/`](releases/) and [`docs/A2065_Minimig_Merge_Plan.md`](docs/A2065_Minimig_Merge_Plan.md) §11.

A formatted HTML version of this overview is at [`docs/A2065_Project_Article.html`](docs/A2065_Project_Article.html);
the Markdown source is [`docs/A2065_Project_Article.md`](docs/A2065_Project_Article.md).

---

## 00 · Updates

### 2026-06-13 — `Minimig_20260613a.rbf` (upstream-merged release)

**What it is:** the A2065 doorbell core rebased onto the latest upstream Minimig —
**`MiSTer-devel/Minimig-AGA_MiSTer` Release 20260603 (`eb7a26e`)**. Previous good builds
(e.g. `20260609a`) were based on the older Release 20260220; this one brings the A2065 work
up to current upstream so it tracks the mainline core.

**What changed vs the previous good build (`20260609a`):**

- **New baseline.** Merged onto upstream `eb7a26e` instead of `3ab91cd`. Picks up upstream's
  "Update sys." framework refactor (the `emu` port list moved into `sys/emu_ports.vh`, plus
  audio/scaler/HDMI plumbing). No A2065 logic changed — the networking RTL is identical to
  `20260609a`; only the surrounding core is newer.
- **Cleaner integration.** The vestigial HPS→FPGA AXI (`h2f`) path was dropped (`sys/sysmem.sv`
  reverted to upstream, byte-identical); dead/unused RTL moved to `rtl/A2065/legacy/`;
  the A2065 block in `sys/sys_top.v` is now guard-commented for easy re-apply after future
  upstream `Update sys.` drops.
- **Same behaviour.** The doorbell signal paths, DDR3 mailbox, and daemon protocol are unchanged.

**Verification (hardware, DE10-Nano):**

- lance-test diagnostics **5/5 PASS** — Buffer / Config / Interrupt / Collision / Loopback.
- `AddNetInterface a2065` → DHCP lease `192.168.1.190`, ping LAN 7/7 + internet (`8.8.8.8`) 13/13, 0% loss.
- Quartus 17.0 clean: 0 errors, setup slack **+0.377 ns**, hold **+0.246 ns**.
- Full parity with the `20260609a` baseline.

Merge details and the full risk register are in
[`docs/A2065_Minimig_Merge_Plan.md`](docs/A2065_Minimig_Merge_Plan.md) §11.
The RBF ships in [`releases/Minimig_20260613a.rbf`](releases/Minimig_20260613a.rbf).

---

## 01 · What It Is

The A2065 was Commodore's Zorro II Ethernet card built around the AMD Am7990 LANCE
controller. This project re-creates that card entirely in the MiSTer Minimig core —
FPGA fabric plus an ARM Linux daemon — so a virtual Amiga can talk to a real network
through stock AmigaOS A2065 drivers. It is a port of Amiberry's `a2065.cpp`
(Toni Wilen, 2009) from emulator-space onto real hardware.

| | |
|---|---|
| **Target chip** | AMD Am7990 LANCE — CSR register file, TX/RX descriptor rings, 32 KB boardram |
| **Card identity** | Zorro II, 64 KB space, manufacturer ID `0x0202`, product `0x70`, MAC OUI `00:80:10` |
| **No Amiga software** | Uses the standard AmigaOS A2065 SANA-II driver — looks like a real card to Workbench & TCP/IP stacks |

---

## 02 · Full Architecture

Work splits across two compute domains living on one DE10-Nano SoC plus a USB NIC:

- **FPGA fabric (Cyclone V)** — presents the card to the Amiga 68020. Zorro II
  autoconfig, flat DDR3 boardram window, CSR regfile + doorbell (zero-latency reads,
  async writes to ARM), INT2 interrupt generation. Clock domains `clk_sys` (68k) and
  `clk_audio` (mailbox/DDR3), bridged by CDC synchronizers.
- **ARM Linux daemon (Cortex-A9 HPS)** — runs the LANCE state machine and talks to the
  wire. Am7990 CSR state machine, TX/RX descriptor ring walker, raw Ethernet socket
  (`AF_PACKET`), boardram via DDR3 flat window, interrupt state via CSR shadow, MAC
  translation in both directions.
- **DDR3 shared-memory bridge** — ties the two domains together over the `f2sdram2`
  Avalon port (already used for audio/PAL), round-robin arbitrated. The HPS2FPGA AXI
  bridge was abandoned — non-functional on MiSTer.
- **USB Ethernet adapter in promiscuous mode** — the physical wire.

### End-to-End Stack

```
┌─────────────────────────────────────────────────────────────────────┐
│  AmigaOS  ·  stock A2065 SANA-II driver  ·  TCP/IP stack (Roadshow)  │  software
├─────────────────────────────────────────────────────────────────────┤
│  Motorola 68020  ·  Zorro II bus cycles to card window $EAxxxx       │  emulated CPU
╞═════════════════════════════════════════════════════════════════════╡  ── FPGA (Cyclone V) ──
│  a2065_autoconfig   ZorroII ROM → base address latched               │  clk_sys
│  a2065_regfile      CSR reads (combinational from shadow) + doorbell  │
│  a2065_ddram        32KB boardram window, AS+DS-qualified capture     │
│  CDC 2-stage  ←→  a2065_ddr3_mailbox FSM  ·  avalon_arbiter           │  clk_audio
╞═════════════════════════════════════════════════════════════════════╡  ── DDR3 shared memory ──
│  f2sdram2 Avalon port  @ 0x1FF00000 : boardram | CMD | CSR | INT | MAC │
╞═════════════════════════════════════════════════════════════════════╡  ── ARM Linux (HPS) ──
│  a2065d_doorbell   poll CMD slot → chip_wput() → push CSR shadow/INT  │  main thread
│  registers.cpp     Am7990 CSR0/1/2/3 state machine, INIT block parse  │
│  rings.cpp         TX/RX descriptor ring walker (TDRA / RDRA)         │
│  mac.cpp           mungepacket(): fakemac 00:80:10:.. ↔ host realmac  │
│  ethernet.cpp      AF_PACKET SOCK_RAW  ·  send() / recv()             │  RX thread (gotfunc)
╞═════════════════════════════════════════════════════════════════════╡
│  USB Ethernet adapter (eth1)  ·  PROMISCUOUS mode  ·  IFF_UP forced   │  physical NIC
├─────────────────────────────────────────────────────────────────────┤
│  Physical LAN  ·  switch  ·  DHCP server, peers                       │  the wire
└─────────────────────────────────────────────────────────────────────┘
```

### Why DDR3 instead of HPS2FPGA

The HPS2FPGA lightweight bridge proved non-functional on MiSTer for this use case.
The DDR3 approach reuses the existing `f2sdram2` Avalon port, shared with the audio/PAL
service via a round-robin arbiter. Verified working since build 20260507.

### Two Bridge Generations

| Architecture | FPGA modules | Daemon | Mechanism |
|---|---|---|---|
| **Old bridge** (`main`) | `a2065_registers.v`, `a2065_boardram.v` (TDP BRAM) | `a2065d_ddr3` | DTACK-stretch + DDR3 mailbox CMD/RSP protocol |
| **Doorbell** (`flat-ddr3-doorbell`) | `a2065_regfile.v`, `a2065_ddram.v` (flat DDR3) | `a2065d_doorbell` | Zero-latency CSR reads; RDP writes raise a doorbell ARM polls async |

---

## 02b · Network Path & Promiscuous Mode

The Amiga side believes it owns a real A2065 with its own MAC. The wire side is a generic
USB Ethernet adapter whose MAC differs. Two mechanisms reconcile them: **promiscuous mode**
on the NIC, and **MAC translation** in the daemon.

**Raw socket on the USB NIC.** `ethernet.cpp` opens
`socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))` bound to `eth1` via `sockaddr_ll`. This
bypasses the host kernel's IP stack entirely — the daemon sees and sends complete Ethernet
frames. The interface is forced `IFF_UP` at open since the USB NIC is often admin-down after boot.

**Promiscuous mode — why it's required.** The Amiga's card MAC (`00:80:10:XX:XX:XX`) is *not*
the USB adapter's hardware MAC. By default the NIC drops frames not addressed to its own MAC,
so replies to the Amiga would never arrive. Setting `PACKET_MR_PROMISC` via
`setsockopt(... PACKET_ADD_MEMBERSHIP ...)` tells the adapter to deliver **every** frame on
the wire to the socket, letting the daemon catch the Amiga's traffic regardless of destination MAC.

**MAC translation (`mungepacket`).** Promiscuous capture is only half the trick — the addresses
on the wire must still be coherent. `mac.cpp` maintains two MACs and rewrites every frame in both
directions:

- **fakemac** `00:80:10:XX:XX:XX` — Commodore OUI, the MAC the AmigaOS driver thinks it has.
- **realmac** — the host USB-NIC MAC with its OUI forced to `00:80:10`.
- **TX path:** frames leaving the Amiga carry `fakemac` as source → daemon swaps it to `realmac`
  before `send()`, so the LAN/switch sees a consistent, valid source.
- **RX path:** frames arriving for `realmac` are swapped back to `fakemac` before being written into
  the RX ring, so the Amiga recognises them as its own.
- **Deep rewrite:** `mungepacket()` also fixes ARP sender/target hardware addresses and DHCP `CHADDR`,
  recomputing the UDP checksum when `CHADDR` changes — without this, ARP and DHCP would advertise the
  wrong MAC and the lease would fail.

> ⚠️ **Non-standard kernel required.** The stock MiSTer Linux kernel ships no USB Ethernet drivers —
> it has only the on-board NIC. This project runs a **locally-built MiSTer kernel (Linux 5.15.1) with the
> USB NIC drivers compiled statically into the image** (CDC-ECM / ASIX / RTL8152 etc.), which is what
> enumerates the USB adapter as `eth1`. The drivers are built in (not loadable modules), so the kernel is
> self-contained — no rootfs `.ko` files needed. The image is included at [`kernel/zImage_dtb`](kernel/zImage_dtb).
> Without that kernel there is no wire-side interface for the daemon to bind its raw socket to, and the
> whole network path is dead. The custom kernel is a hard prerequisite, not an optional convenience.

> ✅ **Result.** With the USB-capable kernel + promiscuous capture + bidirectional MAC munging, the
> virtual Amiga obtains a real DHCP lease and exchanges live traffic over the physical LAN — verified
> end-to-end via the External Loopback test (frame out `eth1`, back through `gotfunc` into the RX ring).

---

## 03 · Doorbell Signal Paths

The current (doorbell) architecture. CSR reads are combinational from a shadow the ARM keeps fresh;
register writes ring a doorbell; boardram rides a flat DDR3 window; interrupts flow ARM→FPGA→Paula.

```
Register read (zero-latency):
  68020 → a2065_regfile (combinational from csr_shadow) → data bus OR-tie
  ARM updates shadow → DDR3 CSR slot → mailbox FSM poll → regfile

Register write (doorbell):
  68020 → a2065_regfile → cmd_pending/rap/data (level) → mailbox FSM (CDC 2-stage)
        → DDR3 CMD slot → ARM polls → chip_wput() → push_csr_shadow() → DDR3 CSR slot

Boardram:
  68020 → a2065_ddram → bram_req_valid (level) → mailbox FSM (CDC 2-stage)
        → DDR3 read/write → ddram (CDC 2-stage) → cpu_data_out

Interrupt:
  ARM daemon → push_csr_shadow + update_int_state → DDR3 INT slot
        → mailbox FSM poll → a2065_int2 → CDC 2-stage → Paula int2 → 68020 level 2
```

### DDR3 Layout (offsets from `DDR3_FLAT_BASE = 0x1FF00000`)

| Offset | Size | Purpose |
|---|---|---|
| `0x0000` | 32 KB | Flat boardram window (4×16-bit per 64-bit DDR3 word) |
| `0x8000` | 8 B | CMD slot: `{data[15:0], rap[6:0], pending[0]}` |
| `0x8010` | 8 B | CSR shadow: `{csr3, csr2, csr1, csr0}` |
| `0x8018` | 8 B | INT state: `{int_assert[0]}` |
| `0x8028` | 8 B | MAC: `{mac[47:0], valid[0]}` |

---

## 04 · Implementation Status

Eleven-step plan. All steps complete through stress test and polish.

| Step | Description | Status |
|---|---|---|
| 0 | Bridge protocol & shared types | ✅ Done |
| 1 | MAC translation unit (6 tests) | ✅ Done |
| 2 | Raw Ethernet socket (skeleton) | ✅ Done |
| 3 | CSR state machine (10 tests) | ✅ Done |
| 4 | Descriptor ring walker (8 tests) | ✅ Done |
| 5 | Full daemon (sim bridge) | ✅ Done |
| 6 | FPGA autoconfig (sim + Quartus) | ✅ Done |
| 7 | FPGA boardram window | ✅ Done |
| 8 | FPGA chip register bridge + DTACK stretch | ✅ Done |
| 9 | Integration (ARM + FPGA on MiSTer) | ✅ Done |
| 10 | Doorbell architecture — lance-test 5/5 PASS | ✅ Done |
| 11 | Stress test & polish — x100: 100/100 | ✅ Done |

> **Headline result:** lance-test diagnostics **5/5 PASS** — "Controller PASSED diagnostics"
> (build 20260609a + byteswap daemon). Buffer, Config, Interrupt, Collision and Loopback all pass.
> Surpasses the old-bridge baseline of 3/4.
>
> **Re-verified on the upstream merge:** `Minimig_20260613a.rbf` (doorbell merged onto Minimig
> Release 20260603) is tested on hardware — lance-test **5/5 PASS**, DHCP lease + LAN/internet ping,
> Quartus-clean (setup +0.377 ns / hold +0.246 ns). Full parity with 20260609a.

| 5/5 | 100/100 | 10/10 | DHCP |
|:---:|:---:|:---:|:---:|
| lance-test diags PASS | x100 stress runs ALL PASS | a2065_memtest PASS | real lease on live network |

### Validation detail

- **lance-test diags:** Buffer / Config / Interrupt / Collision / Loopback all PASS.
- **External Loopback:** PASS, repeatable — real frames out `eth1` and back through the full TX path
  (68k → `do_transmit` → raw socket → wire → `gotfunc` → RX ring). Confirms actual networking, not sim loopback.
- **a2065_memtest:** 10/10 — walking-bit, address-uniqueness, byte access, odd/even independence,
  post-INIT integrity, CSR checks.
- **x100 stress:** single persistent daemon, all 5 tests at 100%.
  Report `tests/lance_logs/results_100x_20260609_143735.md`.
- **AddNetInterface A2065:** a stock AmigaOS TCP/IP stack obtained DHCP lease `192.168.1.190` with route
  and DNS, then pinged the network.
- **Daemon footprint:** 2–7% CPU under load, ~ 2% / 11.7 MB resident, no leak. Adaptive poll backoff dropped
  idle CPU from ~ 100% (busy-spin) to ~ 2–9%; a `--debug` flag gates verbose logging.

---

## 05 · The Hard Bugs (and Their Fixes)

The doorbell port hung on first boot. Three distinct bugs, then a data-path bug, then a byte-order bug —
each cost build cycles to isolate.

### Three bugs that caused the doorbell hang

1. **Back-pressure** — second RDP write saw `cmd_pending` still set and stretched DTACK forever.
   *Fix:* `regs_nrdy = 0` always; new write overwrites the pending doorbell.
2. **ddram nrdy guard** — `nrdy_state` could enter `NR_WAIT` without issuing a DDR3 request → response
   never arrives → hang. *Fix:* guard the transition with `!sys_req && !sys_got_resp`.
3. **Edge-detect miss** — one-cycle edge pulse for boardram requests was missed while the FSM serviced other
   slots. *Fix:* level detection, like the CMD doorbell.

### Buffer test — the AS+DS capture fix (build 20260608h)

Root cause: capturing the boardram request on raw `sel_br = sel && cpu_addr[15]` samples during the
*address* phase — before write strobes and write data are valid. Direction and data were both read too early.

*Fix:* `sel_br = sel && cpu_addr[15] && !cpu_as_n && !cpu_ds_n` and `is_write = ~cpu_rw`. DS deasserts
between cycles, giving a clean `NR_DONE→NR_IDLE` separation. The earlier `cpu_rw` attempts failed only
because they kept raw `sel_br` with no DS qualification.

### Collision test — daemon byteswap

The 68k is big-endian; the FPGA stores each 16-bit word little-endian in its DDR3 lane. The daemon read the
init block byteswapped → wrong TDRA/RDRA → `do_transmit` found no TX descriptors. Fix: flat accessors XOR
every byte index with 1 (`boardram[(off ^ 1) & RAM_MASK]`). After the fix TDRA resolves and all 10 collision
sends run.

> ⚠️ **Gotcha that cost ~ 5 build cycles:** a stale duplicate `src/boardram_access.h` existed only on the build
> host. Because the `.cpp` files live in `src/` and include the header by name, the compiler resolved the
> same-dir copy *before* `-I include`, silently ignoring edits to the real header. Verify the active header with
> `g++ -I include -E src/registers.cpp | grep get_ram_byte`.

---

## 06 · Critical Design Decisions

- **DDR3 mailbox over HPS2FPGA** — the lightweight bridge doesn't work on MiSTer; DDR3 via `f2sdram2` does.
- **Level detection for all CDC requests** — edge pulses get missed when the FSM is busy; levels never lost.
- **Read-modify-write for boardram** — Quartus can't infer cross-domain TDP M10K from byte arrays; single
  16-bit array + RMW with byte enables is the Quartus-friendly pattern.
- **Capture must qualify on AS+DS, not raw select** — raw select is the address phase; data & direction
  aren't valid yet.
- **No back-pressure on doorbell RDP writes** — the daemon only needs the latest state; new write overwrites pending.
- **Do NOT modify the v5 arbiter** — its burst-tracking "bug" accidentally pins grant to the A2065 master,
  stopping the PAL service from stealing the bus. Every "fix" regressed.
- **Self-loopback collision detection** — the Am7990 relies on a physical loopback plug; MODE_COLL is never set
  in the init block, so the daemon detects DST MAC == own MAC and simulates the collision.

---

## 07 · Bring-up Methodology

The DDR3 mailbox path was brought up across **8 FPGA builds in one session** using a binary-search isolation
technique — hardcoded magic return values that pinpoint exactly which stage of the multi-stage path fails,
without DDR3 muddying the picture.

| Marker | What it isolates | Proved |
|---|---|---|
| `$DEAD` | Watchdog timeout returns 0xDEAD instead of open-bus 0x0000 | Register module triggered vs open-bus read |
| `$BEEF` | DDR3 removed entirely; mailbox returns a constant | CDC + handshake path works |
| `$CAFE` | DDR3 write restored, read/poll skipped | Arbiter grants + `f2sdram2` accepts writes |

The remaining gap — DDR3 *reads* never completing — was then narrowed to two Avalon bugs: the arbiter cleared
`burst_active` before `readdatavalid` arrived (routing the response to the wrong master), and the mailbox
didn't hold `avl_read` asserted until `!waitrequest`. Both classic Avalon protocol traps.

---

## 08 · Engineering Lessons

Distilled from the build journal — the non-obvious traps that cost the most time.

- **Gate capture on the data strobe.** A raw chip-select is high during the 68k *address* phase, before write
  data and R/W are valid. Qualify request capture with `!AS && !DS` and sample `cpu_rw` then. DS also deasserts
  between cycles → clean access separation without edge detection.
- **Endianness bites only cross-domain.** 68k big-endian words land little-endian in the DDR3 lane. 68k-only
  tests (memtest, buffer) are self-consistent and pass, hiding the swap. It breaks only when the little-endian
  daemon parses 68k-written structures (init block, descriptors). Fix: XOR the flat byte offset with 1.
- **A doorbell needs drain confirmation.** A single no-back-pressure CMD slot silently drops writes when the
  producer outruns the consumer — it lost the rapid LANCE INIT RAP/RDP burst. Stretch the producer (DTACK)
  until the consumer has actually drained, not merely until the value was posted.
- **The build host is rsync'd, not the repo.** A stale `src/boardram_access.h` existed only on the build machine
  and shadowed `include/` (same-dir `#include` wins over `-I`). Edits had zero effect, md5 unchanged. When a
  change "does nothing," preprocess (`g++ -E`) to see what the compiler actually used.
- **Avalon read/write burst tracking differs.** Writes complete on `!waitrequest`; reads complete on
  `readdatavalid`. The arbiter must track them separately, and masters must hold `read` asserted until
  `!waitrequest` — a single-cycle read pulse is lost if the slave has waitstates.
- **Address formats & unconnected outputs.** Minimig uses `wire [23:1]` word addresses — reconstruct byte
  addresses with `{addr, 1'b0}`. And an unconnected Verilog `output` is silently ignored: `regs_nrdy` was
  declared but unwired, making the whole register module invisible to the DTACK path.

---

## 09 · Build & Test Environment

| Role | Host | Notes |
|---|---|---|
| Quartus FPGA build | `nshearman@192.168.1.65` · Quartus 17.0 | ~ 25 min wall. Output `Minimig.rbf`. Good builds: `Minimig_20260613a.rbf` (latest, upstream-merged, tested) · `Minimig_20260609a.rbf` |
| ARM cross-compile | `root@192.168.1.97` · `arm-none-linux-gnueabihf-g++` | `make doorbell` → `a2065d_doorbell` |
| m68k cross-compile | `/opt/amiga/bin/m68k-amigaos-gcc` | Cross-built Amiga binaries run silently/empty here — only the pre-built `share:lance-test` and shell commands work |
| MiSTer target | `root@192.168.1.29` | Deploy to `/media/fat/trans/`. Serial `/dev/ttyS1` @ 115200. Core reload via `load_core` to `/dev/MiSTer_cmd` |

> **Operational notes:** use `\r` (not `\r\n`) for Amiga serial commands — `\n` eats the second word. Kill the
> conflicting init daemon: `killall minimig_netd` and disable `S90minimig_netd`. Daemon runs with `--iface eth1`.

### Building

```bash
# ARM daemon — doorbell architecture (cross-compile for MiSTer)
cd arm
make doorbell        # uses build/doorbell/ → a2065d_doorbell

# ARM daemon — native build for host-side unit testing
make native
./build/native/a2065d --help

# Unit tests
make test

# FPGA simulation
cd ../fpga/sim
make sim_autoconfig  # also: tb_boardram, tb_regfile

# FPGA bitstream (on the Quartus host)
#   cd ~/Development/Minimig-AGA_MiSTer
#   /opt/altera/17.0/quartus/bin/quartus_sh --flow compile Minimig
```

### Deploy to MiSTer

```bash
# Daemon (kill the old one first; exFAT sync mount may need rm before scp)
ssh root@192.168.1.29 'killall a2065d_doorbell; rm -f /media/fat/trans/a2065d_doorbell'
scp arm/build/doorbell/a2065d_doorbell root@192.168.1.29:/media/fat/trans/

# Core
scp Minimig_20260609a.rbf root@192.168.1.29:/media/fat/
ssh root@192.168.1.29 "echo 'load_core /media/fat/Minimig_20260609a.rbf' > /dev/MiSTer_cmd"

# Run daemon (USB NIC must enumerate as eth1 — needs the custom kernel)
ssh root@192.168.1.29 '/media/fat/trans/a2065d_doorbell --iface eth1'
```

### Test (lance-test via serial)

```bash
cd tests
python3 -m pytest test_lance.py -v -s
# A2065_CORE=/media/fat/Minimig_20260609a.rbf A2065_DAEMON=/media/fat/trans/a2065d_doorbell
# x100 stress: .venv/bin/python run_lance_100x.py 100
```

---

## 10 · Stress Test & Production Readiness

> **Verdict: stress passed — the A2065 doorbell is production-solid under sustained real-world load.**
> Networking is fully functional on **both AmigaOS TCP/IP stacks — Roadshow and MiamiDX**. Stable over hours of
> saturated transfer with no daemon fault, no errors, no drops, no stalls. Steps 0–11 complete.

### Evidence (this session)

1. **Repeated 100 MB downloads** — Amiga `wget` command over the A2065, 08:36→09:51, ~ 75 min continuous, ~ 15 back-to-back
   100 MB pulls at a steady ~ 505 KB/s. Every one completed `[104857600/104857600]`. Zero variance, zero corruption.
2. **Daemon health under load** — `top`: steady ~ 11–12% CPU, RSS flat at 11772 KB start→end (no leak), PID stable,
   no crash or restart across the whole window.
3. **Amiga-native TCP transfer** — `test-fix.pcap`: the Amiga's own stack doing an HTTP GET: 109 MB @ 520 KB/s,
   0 lost segments, 0 RST, 0.014% retransmit. Clean.
4. **DHCP + ping, both stacks** — Roadshow *and* MiamiDX both lease `192.168.1.190`; ping 18/18 + 43/43, zero loss.

> **Throughput ceiling is the 68020, not the A2065.** ~ 520 KB/s with zero-window flow control means the 68020
> (50 MHz) simply can't drain the RX ring any faster — the card and DDR3 bridge keep up fine. The bottleneck is the
> emulated Amiga itself, not the emulated card.

| ~ 75 min | ~ 505 KB/s | 11772 KB | 0.014% |
|:---:|:---:|:---:|:---:|
| continuous saturated transfer | steady, zero variance | RSS flat — no leak | retransmit, 0 RST |

### Remaining Work

| Item | Kind | Status |
|---|---|---|
| Both TCP/IP stacks (Roadshow + MiamiDX) — DHCP + ping + saturated transfer | Integration | ✅ Done — both work |
| Clean unattended full x100 run (detached, reach 100) | Validation | ✅ Done — 100/100 |
| Issue B — RX multicast flood (LADRF hash filter) | Correctness | ✅ Fixed + verified |
| Issue A — station MAC low bytes `…00:00` (collision risk with 2 cards) | Cosmetic (single card) | 🔲 Open |
| MAC display byte ordering — shows `00:FFFFFF80:10:...` (sign-extension) | Cosmetic | 🔲 Open (issue #3) |
| Connect `cpu_berr_n` — watchdog timeout should raise BERR, not return $0000 | Robustness | 🔲 TODO (low) |
| Packet throughput — bounded by the 68020, not the card | Performance | ✅ Characterized |

Two non-blocking issues remain: **Issue A** (on-wire MAC low bytes `00:00`) — cosmetic for a single card, a
MAC-collision risk only with two A2065 on one LAN; and the MAC *display* sign-extension. **Issue B** (multicast
flood) was fixed and verified this session.

---

## 11 · Repository Map

```
A2065/
├── DESIGN.md / IMPLEMENTATION_PLAN.md   architecture + 11-step plan
├── arm/                                 ARM Linux daemon (Amiberry port)
│   ├── src/   registers, rings, mac, ethernet, crc32,
│   │          main_doorbell.cpp, boardram_remote.cpp, *_test.cpp
│   └── include/  a2065_bridge.h, a2065_types.h, boardram_access.h
├── fpga/
│   ├── rtl/   a2065_top, a2065_autoconfig, a2065_regfile, a2065_ddram
│   ├── sim/   tb_autoconfig, tb_boardram, tb_regfile (all passing)
│   └── constraints/  a2065.sdc
├── tests/   test_lance.py (pytest), run_lance_100x.py, mister_ssh.py, serial_long.py
├── docs/    AMD_Am7990.pdf, DESIGN_INTERNAL_LOOPBACK.md, Roadshow.md, A2065_Project_Article.{html,md}
└── Minimig-AGA_MiSTer/  submodule — upstream core, modified for A2065
    └── rtl/A2065/  a2065_regfile.v, a2065_ddram.v, a2065_ddr3_mailbox.v, avalon_arbiter.v
```

### Minimig core modifications

- `cpu_wrapper.v` — A2065 autoconfig nibbles (type 0xC1, product 0x70, mfr 0x0202), base latched from `0xE80048`.
- `gary.v` — `sel_a2065 = a2065_ena && cpu_address_in[23:16]==a2065_base`.
- `minimig.v` — instantiates `a2065_regfile` + `a2065_ddram`, OR-ties data mux, 2-stage CDC for INT2 into Paula's `int2`.
- `sys_top.v` — `a2065_ddr3_mailbox` (clk_audio), `avalon_arbiter` sharing `f2sdram2`, interrupt wiring.

---

## Reference

- Amiberry `src/a2065.cpp` by Toni Wilen (2009) — reference implementation
- AMD Am7990 LANCE datasheet — bus timing specifications ([`docs/AMD_Am7990.pdf`](docs/AMD_Am7990.pdf), [`docs/am79c90.md`](docs/am79c90.md))
- Commodore A2065 Hardware Reference — Zorro II card details
- Zorro II specification — Amiga Hardware Reference Manual chapter 6
- [`DESIGN.md`](DESIGN.md) · [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) — architecture and the 11-step plan

*Port of Amiberry `a2065.cpp` (Toni Wilen, 2009).*
