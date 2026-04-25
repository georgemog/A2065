# A2065 / ARIADNE Autoconfig ROM Bytes

Extracted from Amiberry `src/expansion.cpp`, `expansionroms[]` array.

## A2065 (Commodore)

```c
// From expansion.cpp expansionroms[] entry for "a2065"
{ 0xc1, 0x70, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
// bytes [12..15] implicitly 0x00
```

### Decoded

| Index | Value | Field | Notes |
|-------|-------|-------|-------|
| [0]   | 0xC1  | er_Type | 0xC0=ZorroII + 0x01=64KB |
| [1]   | 0x70  | er_Product | 112 |
| [2]   | 0x00  | er_Flags | |
| [3]   | 0x00  | reserved | |
| [4]   | 0x02  | er_Manufacturer high | |
| [5]   | 0x02  | er_Manufacturer low | 0x0202 = 514 = Commodore-Amiga |
| [6]   | 0x00→MAC[2] | er_SerialNumber byte 0 | patched at runtime |
| [7]   | 0x00→MAC[3] | er_SerialNumber byte 1 | patched at runtime |
| [8]   | 0x00→MAC[4] | er_SerialNumber byte 2 | patched at runtime |
| [9]   | 0x00→MAC[5] | er_SerialNumber byte 3 | patched at runtime |
| [10]  | 0x00  | er_InitDiagVec high | no diag vector |
| [11]  | 0x00  | er_InitDiagVec low | |
| [12–15] | 0x00 | reserved | |

### MAC Handling

From `a2065_config()` in `a2065.cpp`:
```c
// Force first 3 bytes to Commodore OUI
maco[0] = 0x00;
maco[1] = 0x80;
maco[2] = 0x10;

// Patch er_SerialNumber with host-specific MAC bytes
aci->autoconfig_bytes[6] = realmac[2];
aci->autoconfig_bytes[7] = realmac[3];
aci->autoconfig_bytes[8] = realmac[4];
aci->autoconfig_bytes[9] = realmac[5];
```

Full MAC presented to Amiga driver: `00:80:10:MAC[3]:MAC[4]:MAC[5]`

The A2065 driver reads the MAC from er_SerialNumber during OpenDevice.

---

## ARIADNE (Village Tronic)

```c
// From expansion.cpp expansionroms[] entry for "ariadne"
{ 0xc1, 0xc9, 0x00, 0x00, 0x08, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
// manufacturer 2167 = 0x0877 = Village Tronic
// MAC OUI: 00:60:30
```

Note: ARIADNE is out of scope for this project (A2065 only).

---

## ZorroII Bus Convention

Autoconfig ROM values are **inverted** on the ZorroII data bus.
FPGA must output `~nibble` on D[7:4] for each autoconfig read.

Autoconfig reads are nibble-wide: each 16-bit word read from 0xE80000+offset
delivers one nibble on bits D[7:4]. The 16 bytes above = 32 nibbles = 32 reads.

AmigaOS autoconfig code (in ROM) reads nibbles at:
- 0xE80000 + 0x00 → byte[0] high nibble
- 0xE80000 + 0x02 → byte[0] low nibble
- 0xE80000 + 0x04 → byte[1] high nibble
- ... etc.

After reading all nibbles, AmigaOS writes the card's base address to:
- 0xE80000 + 0x48 (low byte) / 0xE80000 + 0x4A (high byte)

Then writes SHUTUP to 0xE80000 + 0x4C to end autoconfig for this card.
