# Releases

The binaries used to verify the A2065/IDE deadlock fix on hardware (2026-10-13),
see [`docs/A2065_IDE_Deadlock.md`](../docs/A2065_IDE_Deadlock.md). Both fixes are
merged upstream ([Main_MiSTer#1357](https://github.com/MiSTer-devel/Main_MiSTer/pull/1357),
[Minimig-AGA_MiSTer#256](https://github.com/MiSTer-devel/Minimig-AGA_MiSTer/pull/256)),
so the next official Main_MiSTer and Minimig-AGA_MiSTer releases include them.

| File | MD5 | What it is |
|---|---|---|
| `Minimig_AP030_d8dda39_racefix_20261013.rbf` | `faaa0e1fc7da35fb571a0329571062c2` | Minimig-AGA core with the AP030 (MC68030) CPU from [AP68030](https://github.com/apolkosnik/AP68030) `d8dda39`, A2065 with the CSR0-acknowledge mailbox fix |
| `MiSTer_A2091` | `206f67f15587015b7b2070b1349b57a2` | Main_MiSTer with the A2065/IDE deadlock fix, plus the A4091 / A2091 SCSI and ZZ9000 RTG support |

## Install

Copy the RBF to `/media/fat/_Computer/` and the binary to `/media/fat/`. Then
point the Minimig cores at the binary in `MiSTer.ini` (this is how it was
tested; your own `/media/fat/MiSTer` is left alone):

```ini
[minimig]
main=MiSTer_A2091
```

Enable the card under OSD → System → Ethernet (eth0, macvlan or tap0) and save
the configuration, or a core reload turns it off again. The binary reads and
writes the same `Minimig.cfg` layout as the A4091/A2091 builds; keep a backup of
yours before first use.

## Test results

On a DE10-Nano, with Roadshow up and a UDP flood keeping receive interrupts
coming. Each pass copies 34 MB (194 files) from `RAM:` to DH0 and back.

- 33 passes over 24 minutes without a hang.
- 5 more passes with the read-back verified by md5: 194/194 files.
- The same test with the Main binary from before the fix hung about 1 s into
  the first copy.

Network checks were also clean: DHCP, ping, a 100 MB wget to disk, and
AmiSpeedTest at 4.5 / 4.2 Mbit/s.

## Sources (GPL)

- Core: [georgemog/Minimig-AGA_MiSTer `ap030-latest-cpu-racefix`](https://github.com/georgemog/Minimig-AGA_MiSTer/tree/ap030-latest-cpu-racefix)
  (`30ef67c`). Built with Quartus 17.0, seed 7.
- Main: [MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) `d5f3844`, with these applied:
  - [georgemog/A2091](https://github.com/georgemog/A2091) `integration/main_mister_d5f3844_scsi.patch` (A4091 + A2091)
  - [georgemog/ZZMiSTerRTG](https://github.com/georgemog/ZZMiSTerRTG) (ZZ9000 RTG)
  - the A2065/IDE fix from [Main_MiSTer#1357](https://github.com/MiSTer-devel/Main_MiSTer/pull/1357)
    ([georgemog/Main_MiSTer `a2065-ide-deadlock`](https://github.com/georgemog/Main_MiSTer/tree/a2065-ide-deadlock))
