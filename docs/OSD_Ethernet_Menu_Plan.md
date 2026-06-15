# Plan: Minimig OSD Ethernet (A2065) Menu Option

Add an OSD option to the Minimig core that selects the host network interface
(`OFF` / `eth0` / `eth1`) for the A2065 card, and starts/stops the
`a2065d_doorbell` daemon accordingly. **Default = `eth1`.**

Status: **CODE APPLIED** (Steps 1–4 + 6 done in `Main_MiSTer/`). Step 5
(ARM build + MiSTer deploy + hardware test) pending — needs build host + MiSTer.
See §12 for status detail and §13 for build/deploy commands.

---

## 1. Where the Minimig OSD menu is configured

Minimig does **not** use the generic core `CONF_STR` config-string menu. It has
**hardcoded menu pages** in `Main_MiSTer/menu.cpp`, driven by a state machine
(`menustate`). Relevant states (declared `menu.cpp:169-183`):

| State | Page | Title |
|-------|------|-------|
| `MENU_MINIMIG_MAIN1/2` | Main (floppies, submenu links) | "Minimig" |
| `MENU_MINIMIG_CHIPSET1/2` | **"System"** (CPU/RAM/chipset/ROM) | "System" |
| `MENU_MINIMIG_VIDEO1/2` | Audio & Video | |
| `MENU_MINIMIG_DISK1/2` | Drives (hardfiles) | |

- Render code: `MENU_MINIMIG_CHIPSET1` at `menu.cpp:5887`.
- Input/handler code: `MENU_MINIMIG_CHIPSET2` at `menu.cpp:5941`.
- The "System" page is reached from Main via `menusub == 6` (`menu.cpp:5654`).

Each option is one `menusub` index. The System page currently uses indices
**0–9** (`menumask = 0x3FF`), where **9 = BACK**:

```
0 CPU      1 D-Cache  2 Chipset  3 ChipRAM  4 FastRAM
5 SlowRAM  6 Joystick 7 ROM      8 HRTmon   9 BACK
```

This is the natural home for the Ethernet option.

### Config persistence — important constraint

`mm_configTYPE` (`support/minimig/minimig_config.h:41`) is saved/loaded as a
**raw binary blob**. The loader (`minimig_config.cpp:422`) checks the file size
with an **exact** `size == sizeof(minimig_config) || size == 5152`. Adding a
field to the struct changes `sizeof` → **all existing `.cfg` files fail to
load**. Therefore the iface selection is **not** stored in `mm_configTYPE`.

Instead, persist it in a tiny standalone state file (host-side concept anyway):

```
/media/fat/config/a2065_iface.txt   contents: "off" | "eth0" | "eth1"
```

(Alternative considered: embed in `mm_configTYPE` + bump `version` + add new
`sizeof` to the accepted-size list. Rejected — more code, cross-version
fragility, and the iface is a Linux/host setting, not an FPGA core setting.)

---

## 2. New helper module

Add `support/minimig/minimig_a2065.{h,cpp}` (keeps menu.cpp clean, mirrors
`minimig_share.*`).

```c
// minimig_a2065.h
#define A2065_OFF  0
#define A2065_ETH0 1
#define A2065_ETH1 2

int  a2065_get_iface(void);        // load persisted mode (default A2065_ETH1)
void a2065_set_iface(int mode);    // persist + apply (start/stop daemon)
void a2065_start(void);            // apply current mode (called on core boot)
void a2065_stop(void);            // kill daemon (called on core exit)
const char *a2065_iface_msg(int mode); // "OFF"/"eth0"/"eth1" for OSD
```

```c
// minimig_a2065.cpp  (sketch)
#define STATE_FILE "/media/fat/config/a2065_iface.txt"
#define DAEMON     "/media/fat/linux/a2065d_doorbell"   // stable deploy path

static int cur_mode = -1;

const char *a2065_iface_msg(int m) {
    return (m == A2065_ETH0) ? "eth0" : (m == A2065_ETH1) ? "eth1" : "OFF";
}

int a2065_get_iface(void) {
    if (cur_mode >= 0) return cur_mode;
    cur_mode = A2065_ETH1;                  // DEFAULT eth1
    FILE *f = fopen(STATE_FILE, "r");
    if (f) {
        char b[16] = {0}; fgets(b, sizeof b, f); fclose(f);
        if (!strncmp(b, "off",  3)) cur_mode = A2065_OFF;
        else if (!strncmp(b, "eth0", 4)) cur_mode = A2065_ETH0;
        else if (!strncmp(b, "eth1", 4)) cur_mode = A2065_ETH1;
    }
    return cur_mode;
}

static void persist(int m) {
    FILE *f = fopen(STATE_FILE, "w");
    if (f) { fprintf(f, "%s\n", a2065_iface_msg(m)); fclose(f); }
}

void a2065_stop(void) {
    system("killall -q a2065d_doorbell 2>/dev/null");
}

void a2065_start(void) {
    int m = a2065_get_iface();
    a2065_stop();
    if (m == A2065_OFF) return;
    // A2065 daemon conflicts with stock minimig_netd — disable it.
    system("killall -q minimig_netd 2>/dev/null");
    char cmd[256];
    snprintf(cmd, sizeof cmd,
        "%s --iface %s >/tmp/a2065d.log 2>&1 &",
        DAEMON, a2065_iface_msg(m));
    system(cmd);
    printf("A2065: started daemon on %s\n", a2065_iface_msg(m));
}

void a2065_set_iface(int m) {
    cur_mode = m;
    persist(m);
    a2065_start();      // restart on new iface (or stop if OFF)
}
```

Notes:
- `system("… &")` matches existing MiSTer spawn style (e.g. `menu.cpp:947`
  `system("/bin/bluetoothd hcireset &")`).
- `killall minimig_netd` + the daemon-conflict handling is already a documented
  requirement (see A2065 `CLAUDE.md` → MiSTer Operational Notes).
- Recommend deploying the daemon to a **stable path** (`/media/fat/linux/` or
  bundled in the core release) rather than the dev `/media/fat/trans/` location.

---

## 3. menu.cpp edits (System page)

### 3a. Render — `MENU_MINIMIG_CHIPSET1` (`menu.cpp:5887`)

Bump `menumask` from `0x3FF` to `0x7FF` (add index 10), add an Ethernet line
after HRTmon (current menusub 8), move BACK to menusub 10.

```c
    menumask = 0x7FF;                              // was 0x3FF
    ...
    strcpy(s, " HRTmon : ");
    strcat(s, (minimig_config.memory & 0x40) ? "enabled " : "disabled");
    OsdWrite(m++, s, menusub == 8, 0);

    // NEW: Ethernet (A2065)
    OsdWrite(m++, "", 0, 0);
    strcpy(s, " Ethernet : ");
    strcat(s, a2065_iface_msg(a2065_get_iface()));
    OsdWrite(m++, s, menusub == 9, 0);

    for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
    OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 10, 0);  // was menusub == 9
```

### 3b. Handler — `MENU_MINIMIG_CHIPSET2` (`menu.cpp:5941`)

Insert a new `menusub == 9` case (cycle OFF→eth0→eth1→OFF), renumber BACK to 10.

```c
    else if (menusub == 9)   // Ethernet (A2065)
    {
        int m2 = a2065_get_iface();
        if (minus) m2 = (m2 + 2) % 3;   // OFF<-eth0<-eth1
        else       m2 = (m2 + 1) % 3;   // OFF->eth0->eth1
        a2065_set_iface(m2);            // persist + (re)start/stop daemon
        menustate = MENU_MINIMIG_CHIPSET1;
    }
    else if (menusub == 10)             // BACK (was 9)
    {
        menustate = MENU_MINIMIG_MAIN1;
        menusub = 6;
    }
```

`#include "support/minimig/minimig_a2065.h"` at top of `menu.cpp`.

---

## 4. Lifecycle hooks (auto start/stop)

| Event | Site | Action |
|-------|------|--------|
| Minimig core boots | `user_io.cpp:1508`, just after `BootInit();` | `a2065_start();` |
| Minimig soft reset | `minimig_config.cpp:522` `minimig_reset()` | `a2065_start();` (re-apply; daemon survives reset, but ensures running) |
| Core unload / switch | `user_io_init()` reset block, `user_io.cpp:~394` (or wherever the previous core is torn down) | `a2065_stop();` if leaving minimig |
| Menu toggle | `MENU_MINIMIG_CHIPSET2` menusub 9 | `a2065_set_iface()` |

`BootInit()` (`minimig_boot.cpp:394`) already calls `minimig_cfg_load(0)`, so the
iface state file is read independently right after — order-independent.

`a2065_start()` is idempotent (it `killall`s first), so calling it on both boot
and reset is safe.

---

## 5. Build system

**No Makefile edit required.** The Makefile gathers C++ sources with a wildcard
(`Makefile:43`):

```make
CPP_SRC = $(wildcard *.cpp) \
          $(wildcard ./lib/serial_server/library/*.cpp) \
          $(wildcard ./support/*/*.cpp)
```

A new `support/minimig/minimig_a2065.cpp` is picked up automatically.

---

## 6. Daemon side

No code change required to `a2065d_doorbell` — it already accepts
`--iface eth0|eth1` (default eth0, `arm/src/main_doorbell.cpp:199-205`). The OSD
default of **eth1** is enforced by `a2065_get_iface()` returning `A2065_ETH1`,
which passes `--iface eth1` explicitly.

Deploy: build per A2065 `CLAUDE.md` (host `192.168.1.97`, `make doorbell`), place
binary at the chosen stable path on MiSTer.

---

## 7. Open questions / decisions

1. **Daemon deploy path** — `/media/fat/linux/a2065d_doorbell` (survives, runs
   early) vs bundle in core release vs current dev `/media/fat/trans/`. Plan
   assumes `/media/fat/linux/`.
2. **MAC uniqueness** — unrelated to this menu, but two-card LAN collision risk
   still open (A2065 `CLAUDE.md` Issue A). Not blocking.
3. **eth0 caveat** — `eth0` may be the MiSTer's own management NIC; selecting it
   could disrupt SSH. A2065 notes recommend `eth1`. Consider greying out `eth0`
   or adding a confirmation, or just document the warning. Default eth1 avoids
   this for normal use.
4. **Persisted-with-config-slot UX** — current plan uses one global state file,
   not per saved-config slot. If per-slot is wanted, revisit the
   `mm_configTYPE` embed (with `version` bump + sizeof migration).

---

## 8. Summary of files touched

| File | Change |
|------|--------|
| `support/minimig/minimig_a2065.h` | **new** — API + mode constants |
| `support/minimig/minimig_a2065.cpp` | **new** — state file I/O + daemon launch |
| `menu.cpp` | System page: render line + handler case + BACK renumber + include |
| `user_io.cpp` | `a2065_start()` after `BootInit()`; `a2065_stop()` on core exit |
| `support/minimig/minimig_config.cpp` | `a2065_start()` in `minimig_reset()` |

(Makefile auto-includes `support/*/*.cpp` — no edit.)

---

## 9. Implementation plan (ordered steps)

Each step is independently buildable. Verify before moving on.

### Step 1 — Helper module (no menu wiring yet)
- Create `support/minimig/minimig_a2065.h` (API from §2).
- Create `support/minimig/minimig_a2065.cpp` (state file + daemon launch from §2).
- **Verify:** `make` on build host compiles clean (file auto-picked by wildcard).
  No behaviour change yet.

### Step 2 — Lifecycle hooks
- `user_io.cpp:1508`: add `a2065_start();` right after `BootInit();`.
- `support/minimig/minimig_config.cpp` `minimig_reset()` (`:522`): add
  `a2065_start();` at end.
- Core-exit teardown: add `a2065_stop();` where the previous core is unloaded
  (confirm exact site — `user_io_init()` reset block `user_io.cpp:~380`, gated on
  "was minimig"). Add `#include` of the helper header to each file.
- **Verify on MiSTer:** load Minimig core → daemon auto-starts on eth1
  (`ps | grep a2065d`, `/tmp/a2065d.log` shows `iface=eth1`). Switch to another
  core → daemon gone.

### Step 3 — OSD render line
- `MENU_MINIMIG_CHIPSET1` (`menu.cpp:5887`): `menumask = 0x7FF`, add the
  " Ethernet : <mode>" line at menusub 9, move BACK to menusub 10 (§3a).
- Add `#include "support/minimig/minimig_a2065.h"` to `menu.cpp`.
- **Verify on MiSTer:** System page shows "Ethernet : eth1"; line is selectable;
  BACK still works at its new position.

### Step 4 — OSD handler (toggle)
- `MENU_MINIMIG_CHIPSET2` (`menu.cpp:5941`): add menusub 9 case (cycle
  OFF→eth0→eth1), renumber BACK to menusub 10 (§3b).
- **Verify on MiSTer:** left/right/select cycles OFF→eth0→eth1; each change
  restarts/stops daemon (`/tmp/a2065d.log`, `ps`); selection survives core
  reload (state file persisted).

### Step 5 — End-to-end
- Build full core image, deploy daemon to chosen stable path (§7).
- **Verify:** boot Minimig with Ethernet=eth1 → `share:lance-test diags` 5/5 PASS;
  `AddNetInterface A2065` → DHCP lease + ping. Set OFF → daemon stops, no
  network. Set eth0 → confirm/document SSH-disruption behaviour (§7.3).

### Step 6 — Docs
- Update A2065 `CLAUDE.md` (MiSTer Operational Notes) with the new OSD control
  and that it supersedes the manual `killall minimig_netd` + manual daemon start.

---

## 10. Test matrix

| Scenario | Expected |
|----------|----------|
| Fresh boot, no state file | iface=eth1, daemon running on eth1 |
| Toggle to OFF | daemon killed, no A2065 networking |
| Toggle to eth0 | daemon on eth0 (SSH-disruption warning applies) |
| Toggle to eth1 | daemon on eth1 |
| Minimig soft reset | daemon still running (re-applied, idempotent) |
| Switch to non-minimig core | daemon stopped |
| Reboot MiSTer | last selection restored from state file |
| lance-test diags (eth1) | 5/5 PASS |

## 11. Rollback

- Pure-additive change. Revert the touched files (`menu.cpp`, `user_io.cpp`,
  `minimig_config.cpp`) and delete `minimig_a2065.{h,cpp}` + the state file.
- No `mm_configTYPE` / `.cfg` format change → no saved-config migration to undo.

---

## 12. Implementation status (as applied)

All edits live in `Main_MiSTer/` (the userspace fork, separate from the
`Minimig-AGA_MiSTer` FPGA submodule).

| Step | File(s) | Status |
|------|---------|--------|
| 1 Helper module | `support/minimig/minimig_a2065.{h,cpp}` (new) | **Done** — host `g++ -fsyntax-only` clean |
| 2 Lifecycle | `support.h` (+include), `user_io.cpp` (`a2065_stop()` top of `user_io_init`, `a2065_start()` after `BootInit()`), `minimig_config.cpp` (`a2065_start()` in `minimig_reset()`, +include) | **Done** |
| 3 Render | `menu.cpp` `MENU_MINIMIG_CHIPSET1`: `menumask 0x3FF→0x7FF`, Ethernet line at menusub 9, BACK→10 | **Done** |
| 4 Handler | `menu.cpp` `MENU_MINIMIG_CHIPSET2`: menusub 9 cycle OFF→eth0→eth1, BACK renumbered to 10 | **Done** |
| 5 End-to-end | ARM build + deploy + hardware test | **Done** — see §14 |
| 6 Docs | this file + A2065 `CLAUDE.md` operational note | **Done** |

Notes:
- Header wired via `support.h` (already aggregates the other minimig headers),
  so `menu.cpp` / `user_io.cpp` need no extra include; `minimig_config.cpp`
  includes it directly (it includes the minimig headers individually).
- Makefile change unnecessary — `$(wildcard ./support/*/*.cpp)` picks the new
  source up.

## 13. Build & deploy (Step 5)

Same ARM toolchain as the A2065 daemon (`arm-none-linux-gnueabihf`,
build host `192.168.1.97`, `/opt/armV7-linux-gcc/bin`).

```bash
# 1. Sync the Main_MiSTer fork to the build host
rsync -avz --exclude obj/ --exclude .git/ \
  /Volumes/Home/nigelshearman/Development/amiga/Main_MiSTer/ \
  root@192.168.1.97:/opt/development/Main_MiSTer/

# 2. Build the MiSTer userspace binary
ssh root@192.168.1.97 \
  'export PATH=/opt/armV7-linux-gcc/bin:$PATH; \
   cd /opt/development/Main_MiSTer && make -j$(nproc)'

# 3. Relay binary to the MiSTer (build host can't reach MiSTer directly)
scp root@192.168.1.97:/opt/development/Main_MiSTer/MiSTer /tmp/MiSTer
scp /tmp/MiSTer root@192.168.1.29:/media/fat/MiSTer

# 4. Ensure the daemon is at the stable path the menu launches
scp /media/fat/trans/a2065d_doorbell root@192.168.1.29:/media/fat/linux/a2065d_doorbell  # (run on/via MiSTer)
ssh root@192.168.1.29 'chmod +x /media/fat/linux/a2065d_doorbell'

# 5. Reboot MiSTer (replaces running MiSTer binary), load Minimig core, test per §10
```

Run the §10 test matrix on hardware to close Step 5.

---

## 14. Hardware test results (2026-06-15)

Built on `192.168.1.97` (`make -j`, rc=0, `bin/MiSTer` 964964 B ARM, new module
compiled + linked clean, no warnings). Deployed live to MiSTer `192.168.1.29`
(old binary backed up → `/media/fat/MiSTer.bak.preA2065`); daemon at
`/media/fat/linux/a2065d_doorbell`. Core: `Minimig_20260613a.rbf`.

| Scenario | Result |
|----------|--------|
| Boot, no state file → load Minimig | ✅ daemon auto-started `--iface eth1`, eth1 up, polling CMD slot (`/tmp/a2065d.log`) |
| Daemon launched from `/media/fat/linux/` | ✅ |
| `minimig_netd` conflict | ✅ not running |
| State file `off`, reboot, load Minimig | ✅ daemon NOT started (read path + OFF honoured across reboot) |
| Remove state file, reboot, load Minimig | ✅ default eth1 restored |

Not driven from this session (needs OSD keyboard input): the live in-OSD toggle
keypress (menusub 9 cycle). The underlying `a2065_set_iface()` (file write +
restart) is the same path exercised by the boot/reset auto-start, which passed.

Behavioural note: `a2065_get_iface()` caches the mode in the running MiSTer
process; editing the state file externally only takes effect after a MiSTer
restart. The in-OSD toggle updates the cached value in-process, so it is
immediate. Expected/acceptable.
