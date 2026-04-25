# A2065 Ethernet Emulation for Minimig MiSTer

Full hardware emulation of the Commodore A2065 ZorroII Ethernet card.
Uses standard AmigaOS A2065 drivers — no custom Amiga-side software required.

## Project Structure

```
A2065/
├── DESIGN.md               Full architecture document
├── IMPLEMENTATION_PLAN.md  Step-by-step plan with verification criteria
├── README.md               This file
├── arm/                    ARM Linux daemon (port of Amiberry a2065.cpp)
│   ├── Makefile
│   ├── include/
│   │   ├── a2065_bridge.h  HPS2FPGA bridge register layout
│   │   └── a2065_types.h   CSR/TX/RX bit definitions
│   └── src/
│       ├── main.cpp        Daemon entry point
│       ├── registers.cpp   Am7990 CSR state machine
│       ├── rings.cpp       TX/RX descriptor ring walker
│       ├── mac.cpp         MAC address translation (mungepacket)
│       ├── ethernet.cpp    AF_PACKET raw socket
│       ├── bridge.cpp      /dev/mem mmap for HPS2FPGA bridge
│       └── crc32.cpp       Ethernet FCS computation
├── fpga/                   Verilog for Minimig core
│   ├── rtl/
│   │   ├── a2065_top.v          Top-level integration
│   │   ├── a2065_autoconfig.v   ZorroII autoconfig ROM state machine
│   │   └── a2065_registers.v    DTACK-stretch for RAP/RDP access
│   └── sim/
│       ├── Makefile
│       └── tb_autoconfig.v      Autoconfig testbench
├── reference/              Amiberry source files for reference
│   ├── amiberry_a2065.h
│   ├── amiberry_ethernet.h
│   ├── amiberry_crc32.h
│   └── autoconfig_bytes.md  Decoded autoconfig ROM values
└── tests/                  Integration test scripts
```

## Building

### ARM Daemon (native, for testing on dev machine)

```bash
cd arm
make native
./build/native/a2065d --help
```

### ARM Daemon (cross-compile for MiSTer)

```bash
cd arm
make arm CXX=arm-none-linux-gnueabihf-g++ CC=arm-none-linux-gnueabihf-gcc
```

### Unit Tests

```bash
cd arm
make test
```

### FPGA Simulation

```bash
cd fpga/sim
make sim_autoconfig
```

### Deploy to MiSTer

```bash
cd arm
make deploy   # scp to root@mister:/usr/local/bin/a2065d
```

## Status

Implementation in progress — see IMPLEMENTATION_PLAN.md for current step.

| Step | Description | Status |
|------|-------------|--------|
| 0 | Bridge protocol & shared types | Done (headers) |
| 1 | MAC translation unit | Skeleton done |
| 2 | Raw Ethernet socket | Skeleton done |
| 3 | CSR state machine | Skeleton done |
| 4 | Descriptor ring walker | Skeleton done |
| 5 | Full daemon (sim bridge) | TODO |
| 6 | FPGA autoconfig | Skeleton done |
| 7 | FPGA boardram window | TODO |
| 8 | FPGA chip register bridge | Skeleton done |
| 9 | Integration | TODO |
| 10 | Stress test & polish | TODO |

## Reference

- Amiberry `src/a2065.cpp` by Toni Wilen (2009) — reference implementation
- AMD Am7990 LANCE datasheet — bus timing specifications
- Commodore A2065 Hardware Reference — ZorroII card details
- ZorroII specification — Amiga Hardware Reference Manual chapter 6
