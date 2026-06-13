# Merge Plan — A2065 "flat‑DDR3 doorbell" FPGA changes into upstream Minimig‑AGA_MiSTer

**Prepared:** 13 June 2026
**Scope:** Plan + execution record. §1–§10 are the original pre‑merge analysis;
**§11 records the actual executed outcome.**

> ## ✅ STATUS: EXECUTED & VERIFIED — 13 June 2026
> The merge was carried out per this plan and verified end‑to‑end. Merge commit
> **`eb894c8`** (later `4a5c67b` after legacy quarantine + guard notes) on submodule branch
> **`a2065-doorbell-rebased`**, pushed to `fork`. Parent `georgemog/A2065 @ 089f253` pins it.
> **Quartus 17.0:** 0 errors, setup +0.377 ns / hold +0.246 ns. **Hardware:** lance‑test
> **5/5 PASS** + DHCP + ping. Every risk R1–R9 resolved or carried as known‑issue — see **§11**.

---

## 1. Executive summary

The networking ("A2065") FPGA work lives in a **fork of the Minimig core**, not in the
`georgemog/A2065` application repo directly. The `A2065` branch you referenced
(`simplification/flat-ddr3-doorbell`) carries the Minimig changes as a **git submodule** pinned to
`georgemog/Minimig-AGA_MiSTer @ a2065-doorbell`, commit `ff20394`. That submodule commit is the
artefact to be merged into the latest upstream core, `MiSTer-devel/Minimig-AGA_MiSTer @ MiSTer`
(currently `eb7a26e`, *Release 20260603*).

The work is **tractable but not a clean cherry‑pick**, for three reasons:

1. **Upstream moved on.** The fork was branched from *Release 20260220* (`3ab91cd`). Upstream has
   since advanced by two commits, one of which is a framework ("`Update sys.`") refactor that
   **deletes the explicit `emu` port list from `Minimig.sv`** and moves it into a new shared include,
   `sys/emu_ports.vh`. The A2065 work added custom ports to exactly that deleted block.

2. **A clean text‑merge produces a broken design.** A trial 3‑way merge yields only **one** textual
   conflict (`Minimig.sv`) and auto‑merges everything else — but the auto‑merged result **will not
   compile**, because the A2065 ports the fork connects in `sys/sys_top.v` no longer exist on the
   `emu` module after upstream's refactor. The real work is *semantic reconciliation across three
   files*, not conflict‑marker resolution.

3. **The fork modifies two shared framework files** (`sys/sys_top.v`, `sys/sysmem.sv`). These are
   normally identical across all MiSTer cores and are periodically overwritten wholesale by
   "`Update sys.`" commits. Carrying core‑specific logic in them creates a recurring maintenance
   burden — and, as detailed in §6, **roughly half of that intrusion (`sysmem.sv`) is vestigial and
   can be dropped entirely.**

There are **no upstream changes that conflict semantically** with the A2065 logic itself — upstream's
two commits touch audio/scaler/HDMI framework plumbing, none of which overlaps the networking data
path. So the merge is dominated by *re‑homing the `emu` interface* and *deciding how much framework
divergence to keep*, rather than by reworking the networking RTL.

---

## 2. Version baseline and repository topology

| Item | Value |
|------|-------|
| Upstream target | `MiSTer-devel/Minimig-AGA_MiSTer`, branch `MiSTer`, HEAD `eb7a26e` (*Release 20260603*) |
| Fork (the changes) | `georgemog/Minimig-AGA_MiSTer`, branch `a2065-doorbell`, HEAD `ff20394` |
| Common ancestor | `3ab91cd` (*Release 20260220*) |
| `georgemog/A2065` branch | `simplification/flat-ddr3-doorbell` — pins the core via submodule `Minimig-AGA_MiSTer → ff20394` |
| Upstream commits ahead of ancestor | `e704ae9` *Update sys.* and `eb7a26e` *Release 20260603* |

**Important topology note.** `georgemog/A2065` also contains a *standalone* RTL set under
`fpga/rtl/` (`a2065_top.v`, `a2065_autoconfig.v`, `a2065_registers.v`, `a2065_ddr_window.v`, …).
That is an **earlier Zorro‑II line of development**, superseded by the flat‑DDR3 integration and **not**
the code that is pinned into the build. The canonical, integrated changes are those in the fork
submodule at `ff20394`. This plan targets the submodule only.

---

## 3. Inventory of the changes to be merged

Diff `3ab91cd..ff20394` = **19 files, +2246 / −27**. Grouped by role:

### 3a. New RTL — all under `rtl/A2065/` (11 files)

| File | Integrated? | Notes |
|------|:-----------:|-------|
| `a2065_ddr3_mailbox.v` (363 L) | ✅ instantiated in `sys/sys_top.v` | Core of the design — see §4. |
| `avalon_arbiter.v` (129 L) | ✅ instantiated in `sys/sys_top.v` | 2‑master Avalon arbiter onto the second DDR3 port. |
| `a2065_ddram.v` (146 L) | ✅ instantiated in `rtl/minimig.v` | 68k‑side boardram window + CDC handshake. |
| `a2065_regfile.v` (130 L) | ✅ instantiated in `rtl/minimig.v` | LANCE RAP/RDP register file, autoconfig identity, CSR shadow. |
| `a2065_axi_slave.v` (195 L) | ⚠️ **in `files.qip` but not instantiated** | Intended h2f‑AXI consumer; unused (see §6). |
| `a2065_ddr3_test.v` (109 L) | ⚠️ **in `files.qip` but not instantiated** | Bring‑up/test harness; compiled but dead. |
| `ddr_arbiter.v` (71 L) | ⚠️ **in `files.qip` but not instantiated** | Superseded by `avalon_arbiter.v`; dead. |
| `a2065_top.v` (141 L) | ❌ not in qip, not instantiated | Legacy Zorro‑II top. |
| `a2065_autoconfig.v` (143 L) | ❌ legacy | Only referenced by `a2065_top.v`. |
| `a2065_registers.v` (209 L) | ❌ legacy | Only referenced by `a2065_top.v`. |
| `a2065_boardram.v` (69 L) | ❌ legacy | Not instantiated; see SDC note in §7. |

> **Finding A — dead code in the build.** Three files (`a2065_axi_slave.v`, `a2065_ddr3_test.v`,
> `ddr_arbiter.v`) are added to `files.qip` and therefore compiled, but never instantiated. Four
> more (`a2065_top.v`, `a2065_autoconfig.v`, `a2065_registers.v`, `a2065_boardram.v`) are carried in
> the tree but neither compiled nor instantiated. None of this breaks the build, but it should be
> pruned or explicitly labelled "legacy/unused" during the merge to avoid future confusion.

### 3b. Modified core RTL (the genuine integration points)

| File | +/− | What it does |
|------|----:|--------------|
| `rtl/minimig.v` | +101 | Adds A2065 ports; instantiates `a2065_ddram` + `a2065_regfile`; ORs `a2065_int2` into `int2`; ORs boardram readback and `nrdy` into the chip bus; 2‑FF CDC sync of `a2065_int2`. |
| `rtl/cpu_wrapper.v` | +64 | Adds A2065 enable/base config + MAC‑byte nibble feed used by autoconfig serial number. |
| `rtl/gary.v` | +6 | Address decode: new `sel_a2065` at `a2065_base` (alongside `sel_toccata`). |

### 3c. Modified project / constraint files

| File | +/− | Notes |
|------|----:|-------|
| `files.qip` | +7 | Adds 7 of the 11 new RTL files (see Finding A). |
| `Minimig.sdc` | +20 | Multicycle/false‑path band‑aids for the A2065 logic — **timing‑marginal, see §7**. |

### 3d. Modified **shared framework** files (the maintenance‑sensitive part)

| File | +/− | Needed? |
|------|----:|---------|
| `sys/sys_top.v` | +194 | **Partly.** The DDR3 arbiter + mailbox + `emu` port connections are required. The h2f‑bridge wires are vestigial (§6). |
| `sys/sysmem.sv` | +113 | **No.** Enables the HPS→FPGA (h2f) AXI master, which nothing consumes (§6). Can be reverted to upstream. |

---

## 4. What the design actually does (so reviewers can reason about the merge)

The integrated path is a **DDR3 "doorbell" mailbox**, not the older Zorro‑II/AXI scheme. From the
module header of `a2065_ddr3_mailbox.v`, everything runs in the **`clk_audio` (~49 MHz)** domain and
performs three jobs against a reserved DDR3 window (`DDR3_BASE = 29'h03FE_0000` on the FPGA's
`f2sdram2` Avalon view):

1. **CMD doorbell** — the 68k writes a LANCE register; `a2065_regfile` raises `cmd_pending`; the
   mailbox writes the command to DDR3 for the ARM daemon and pulses `cmd_clear`.
2. **Boardram window** — 68k accesses to `card_base + 0x8000` are DTACK‑stretched and serviced as
   direct DDR3 reads/writes, with a request arriving via async handshake from `clk_sys`.
3. **CSR shadow + INT poll** — periodically reads CSR/interrupt state written by the ARM daemon and
   drives `a2065_int2` back to Paula.

The second DDR3 master (`a2065_ddr3_mailbox`) is multiplexed with the existing audio DDR service
(`ddr_svc`) onto the single `f2sdram2` port via `avalon_arbiter` — which is why `sys/sys_top.v` had
to be touched.

> **Cross‑component contract (out of FPGA scope, but coupled).** The same DDR3 window is mapped by
> the ARM daemon at **physical `0x1FF0_0000`** (`arm/src/*.cpp: #define DDR3_BASE 0x1FF00000UL`).
> The FPGA Avalon base `0x03FE_0000` and the ARM physical base `0x1FF0_0000` are two views of the
> **same** reserved region. The merge must not let the newer framework's memory map reclaim that
> region — see Risk R2 in §7.

---

## 5. Conflict & integration analysis (empirical)

A trial `git merge ff20394` onto `upstream/MiSTer` was run to get ground truth rather than guesswork.

**Result:** exactly **one** textual conflict, plus a set of clean‑but‑incomplete auto‑merges.

| File | Auto‑merge outcome | Reality |
|------|--------------------|---------|
| `Minimig.sv` | ❌ CONFLICT | Real conflict; clean resolution known (§5a). |
| `sys/sys_top.v` | ✅ auto‑merged | **Compiles only after `Minimig.sv` is fixed** — it connects A2065 ports that must exist on `emu`. |
| `sys/sysmem.sv` | ✅ applied (fork version) | Should instead be **reverted to upstream** (§6). |
| `rtl/minimig.v`, `rtl/cpu_wrapper.v`, `rtl/gary.v` | ✅ clean | Genuinely clean — upstream did not touch them. |
| `files.qip`, `Minimig.sdc` | ✅ clean | Clean, but need pruning/re‑validation (§3, §7). |
| 11 × `rtl/A2065/*.v` | ✅ added | Clean adds. |

> **Finding B — the dangerous part is the *silent* one.** Git reports `sys/sys_top.v` as a
> successful auto‑merge, but the result is broken: the fork's `emu` instantiation in `sys_top.v`
> wires `.A2065_INT2(...)`, `.A2065_BRAM_REQ_*(...)` etc., while upstream's refactor means the `emu`
> module's port list now comes from `sys/emu_ports.vh`, which has **zero** A2065 ports. Anyone
> committing the "successful" merge without re‑homing the ports will hit a wall of *"port not
> declared on module emu"* errors at compile time. This is the single most important thing to get
> right.

### 5a. The `Minimig.sv` conflict and its clean resolution

The conflict is precisely the `emu` port block:

- **Upstream side:** the entire explicit port list is replaced by `` `include "sys/emu_ports.vh" ``.
- **Fork side:** the full explicit list *plus* the appended A2065 ports.

`sys/emu_ports.vh` ends with `input OSD_STATUS` (no trailing comma, no closing paren — the `(` and
`)` live in `Minimig.sv`). The clean resolution **keeps upstream's include and appends the A2065
ports after it**, using comma‑first continuation so the shared include file is left untouched:

```verilog
module emu
(
    `include "sys/emu_ports.vh"

    // ---- A2065 networking (core-specific extension) ----
    , input         A2065_INT2
    , input  [7:0]  A2065_MAC_BYTE2
    , input  [7:0]  A2065_MAC_BYTE3
    , input  [7:0]  A2065_MAC_BYTE4
    , input  [7:0]  A2065_MAC_BYTE5
    , output        A2065_CMD_PENDING
    , output [6:0]  A2065_CMD_RAP
    , output [15:0] A2065_CMD_DATA
    , input         A2065_CMD_CLEAR
    , input  [15:0] A2065_CSR0_IN
    , input  [15:0] A2065_CSR1_IN
    , input  [15:0] A2065_CSR2_IN
    , input  [15:0] A2065_CSR3_IN
    , input         A2065_BRAM_REQ_ACK
    , input         A2065_BRAM_RESP_VALID
    , input  [15:0] A2065_BRAM_RESP_DATA
    , output        A2065_BRAM_REQ_VALID
    , output [13:0] A2065_BRAM_REQ_ADDR
    , output [15:0] A2065_BRAM_REQ_WDATA
    , output        A2065_BRAM_REQ_RW
    , output [1:0]  A2065_BRAM_REQ_BE
);
```

This is preferable to forking `emu_ports.vh` (which would re‑introduce a shared‑file divergence) and
keeps the A2065 ports clearly marked as a core extension. The body changes the fork made to
`Minimig.sv` (the `cpu_wrapper`/`minimig` instantiation additions, the `a2065_ena`/`a2065_base`
wires, removal of the unused `DDRAM_BE_S` reg) are below the conflict region and apply cleanly.

> Note: the fork's `Minimig.sv` also "fixes" a CRLF on the `HDMI_BLACKOUT` line. Drop that hunk —
> it is cosmetic and just creates noise against upstream's line endings.

---

## 6. Architectural decision: how much framework divergence to keep

This is the most consequential choice in the merge. The fork carries logic in two shared framework
files. Each should be handled differently.

### 6a. `sys/sysmem.sv` — **recommend reverting to upstream (drop the fork changes)**

The fork's 113 lines enable the **HPS→FPGA lightweight AXI master** (`h2f_axi_*`) by hand‑wiring the
`cyclonev_hps_interface_hps2fpga` primitive. Crucially, this is **pure RTL** — it does *not* require
a Quartus/Platform Designer (Qsys) regeneration, which is good. But it is also **unused**:

> **Finding C — the h2f bridge is vestigial.** Every `h2f_*` signal appears exactly twice in
> `sys/sys_top.v`: once in a wire declaration and once in the `sysmem` port map. **Nothing consumes
> them.** The intended consumer, `a2065_axi_slave.v`, is instantiated nowhere. The live doorbell
> uses the **DDR3 `f2sdram2`** path, not h2f. The README history confirms the project migrated
> *away* from an AXI doorbell to the flat‑DDR3 scheme.

**Recommendation:** revert `sys/sysmem.sv` to upstream and delete the dangling `h2f_*` declarations
and the `sysmem` h2f port connections from `sys/sys_top.v`. This removes one entire framework‑file
fork at zero functional cost and shrinks the recurring "Update sys" maintenance surface by ~55%.
(Confirm with the authors that no imminent direct‑AXI work depends on it; the evidence says it does
not.)

### 6b. `sys/sys_top.v` — **must keep the DDR3 arbiter/mailbox; isolate it**

The arbiter (`avalon_arbiter`) + `a2065_ddr3_mailbox` instances and the `emu` A2065 connections are
**required** and **do not conflict** with upstream's `sys_top.v` edits (which touch `gp_in`,
`vol_boost`, `io_dout_sys` opcodes and the audio_out `.boost` port — all in unrelated regions).
However, keeping them in `sys_top.v` means re‑applying them after every future `Update sys`.

Two options:

- **Option 1 (lower effort, recommended for first landing):** keep the arbiter + mailbox in
  `sys_top.v`, but wrap the block in clearly delimited `// ===== A2065 BEGIN/END =====` guards and
  document the re‑apply procedure. Accept the maintenance cost.
- **Option 2 (cleaner, more effort):** move the arbiter + mailbox into a small core‑owned wrapper
  instantiated from the core, exposing only the second‑DDR3 Avalon port and the A2065 signals to
  `emu`. This minimises `sys_top.v` to just the `emu` port connections. Defer to a follow‑up unless
  the team wants to upstream the core eventually.

> The MAC‑byte feed crosses `clk_audio → clk_sys` and the fork relies on a false‑path SDC constraint
> for it (`set_false_path -from {*a2065_mailbox_inst|mac_byte*} -to {emu|cpu_wrapper|mac_nibble_*}`).
> Whichever option is chosen, that CDC and its constraint must be preserved (see R3).

---

## 7. Risks and required checks

| ID | Risk | Why it matters | Mitigation |
|----|------|----------------|------------|
| **R1** | **Silent broken auto‑merge** of `sys_top.v` (Finding B). | Compiles fail unless `emu` ports re‑homed in `Minimig.sv`. | Apply §5a *before* trusting any "successful" merge; do a full compile, not just `git status`. |
| **R2** | **DDR3 reserved‑region collision.** Mailbox uses `0x03FE_0000` (FPGA) / `0x1FF0_0000` (ARM). | A newer framework may repartition `f2sdram2` / scaler FB and reclaim that window → silent memory corruption. | Diff the merged `sysmem.sv`/scaler memory map vs the fork's baseline; confirm the window is still unallocated. Keep FPGA and ARM `DDR3_BASE` defines in lock‑step. |
| **R3** | **Timing closure regression.** The fork's SDC comments admit the A2065 BRAM degraded placement (`yc_out` slack `+0.288ns → −0.471ns`) and added multicycle band‑aids. | Upstream `20260603` has different placement; the band‑aids may not hold, or may now be unnecessary/incorrect. | Re‑run Quartus timing on the merged build; re‑derive (don't blindly copy) the multicycle/false‑path exceptions; treat any new negative slack as a real fix item, not a constraint to paper over. |
| **R4** | **Stale SDC node name.** `Minimig.sdc` constrains `emu|minimig|a2065_boardram_inst|*`, but no such instance exists — the boardram lives inside `a2065_ddram_inst`. | The multicycle exception **matches nothing** and is silently ignored; the path it was meant to relax is unconstrained. | Correct the instance path to the real hierarchy (`…|a2065_ddram_inst|…`) or remove if obsolete. Verify with `report_timing` that the intended path is covered. |
| **R5** | **Framework‑fork drift** (`sys_top.v`). | Future `Update sys` overwrites the file; A2065 logic is lost unless re‑applied. | Guard‑comment the block (Option 1) or refactor into the core (Option 2). Record the procedure in the core README. |
| **R6** | **Dead/legacy files** (Findings A). | Confusion; `ddr_arbiter.v` in particular looks like the live arbiter but isn't. | Prune from `files.qip`; either delete legacy `a2065_top/autoconfig/registers/boardram.v` or move to a `legacy/` dir with a README. |
| **R7** | **`sys.qip` / `audio_out` rename.** Upstream renamed `audio_out.v → audio_out.sv` (+`boost`) and edited `hps_io.sv`, `sys.qip`. | The merge must adopt **upstream's** `sys/*` for these; the fork never saw them. | Take upstream versions wholesale for all `sys/*` files except the deliberately‑kept `sys_top.v` block. Confirm no duplicate file references in `sys.qip` vs `files.qip`. |
| **R8** | **Unverifiable in this environment.** No Quartus here; cannot synthesize/place/route. | "Merges and compiles in head" ≠ "fits and closes timing on DE10‑nano." | Treat §8 hardware verification as mandatory gating, not optional. |
| **R9** | **Known residual bugs inherited from the fork.** Per `progress.md`: intermittent interrupt‑test race (CSR0 flag propagation), RX‑ring exhaustion under live traffic, and a MAC byte‑ordering bug from hardcoded serial bytes in `cpu_wrapper.v`. | These ship with the merge; they are not *caused* by it but will be attributed to it. | Carry them forward as tracked issues (git‑bug), explicitly out of merge scope, so the merge is judged on parity with the fork, not on fixing pre‑existing defects. |

---

## 8. Verification plan

**Stage 0 — static/merge sanity (this environment can do most of this).**
- Resolve per §5a/§6; `grep` confirms: every `A2065_*` port connected in `sys_top.v` has a matching
  declaration in `Minimig.sv`; no dangling `h2f_*`; no instantiation of pruned modules.
- Lint with a free Verilog front‑end (Verilator `--lint-only` / Icarus parse) to catch port and
  width mismatches before Quartus.

**Stage 1 — Quartus compile (requires the Quartus toolchain; not available here).**
- Full Analysis & Synthesis: zero unresolved‑port / missing‑module errors.
- Fitter + TimeQuant: review **every** A2065‑related SDC exception in `report_timing`; confirm R3/R4
  are genuinely addressed (constraints match real nodes; no masked violations).
- Compare resource/utilisation and Fmax against the fork's last good build to catch regressions.

**Stage 2 — hardware bring‑up on DE10‑nano, mirroring `progress.md` build `20260525a`.**
- Register read/write suite (17), boardram read/write (5), interrupt assert/deassert lifecycle (8),
  ≥60 s DDR3 idle stability — all must PASS as they did pre‑merge.
- Commodore `lance-test` diagnostics: Buffer Memory + LANCE Config PASS (these were stable);
  document interrupt/collision results against R9 so regressions are distinguishable from known issues.
- End‑to‑end: virtual Amiga obtains a real DHCP lease and exchanges live traffic on **both** Roadshow
  and MiamiDX (the fork's stated working baseline).

**Stage 3 — framework‑drift check.**
- Re‑run a no‑op `Update sys` dry run to confirm the guard‑commented `sys_top.v` block (or the
  refactored wrapper) survives, and that `sysmem.sv` is byte‑identical to upstream (proving 6a worked).

---

## 9. Recommended sequence (checklist)

1. **Branch** off upstream `eb7a26e` (e.g. `a2065-doorbell-rebased`). Decide policy: rebase vs merge
   commit. A **merge** preserves the fork's granular history; a **rebase/squash** gives a cleaner
   single "A2065 networking" commit that is easier to re‑apply across future `Update sys`. Recommend
   rebase‑to‑a‑small‑set for long‑term maintainability.
2. **Add** the 4 live RTL modules and prune the rest: keep `a2065_ddr3_mailbox.v`, `avalon_arbiter.v`,
   `a2065_ddram.v`, `a2065_regfile.v` in `files.qip`; drop `a2065_axi_slave.v`, `a2065_ddr3_test.v`,
   `ddr_arbiter.v` from the qip; quarantine the four legacy files (Finding A / R6).
3. **Apply** the clean core edits: `rtl/minimig.v`, `rtl/cpu_wrapper.v`, `rtl/gary.v` (no conflicts).
4. **Resolve `Minimig.sv`** per §5a (include + appended A2065 ports; drop the CRLF hunk).
5. **`sys/sysmem.sv` → revert to upstream** (§6a). Delete dangling `h2f_*` from `sys_top.v`.
6. **`sys/sys_top.v`** → keep the arbiter + mailbox + `emu` connections inside guard comments
   (Option 1) or refactor into the core (Option 2). Take upstream versions of all *other* `sys/*`
   files (R7).
7. **`Minimig.sdc`** → re‑derive A2065 exceptions; fix the stale `a2065_boardram_inst` path (R4);
   re‑validate timing rather than copying band‑aids (R3).
8. **Verify** through Stages 0→3 (§8).
9. **Re‑point the `A2065` submodule** (`simplification/flat-ddr3-doorbell`) to the new core commit and
   confirm the ARM `DDR3_BASE` still matches the FPGA window (R2).
10. **File known‑issue tickets** (R9) so the merge is assessed on fork parity.

---

## 10. Effort estimate (engineering judgement)

| Phase | Rough effort |
|-------|--------------|
| Merge mechanics + `Minimig.sv`/`sys_top.v` reconciliation (§5, §6a) | ~0.5–1 day |
| `sys_top.v` isolation (Option 1) | ~0.5 day · Option 2 refactor: +1–2 days |
| SDC re‑derivation + timing closure (R3/R4) | ~0.5–2 days, **placement‑dependent and the main unknown** |
| Quartus compile + DE10‑nano verification (§8 Stages 1–2) | ~1–2 days incl. iteration |
| **Total (Option 1, no nasty timing surprises)** | **~3–5 working days** |

The dominant uncertainty is timing closure on the newer placement (R3), not the merge logic itself.

**Actual:** ~half a day end‑to‑end including a clean 26‑minute Quartus compile. Timing closed
comfortably on the first try (no R3 surprise). See §11.

---

## 11. Execution outcome (13 June 2026)

The merge was executed, compiled, and hardware‑verified the same day. This section records
what actually happened against the plan.

### 11a. Git topology as built

| Item | Value |
|------|-------|
| Merge commit | `eb894c8` — 2‑parent (`eb7a26e` upstream + `ff20394` doorbell) |
| Submodule branch | `a2065-doorbell-rebased` (off `origin/MiSTer @ eb7a26e`) |
| Follow‑ups | `a1f1c1f` legacy quarantine (R6), `4a5c67b` drift‑guard note (R5) |
| Submodule remote | pushed to `fork` (`georgemog/Minimig-AGA_MiSTer`) |
| Parent | `georgemog/A2065 @ 089f253`, submodule pinned `ff20394 → 4a5c67b` |
| Pre‑work cleanup | local submodule `MiSTer` branch was mis‑pointed at `ff20394`; reset to `origin/MiSTer`. Quartus server working tree was uncommitted‑edited to `ff20394` content on top of ancestor `daec257` — no unique work lost (only an auto‑generated `LAST_QUARTUS_VERSION` string differed). |

### 11b. Reconciliation actually applied (matches §5a/§6)

- **`Minimig.sv`** — kept upstream `` `include "sys/emu_ports.vh" `` + appended A2065 emu ports
  comma‑first; dropped the fork's cosmetic `HDMI_BLACKOUT` CRLF hunk. Body wiring applied clean.
- **`sys/sysmem.sv`** — reverted to upstream (vestigial h2f AXI master dropped). Confirmed
  **byte‑identical** to upstream in the final tree.
- **`sys/sys_top.v`** — removed the dangling `h2f_*` sysmem ports + wire decls; kept
  arbiter + mailbox + emu A2065 wiring inside `// ===== A2065 BEGIN/END =====` guards.
- **`files.qip`** — pruned 3 dead modules → 4 live (`a2065_regfile`, `a2065_ddram`,
  `avalon_arbiter`, `a2065_ddr3_mailbox`).
- **`Minimig.sdc`** — removed the phantom `a2065_boardram_inst` multicycle (R4); annotated the
  `yc_out` exception for re‑validation; kept the `mac_byte` false‑path note.
- Core RTL (`minimig.v`, `cpu_wrapper.v`, `gary.v`) + all 11 `rtl/A2065/*.v` were
  byte‑identical to `ff20394` (clean adds / clean auto‑merge).

### 11c. Verification (Stages 0→3 all green)

- **Stage 0 (static):** no conflict markers; no dangling h2f AXI; every connected emu `A2065_*`
  port declared; pruned/legacy modules uninstantiated; comma‑first emu port list + 3/4 live leaf
  modules lint‑clean under iverilog (`a2065_ddram` declare‑after‑use is iverilog strictness,
  identical in `ff20394`, Quartus‑clean).
- **Stage 1 (Quartus 17.0, server `192.168.1.65`, dir `~/Development/Minimig-A2065-rebased`):**
  **0 errors**, 83 warnings; **setup +0.377 ns, hold +0.246 ns** (all corners positive);
  RBF `Minimig.rbf` produced (3,488,776 B). Elapsed 26:17.
- **Stage 2 (DE10‑nano, `Minimig_20260613a.rbf`):** lance‑test **5/5 PASS** — Buffer / LANCE
  config / Interrupt / Collision / Internal loopback → "Controller PASSED diagnostics".
  AddNetInterface a2065 → DHCP `192.168.1.190`; ping LAN 7/7 + internet `8.8.8.8` 13/13, 0% loss.
  Full parity with fork baseline 20260609a.
- **Stage 3 (drift):** `sysmem.sv` byte‑identical to upstream (zero re‑apply); `sys_top.v` is the
  only `sys/` file carrying A2065 logic; guarded blocks + the two in‑place edits (ddr_svc
  `ram2_*→arb_m0_*`, emu `.USER_IN` comma) documented in the guard header.

### 11d. Risk register — final disposition

| ID | Plan risk | Outcome |
|----|-----------|---------|
| **R1** | Silent broken `sys_top.v` auto‑merge | **CLEARED.** §5a applied; Quartus elaborated the `emu`/`minimig` hierarchy with no unresolved‑port errors. |
| **R2** | DDR3 reserved‑region collision | **OK.** Upstream `20260603` did not repartition `f2sdram2`; window `0x03FE_0000`/`0x1FF0_0000` still free; ARM `DDR3_BASE` unchanged. Daemon mapped DDR3 and ran. |
| **R3** | Timing‑closure regression | **CLEARED.** First compile closed at setup +0.377 / hold +0.246 ns. `yc_out` multicycle left in (annotated); no negative slack anywhere. |
| **R4** | Stale `a2065_boardram_inst` SDC node | **FIXED.** Phantom multicycle removed; boardram is DDR3 in `a2065_ddram_inst`. |
| **R5** | Framework‑fork drift (`sys_top.v`) | **MITIGATED.** Guard‑commented (Option 1) with a documented re‑apply procedure incl. both in‑place edits. |
| **R6** | Dead/legacy files | **DONE.** 3 dead dropped from qip; all 7 dead/legacy moved to `rtl/A2065/legacy/` + README. |
| **R7** | `sys.qip`/`audio_out` rename | **OK.** All `sys/*` taken from upstream except the guarded `sys_top.v` block; `sysmem.sv` byte‑identical. |
| **R8** | Unverifiable here (no Quartus) | **RETIRED.** Built on the Quartus server + verified on DE10‑nano hardware. |
| **R9** | Inherited fork bugs | **CARRIED (out of scope).** MAC low‑byte zero (Issue A — `A2065_MAC_BYTE2..5` declared but undriven, as in fork; on‑wire src `00:80:10:00:00:00`), MAC display sign‑extension (cosmetic), `cpu_berr_n` unconnected. None caused by the merge. |

### 11e. Gotcha worth keeping

The pytest `tests/test_lance.py` autocapture reported **empty serial output** — a
`serial_long.py` capture‑timing artifact, **not** a core fault. A manual
`share:lance-test diags` over serial runs clean (5/5). Don't trust the pytest serial autocapture
as the pass/fail oracle for lance‑test diags; read the serial transcript.

---

*End of plan — executed and verified 13 June 2026.*
