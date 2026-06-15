# A2065 Manual Installation

Where every artifact goes on a MiSTer (DE10-Nano) + virtual Amiga, the command
to put it there, and the design limitations you should know before relying on it.

> **Scope.** This describes a *manual* install of a pre-built release. Build
> instructions (Quartus / ARM cross-compile / m68k) live in [`../README.md`](../README.md)
> and [`../CLAUDE.md`](../CLAUDE.md). The release artifacts referenced below are in
> [`../releases/`](../releases/) and [`../kernel/`](../kernel/).

---

## 0. Prerequisites

| Need | Why |
|------|-----|
| MiSTer with root SSH (default `root@<mister-ip>`) | All Linux-side copies |
| A USB Ethernet adapter using the **ASIX `AX8817x`** chipset (e.g. `AX88772B`) | Enumerates as `eth1` — the wire interface the daemon binds to |
| The custom kernel (this repo) | Stock MiSTer kernel has **no** USB-NIC driver — see §2 and Limitation L1 |
| A mounted Amiga volume with a `share:` assign (optional) | Only for the `lance-test` diagnostic binary |

This repo's MiSTer host in development is `root@192.168.1.29`. Substitute your own IP.

---

## 1. Components & destinations (summary)

| # | Artifact | Repo source | MiSTer / Amiga destination | Required? |
|---|----------|-------------|----------------------------|-----------|
| A | FPGA core | `releases/Minimig_20260613a.rbf` | `/media/fat/_Computer/` + `Minimig.rbf` symlink | **Yes** |
| B | ARM daemon | `releases/a2065d_doorbell` | `/media/fat/linux/a2065d_doorbell` (chmod +x) | **Yes** |
| C | Custom kernel | `kernel/zImage_dtb` | `/media/fat/linux/zImage_dtb` (back up original) | **Yes** (for networking) |
| D | Main_MiSTer binary (OSD ethernet menu) | built from `Main_MiSTer` fork | `/media/fat/MiSTer` (back up original) | Optional (enables OSD toggle) |
| E | iface config | (auto-created) | `/media/fat/config/a2065_iface.txt` | Auto |
| F | Manual start scripts | `releases/start_a2065d`, `releases/start_a2065d_log` | anywhere (e.g. `/media/fat/`) | Optional (if no menu D) |
| G | lance-test diag binary | `lance-test/Lance-Test` | Amiga `share:lance-test` | Optional (testing) |

---

## 2. Step-by-step

### A — FPGA core (RBF)

The Minimig core for MiSTer lives in `_Computer/`. Copy the build and point the
`Minimig.rbf` symlink at it so the menu launches the A2065-enabled core.

```sh
scp releases/Minimig_20260613a.rbf root@<mister>:/media/fat/_Computer/
ssh root@<mister> "cd /media/fat/_Computer && rm -f Minimig.rbf && ln -s Minimig_20260613a.rbf Minimig.rbf"
```

> `Minimig_20260613a.rbf` is the upstream-merged release (Minimig Release 20260603 +
> doorbell). `Minimig_20260609a.rbf` is the older fork-only build; use one or the other,
> not both as the symlink target.

### B — ARM daemon

Deploy to the **stable** path `/media/fat/linux/` (survives, runs early), not the dev
`/media/fat/trans/` location.

```sh
scp releases/a2065d_doorbell root@<mister>:/media/fat/linux/a2065d_doorbell
ssh root@<mister> "chmod +x /media/fat/linux/a2065d_doorbell"
```

### C — Custom kernel (hard prerequisite for networking)

The stock MiSTer kernel ships no USB-Ethernet driver. This kernel (Linux 5.15.1) has
the ASIX `AX8817x` driver built in statically. **Back up the original first.**

```sh
ssh root@<mister> "cp /media/fat/linux/zImage_dtb /media/fat/linux/zImage_dtb.orig"
scp kernel/zImage_dtb root@<mister>:/media/fat/linux/zImage_dtb
# reboot MiSTer to boot the new kernel
ssh root@<mister> "reboot"
```

After reboot, confirm `eth1` exists: `ssh root@<mister> "ip link show eth1"`.

> No rootfs `.ko` files needed — the driver is built-in, not a module. This dodges the
> MiSTer module-autoload mismatch (running `uname` `5.15.1+` ≠ `/lib/modules/5.15.1-MiSTer`).
> Because the driver is fully built-in there are **no alias / modprobe files** — the kernel
> binds devices by USB VID:PID compiled into the image.

#### Supported USB adapters

The `asix` driver (USB 2.0, AX8817x / AX88772x family) binds the devices listed in the
upstream source `drivers/net/usb/asix_devices.c`. Any of these will enumerate as `eth1`:

**ASIX reference chips**

| Chip | USB VID:PID |
|------|-------------|
| AX88172 | `0b95:1720` |
| AX88172A | `0b95:172a` |
| AX88178 | `0b95:1780` |
| AX88772 | `0b95:7720` |
| AX88772A / AX88772B **(this project's device)** | `0b95:772a` / `0b95:772b` |
| AX88772C / AX88760 | — |

**OEM / rebranded (same driver)**

| Adapter | USB VID:PID |
|---------|-------------|
| D-Link DUB-E100 | `2001:1a02` / `2001:3c05` |
| Netgear FA120 | `0846:1040` |
| Linksys USB200M | `077b:2226` |
| Hawking UF200 / Sitecom LN-029 | `07aa:0017` |
| Intellinet / ST Lab USB Ethernet | `0b95:7720` |
| Belkin F5D5055 | `050d:5055` |
| Apple USB Ethernet Adapter | `05ac:1402` |
| Cables-to-Go USB Ethernet | `0b95:772b` |

Also bound (same driver, IDs in source): ABOCOM, Surecom EP-1427X-2, Billionton,
ATEN UC210T, Buffalo LUA-U2-KTX / LUA-U2-GT, Sitecom LN-028 / LN-031, Lenovo USB 2.0
Ethernet, Logitec LAN-GTJ/U2A, Asus AX88772, MSI / various no-name AX88772 dongles,
Lindy USB 2.0 Ethernet, Goodway / Marubun, JVC MP-PRX1, HP hs2300 / various.

### D — Main_MiSTer binary (OSD ethernet menu) — optional

The forked `Main_MiSTer` adds an `Ethernet : OFF/eth0/eth1` item to the Minimig System
(OSD) page that starts/stops the daemon and kills the conflicting `minimig_netd`
automatically. Back up the original binary.

```sh
ssh root@<mister> "cp /media/fat/MiSTer /media/fat/MiSTer.bak.preA2065"
scp /path/to/built/MiSTer root@<mister>:/media/fat/MiSTer
```

Code: `Main_MiSTer/support/minimig/minimig_a2065.{h,cpp}`, `menu.cpp`, `user_io.cpp`,
`minimig_config.cpp`. Build/deploy detail in [`docs/OSD_Ethernet_Menu_Plan.md`](OSD_Ethernet_Menu_Plan.md) §13.

### E — iface config

`/media/fat/config/a2065_iface.txt` holds `off` | `eth0` | `eth1`. The OSD menu (D)
creates and persists it; the menu launches the daemon from `/media/fat/linux/a2065d_doorbell`.
No manual step needed if D is installed.

### F — Manual start (only if you skip the OSD menu D)

Without the forked MiSTer binary, start the daemon by hand. **First disable the
conflicting service** (Limitation L6):

```sh
ssh root@<mister> "killall minimig_netd 2>/dev/null; \
  mv /etc/init.d/S90minimig_netd /etc/init.d/S90minimig_netd.disabled 2>/dev/null"
```

Then either run the daemon directly or use a start script:

```sh
# direct
ssh root@<mister> "nohup /media/fat/linux/a2065d_doorbell --iface eth1 > /tmp/a2065d.log 2>&1 &"
```

`releases/start_a2065d` (log to `/tmp`) and `releases/start_a2065d_log` (verbose
`--debug` log to `/media/usb0`) are ready-made versions — note they reference the dev
path `/media/fat/trans/`; edit to `/media/fat/linux/` if you deployed per §B.

### G — lance-test diagnostic (optional)

`lance-test/Lance-Test` is a pre-built AmigaOS binary. Copy it onto the Amiga
filesystem where the `share:` assign points, as `lance-test`. Run from the Amiga
serial shell:

```
share:lance-test diags
```

> Cross-compiled Amiga C programs run silently/empty on this Minimig (Limitation L8);
> this **pre-built** binary is the only working on-Amiga test tool.

---

## 3. Bring-up order

1. Install kernel (C) → reboot → verify `eth1`.
2. Install core (A) + daemon (B).
3. Install OSD binary (D) **or** prepare manual start (F).
4. Load the Minimig core. Set OSD `Ethernet : eth1` (D) — or run start script (F).
5. On the Amiga: `AddNetInterface a2065` from your TCP/IP stack (Roadshow / MiamiDX /
   AmiTCP 3.3) → DHCP lease + ping.
6. Optional: `share:lance-test diags` → expect 5/5 PASS.

---

## 4. Design limitations

**L1 — Custom kernel is mandatory.** Stock MiSTer Linux has only the on-board NIC and
no USB-Ethernet driver, so there is no wire-side interface for the daemon's raw socket.
Networking is dead without the kernel in §2C. Not optional.

**L2 — USB NIC is ASIX-only.** The supplied kernel enables only the `AX8817x` driver
(asix, USB 2.0). Gigabit `AX88179`, CDC-ECM, RTL8152, DM9601, SMSC95xx, etc. are
disabled. A non-ASIX USB adapter will not enumerate. Supported VID:PIDs listed under
§2C "Supported USB adapters" — anything outside that list is dead.

**L3 — Throughput bound by the 68000, not the card.** Verified HTTP download ran
~520 KB/s (109 MB clean); the Amiga's 68000 zero-window flow control is the bottleneck,
not the A2065 path. RDP register writes add one daemon round-trip each (doorbell
back-pressure) — fine for control, may bound TX rate.

**L4 — Amiga-visible station MAC is `00:80:10:00:00:00` (cosmetic).** The Amiga reads
its station address from FPGA autoconfig `er_SerialNumber`, which the daemon cannot
change and which boots to `…00:00:00`. **The on-wire MAC is correct and unique** —
the daemon munges it to a NIC-derived `00:80:10:XX:YY:ZZ` on TX and back on RX (verified
by pcap). A true fix for the *displayed* address needs FPGA `er_SerialNumber` wiring and
has an autoconfig boot-race; left as cosmetic.

**L5 — `lance-test` MAC display shows `00:FFFFFF80:10:…`.** Sign-extension of the `0x80`
byte in a signed `char` printf **inside the pre-built `share:lance-test` binary** (no
source in this repo). Display-only; the wire MAC is correct. Won't-fix — not our code.

**L6 — `minimig_netd` conflicts with the daemon.** MiSTer's `/etc/init.d/S90minimig_netd`
starts `minimig_netd`, which fights the A2065 daemon for shared memory. The OSD menu (D)
kills it automatically; for manual installs you must disable it (§2F).

**L7 — `eth1` vs `eth0`.** `eth0` is the on-board MiSTer NIC (may be in use by MiSTer
itself). Bind the daemon to the USB adapter `eth1` (`--iface eth1`). The daemon falls
back to `eth0` only for deriving a unique `realmac` when no iface MAC is available.

**L8 — Cross-compiled Amiga binaries don't run here.** Both GCC `-noixemul` and VBCC
output silently exit / produce no output on this Minimig serial. Only the pre-built
`share:lance-test` and native AmigaOS shell commands work for on-Amiga testing.

**L9 — A dead daemon hangs the Amiga bus, not BERR.** The doorbell RTL has no watchdog;
register reads are zero-latency (can't time out), but register *writes* and boardram
accesses stretch DTACK. If the daemon dies mid-access the 68k bus stalls rather than
taking a bus error (the TG68/020 has no usable BERR line). Keep the daemon running while
the Amiga uses the card. (A future watchdog-release returning `$FFFF` could harden this.)

**L10 — No mutex between RX thread and main thread.** Both touch CSR0 and boardram.
Currently benign — `registers_csr0()` reads a volatile `uint16_t` (atomic on ARM) and
flat DDR3 boardram access is effectively single-threaded — but it is not formally locked.
