# Multi-disc titles — design proposal

Status: design proposal. No code committed against this doc yet.

One launcher entry, one binary, N discs. What it takes for psxrecomp and
recomp-ui to ship a title like Final Fantasy VII — boot from any disc, swap
mid-game, one memory card across all three.

Multi-disc is **foundation work, not enhancement work**, so the strict readings
of `CLAUDE.md` apply throughout: no per-game hacks, no stubs, oracle first,
unknown rather than guessed.

---

## 1. Two axes, not one

"Multi-disc" sounds like a file-path problem. It is two independent problems,
and a multi-disc title exercises both.

**Axis A — data: which image is mounted.** Swapping the ISO under a running
guest, faithfully: the lid opens, the motor stops, the drive reports
shell-open, and a new TOC appears on close. This is CD-ROM hardware behaviour
the runtime does not model at all today.

**Axis B — code: which program is running.** Each FF7 disc carries its own boot
executable and serial (US: SCUS-94163 / 94164 / 94165). A save loaded from disc
3 must run *disc 3's* recompiled program. One binary therefore has to link and
select between N statically recompiled programs.

Axis B is the load-bearing one, and it is required whether or not a mid-game
swap re-executes anything: the player who inserts disc 3 and hits Continue is
booting a different program. Axis A is what makes the in-game "Insert Disc 2"
screen actually proceed.

---

## 2. What the tree already gives us

Read before designing. Several pieces are further along than they look, and one
is entirely absent.

| Mechanism | State today | Where | Cost to us |
|-----------|-------------|-------|------------|
| `discs = [...]` config key | Already parsed into `std::vector<fs::path>`, absolutized against the project root. `disc = "..."` is the single-value fallback. | `recompiler/src/config_loader.cpp:1217-1225`, declared `config_loader.h:742` | extend |
| First disc wins | `if (!gc.discs.empty()) resolved_disc = gc.discs.front();` is the **sole** consumer of `gc.discs` in the runtime. The rest of the vector is dropped. | `runtime/src/main.cpp:10931` | extend |
| BIOS backend registry | Two recompiled BIOS images already coexist in one binary: every symbol stem-prefixed, one exported `PsxBiosBackend` descriptor each, a generated registry, and unprefixed forwarders that left ~66 runtime call sites untouched. This is the exact pattern Axis B needs. | `runtime/include/psx_bios_backend.h:35-61`, `runtime/src/psx_bios_backend.c:25-100`, descriptor emitted at `recompiler/src/full_function_emitter.cpp:2285` | precedent |
| Emitter symbol prefixing | `g_sym_prefix = out_stem + "_";` — the BIOS emitter already has the whole mechanism, including `#define psx_dispatch` shims. | `recompiler/src/full_function_emitter.cpp:2324` (decl `:69`, helpers `:73/78/85`) | precedent |
| Game generated C | Emits **unprefixed** `func_XXXXXXXX`, `psx_game_address_in_text`, `k_psx_game_dispatch`. No `g_sym_prefix` equivalent exists in the game emitter at all. Two discs' programs collide at link time. | `recompiler/src/main_psx.cpp:1470, 1477, 1590` | emitter work |
| Multi-TU game build | `GAME_GENERATED_FULL_C` is already a multi-value list (the split-TU build), consumed with `foreach`. Dispatch is still singular. | `runtime/runtime.cmake:800, 865-872` | ready |
| Re-mounting a disc | `cdrom_init()` is deliberately re-callable and already closes and re-opens the ISO handle — netplay rematch does exactly this. | `runtime/src/cdrom.c:2709, 2779-2789` | ready |
| Lid / shell state | **Absent.** `CDSTAT_SHELL` is computed in four places, all pure functions of `has_disc()`. No `shell_open`/lid/tray variable, no open transition, no latch, no INT5-while-open. The one transient poke is a debug helper, not a state machine. | `runtime/src/cdrom.c:881` (define), `:2043, 2150, 2792`; debug poke `debug_force_cd_reinsert()` `:3388-3410` | build it |
| Overlay cache | Content-addressed `cache/<game_id>/<region>_<crc>.dll`. Disc 2's overlays are new hashes in the same per-game directory — content addressing keeps that correct, so nothing to change. | `docs/overlay-recompilation-design.md:43-45` | free |
| Memory cards | Rooted per game, not per disc: one `memcard_dir`, disc-agnostic `card1.mcd`/`card2.mcd`. Save on disc 1, continue on disc 2 already works by construction. | `runtime/src/main.cpp:10932, 12073-12075` | free |
| Save states / rewind | Keyed on a single game-wide `entry_pc`, so disc 2's slots land in different files automatically. Nothing stops loading a disc-1 state while disc 3 is mounted. | `runtime/include/savestate.h:33, 39`; call sites `main.cpp:13592, 13597`, `psx_netplay.c:1175, 1192` | guard |
| recomp-ui launcher | **Not in this repo** — see the note below. `.gitmodules` pins only `lib/recomp-net` and `lib/retcomm-rbengine`. | separate repo | widen ABI (elsewhere) |
| Mod derived discs | Resolution errors when more than one derived-disc provider is active; the plan path also takes `derived_discs.front()`. Written for a world with one disc. | `runtime/src/mod_packages.cpp:3269, 3275-3276`; `runtime/src/mod_runtime.cpp:510-511` | scope it |

**recomp-ui is a sibling submodule, not part of this framework repo.** Game
repos pin `psxrecomp/` and `recomp-ui/` side by side; configure fails with a
`FATAL_ERROR` if `-DPSX_RECOMP_UI=ON` and the submodule is absent
([`BUILDING.md`](BUILDING.md)). All of P5 therefore lands in the **recomp-ui
repo**, not here, and its ABI claims below (`launcher_model.h`,
`recomp_launcher.h`) are recorded from that repo and are **not verifiable from
this tree** — re-check them there before starting P5.

**A stale claim in the schema doc, now corrected on this branch.**
[`config_schema.md`](config_schema.md) documented `discs` as a live key in its
field table while its "What's NOT in the schema yet" section still listed the
same field as unimplemented future work. The field is parsed (row 1 above), so
that second entry was false and has been removed.
`[runtime] disc_swap_command` is genuinely absent — zero hits in any source
file — and stays listed as not-yet-schema.

---

## 3. Sequenced work

Each phase names the gate that lets the next one start. The order is real: P2 is
unblockable without P1's schema, and P4 may turn out not to exist at all
depending on what P0 observes.

### P0 — Find out what actually happens on a swap

*No dependencies. No code.*

Everything downstream is shaped by facts we do not have yet. Get them from the
oracle, not from reasoning about them.

**Use Syphon Filter 2, not FF7, as the P0 subject.** `syphon-filter-2-recompiled`
is already a port in this workspace, and its README claims "Disc 1 and Disc 2
gameplay verified" while its `game.toml` names only Disc 1
(`id = "SCUS-94451"`, one `exe`, one `disc`) and the runtime mounts
`discs.front()`. Either that claim rests on a swap path nobody has written down,
or SF2 is the data-only case and disc 2 runs disc 1's program. **Which it is, is
unknown and is the single most useful thing P0 can settle** — it decides whether
the first real consumer needs the flat `discs` form or the full `[[game.disc]]`
registry, and it is answerable today against a title we have.

- Drive a real multi-disc title on `psx-beetle` to its disc-change prompt and
  capture the full CD command/response trace across the swap: which commands are
  issued, what the status byte does, whether the shell bit latches until read,
  whether `GetTN`/`GetTD` are re-issued, whether `SYSTEM.CNF` is re-read.
- Answer the decisive question for Axis B: does the title *re-execute* a boot
  executable after the swap, or does the disc-1 program keep running and just
  read new data? Watch the entry PC in `fntrace`. Do not assume — titles differ.
- Confirm Beetle's libretro disc-control path is reachable through
  `beetle_libretro.cpp`. It is the oracle for every later step, so this is a
  verification, not an assumption.
- Cross-read the swap sequence against psx-spx and the BIOS disassembly's CD
  driver.

Output: a new "§6 — Observed swap sequence" section appended to this document,
with the traces pasted in as the proof artifact, and anything unanswered listed
there as an explicit unknown.

> **Gate.** The swap sequence is recorded from the oracle and from psx-spx, not
> inferred. If a question cannot be answered, it is written down as unknown.

### P1 — A disc becomes a first-class config entry

*After P0.*

Today `discs` is a flat array of paths and the schema doc parks it as future
work. Promote it, and separate the two cases explicitly so a title declares
which one it is.

- `disc = "..."` stays as today's sugar.
- `discs = ["a.cue", "b.cue"]` is redefined to mean **one program, N images** —
  the data-only case.
- New `[[game.disc]]` array of tables means **N programs, N images**: per-disc
  `index`, `label`, `cue`, `serial`, `exe`, `entry_pc`, `load_address`,
  `stack_base`, `text_size`, `disc_crc`/`disc_sha1`, seeds, and the `[netplay]`
  TOC policy.
- Runtime: a `DiscSet` — ordered resolved mounts plus their `DiscIdentity` —
  replaces the single resolved disc. All N are resolved and identified at launch
  through the existing `resolve_disc_path` / `disc_identity` path, so a wrong or
  missing disc 3 is named at startup rather than discovered three hours in.
- Persistence: `discs.cfg` (one path per line, index-ordered), with the existing
  `disc.cfg` read as the legacy single-slot form.

> **Gate.** A three-disc `game.toml` loads, all three identities verify at
> launch, and a single-disc title boots byte-identically to today.

### P2 — A game backend registry, mirroring the BIOS one

*After P1. Largest phase.*

This is Axis B, and the design is already written — it is what
`psx_bios_backend.{h,c}` did for two BIOS images. Copy the mechanism rather than
inventing a second one.

- Give the game emitter the same symbol-prefix treatment the BIOS emitter has:
  every `func_*`, the dispatch table, and the text predicates get stem-prefixed
  with the disc's serial stem.
- Emit one exported descriptor per program, `<STEM>_psx_game_backend`, carrying
  dispatch, `address_in_text`, `text_native_ok`, `text_native_ok_full`, text
  range, entry PC, load address and serial.
- New `runtime/src/psx_game_backend.c`: the unprefixed names the runtime and
  generated C already call become thin forwarders to the active backend — the
  same trick that meant adding a second BIOS changed no call sites.
- `runtime.cmake` grows `PSXRECOMP_GAME_STEMS` and generates
  `psx_game_registry.c`, exactly like the BIOS stem list.
- Selection at launch: the mounted disc's detected serial picks the backend. A
  mounted disc with no matching backend is a loud fatal, never a fallback to
  disc 1's program.
- `game_dispatch_compat.c` gains one more compat arm, so titles generated before
  the registry keep building — the pattern that file already implements twice.

**Cost to measure, not assume — and the BIOS precedent's cost argument does
*not* transfer.** [`BIOS_SELECTION.md`](BIOS_SELECTION.md) ("Why both are
compiled in") already made and won this argument for BIOS images, but it was
affordable there for a reason that does not hold here: the game's own recompiled
code is *identical* regardless of which BIOS it was generated against, so only
the BIOS is duplicated — roughly 20 MB, colliding on just 22 symbols. Multi-disc
inverts that. The thing being duplicated is the game program itself, N times, so
expect roughly N× the generated C, compile time and binary size. FF7's
executables are large and the three are near-identical, so shard-level dedup by
content hash is probably available — but treat that as an optimisation after the
registry works, and measure the un-deduped cost first so the "one fat binary"
decision below is made on numbers.

> **Gate.** A three-program binary links and runs; the boot disc's program is
> selected from the mounted serial; a single-disc build is unchanged.

### P3 — A faithful lid in `cdrom.c`

*After P0. Parallel to P2.*

Axis A. The drive needs the state it has never had: open, closed, and the latch
between them. This is not a new axis — it executes the `shell/lid` clause of the
**CDROM** item already open in
[`internal/ACCURACY_BURNDOWN.md`](internal/ACCURACY_BURNDOWN.md) (Axis 4), whose
recorded method is "Beetle `cdrom.cpp`; psx-spx *CDROM*; validate by
sector/response diff". Update that item there rather than opening a parallel
record.

- `cdrom_open_shell()` / `cdrom_close_shell(path)`: motor and read stream torn
  down, CDDA stopped, shell bit set and *latched* until read, commands erroring
  while open, TOC re-read on close.
- Modelled from psx-spx and the BIOS CD driver, then diffed against the P0
  oracle trace. Not "make our trace agree with our other trace".
- The existing `cdrom_init()` re-mount keeps its meaning as a hard reset; the
  swap path is the new one, and the two must not be conflated.
- `debug_force_cd_reinsert()` already pokes `stat_reg` to `CDSTAT_SHELL` and
  immediately back. It is a debug transient, not a state machine. Either it
  becomes a caller of the real swap path or it goes — what it must not do is
  survive alongside as a second, divergent notion of reinsertion.
- Save-state wire version bump, with the same "intentionally breaks older
  savestates" note `boot_state.h` already sets a precedent for.
- Observability per the standing rule: a swap ring in the debug server
  (`disc_swap_*`), no printf anywhere.

**The gate cannot be a raw trace match, because this subsystem already diverges
from the oracle on purpose.** `ACCURACY_BURNDOWN.md` records a
`KNOWN_DIVERGENCES["cdrom"]` set in `tools/devtrace_diff.py` where we are
deliberately *more* faithful than Beetle — and one of those entries lands
squarely on this path: our CD controller version (`94h`, the real SCPH-1001
sub-CPU) keeps the shell CD-init flag at `[0xA000DFFC]` clear, where Beetle's
hardcoded `97h` sets it and forces a spurious boot ReadTOC. Shell state and TOC
re-reads are exactly what P3 changes, so a swap diff will show CD-init deltas
that are correct.

> **Gate.** Our command/response trace across a swap matches Beetle's *modulo
> the recorded `KNOWN_DIVERGENCES["cdrom"]` entries*, with first divergence
> resolved. Any new delta is a bug until it is either fixed or added to that
> list with a psx-spx citation and a soak — never silenced. And the game's own
> "Insert Disc 2" screen proceeds.

### P4 — Program handoff mid-session

*After P2 + P3. Conditional — only if P0 says it happens.*

If the swapped-to disc re-executes its own boot executable, the active backend
has to change while the machine is running. If P0 shows the running program
simply continues, **this phase does not exist and should be deleted rather than
built speculatively.**

- Deactivate the current game backend and activate the new one at a proven
  boundary, re-arming everything keyed to the old program: dirty-RAM epochs and
  overlay page generations, the text-image guard, `fntrace` game ranges,
  save-state and rewind integrity keys, `psx_selfcheck`.
- A handoff at an unproven boundary is a fatal, not a best-effort.

> **Gate.** Entry into the second program is observed natively and matches the
> oracle at the same point.

### P5 — One entry, N slots, in the launcher

*After P1. **Lands in the recomp-ui repo, not this one.***

The visible deliverable. The model is where the work is — both render backends
draw whatever it exposes, so the ImGui and Clay paths come along for free.

The file and line references in this section were recorded from recomp-ui and
are not verifiable from this tree (see §2). Re-confirm them there before
starting, and land this phase as its own change in that repo, gated on the P1
schema shipping here first.

- **ABI** (`recomp_launcher.h`): `disc_count`, per-slot path / label / serial /
  verdict, `boot_disc_index`; the launch call returns the chosen index alongside
  the path.
- **Model**: a slot array replaces the single rom path;
  `launcher_model_set_rom` becomes per-slot; `disc_verify_cb` runs per slot;
  `prepare_disc` runs per slot.
- **Readiness**: a present-and-verified boot disc is required to play; missing
  later discs warn rather than block, so a player with only disc 1 can still
  start.
- **Dashboard**: a Discs card listing Disc 1..N with each slot's verdict badge
  and its own Change… control, replacing the single Change Disc row. Slot rows
  reuse the existing checklist verdict vocabulary (serial / region / ISO
  header).
- **Wizard**: a multi-slot pick page on first run, all slots on one page.
- **In-game**: a Discs row in the pause UI that performs the swap. Player-driven
  only — auto-inserting the next disc when the guest reads a missing sector
  belongs in the enhancement tier as an opt-in mod, not in the core.

> **Gate.** FF7 appears as one launcher entry with three verified slots; PLAY
> boots the selected disc; the pause-menu swap lands in-game.

### P6 — Everything else that assumed one program

*After P2–P5.*

A sweep across subsystems whose invariants quietly encode "there is exactly one
disc". The rule for each is the same: handle the disc set, or fail loudly with a
named reason. Silent wrong-disc state is the failure mode to design out.

- **Save states**: the slot menu names the disc; loading a state whose program
  is not the active one either re-selects the backend and re-mounts, or refuses
  with the reason. Never loads onto the wrong program.
- **Rewind**: same integrity key, same rule.
- **Netplay**: `required_disc_fp` and `required_tracks` become per-disc; peers
  must agree on the disc *index* at join, and `disc_mismatch` gains that
  dimension.
- **Mods**: derived-disc providers resolve per source disc, or a multi-disc
  title rejects them with an explicit single-disc-only message.
- **Prepare / verify**: `[prepare_disc]` digests per disc.

> **Gate.** Every subsystem in the list either handles the set or errors by
> name. No path can reach a state where the mounted disc and the active program
> disagree without saying so.

### P7 — Scaffolding, packaging, catalog

*After P6.*

Making `--disc` repeatable is the small part of this phase and it is tempting to
do first. Resist that: the flag is the *last* five percent. Everything the flag
would feed is singular, and a scaffold that accepts three discs and then writes
one program's project is worse than one that refuses.

**Partially landed — the verification half only.** `--disc` is repeatable today
(`-Disc a,b` on PowerShell, which has no repeatable parameters), and
`verify_disc_set.py` probes every disc with `probe_disc.py --identity-only` and
checks the set *before* `$ROOT` exists. A data-only set scaffolds its one
program and records `disc_probe.N.json` + `disc_set.json`; an N-programs set,
a duplicate image, or a mixed-release set is refused by name with nothing
created. `game.toml` still carries the singular `disc` key, so `generate` is
unaffected and single-disc scaffolding is byte-identical to before. **None of
the generation half below has landed** — no per-disc `[[game.disc]]`, no
per-program seeds, no N-program build.

**The arg parsing is genuinely cheap.** There is no interactive disc prompt to
extend — the disc is a hard CLI requirement checked before prompting begins
(`setup_project.sh:221-230`), deliberately excluded from the `prompt_line` flow
because `read` gives no tab completion. `--yes` mode imposes nothing extra. So
the shell side is an accumulator in the arg loop (`:164-206`) plus the
`DISC`→`DISC_ABS` block (`:728-743`); `setup_project.ps1:20` becomes
`[string[]]$Disc`, which PowerShell supports natively. Note it must be written
twice — the `.ps1` mirrors the `.sh` argument-for-argument, including the probe
invocation (`sh:779-792` ≡ `ps1:543-556`).

**What actually blocks it — every downstream artifact is singular:**

- `probe_disc.py` **clobbers rather than merges.** `render_game_toml`
  (`:478-534`) builds a file from scratch and `:659` `write_text`s it. A second
  disc overwrites disc 1's `game.toml`, `catalog_identity.json` (`:673`),
  `disc_probe.json` (`:648`, and it is the one probe artifact that gets
  committed) and `seeds/ghidra_funcs.txt` (`:690-707`, `write_text`, not
  append). Only `--write-boot-exe` (`:666`) escapes, and only by luck — it names
  the file after the on-disc boot EXE, so two discs that reuse a boot-EXE name
  silently last-write-wins.
- It emits singular `disc = "..."` at `:490` and has no `discs` codepath at all,
  even though `config_loader.cpp:1217` has accepted the array form all along.
  **The consumer already accepts what the scaffolding refuses to emit.**
- `[recompiler] seeds` is written as one hardcoded path (`:510`), so per-program
  seed lists need the schema change flagged in `config_schema.md`'s "not yet"
  list.
- Templates bake one program in four load-bearing places: `CMakeLists.txt.in:32-33`
  (`GEN_MARKER` + `GEN_FULL_GLOB` keyed to `@BOOT_EXE@`), `codegen_setup.c.in:16`
  (the setup-host's "is it generated yet?" marker), and `symbols.toml.in:6-11`
  (a single `[[func]] BootEntry`). `fill_tokens.py` does scalar `@TOKEN@`
  substitution with no list support.
- `runtime.cmake:1709` has `GEN_MARKER` in `oneValueArgs`, and `:1827-1831`
  makes a missing one a `FATAL_ERROR`; that single marker is what decides
  setup-host vs. full runtime (`:1838-1841`).
- **Hard blocker even for the data-only case:** `psxrecomp_cli.py:968-972` —
  the `generate` the scaffold itself invokes — reads only `game.get("disc")` and
  errors `no --disc and game.disc empty`. A `game.toml` carrying only `discs`
  fails to generate. `disc` has to stay as a first-disc alias, or `generate`
  learns the array.

**Precedents to copy rather than invent:** `GEN_FULL_GLOB` is already
`multiValueArgs` and accumulated with `foreach` (`runtime.cmake:1715, 1875-1881`),
so the *sources* side already tolerates N programs; `symbols.toml.in`'s
`[[func]]` is already an array-of-tables; and `[[runtime.warm_cd_routes]]`
(`config_loader.cpp:431-447`) is a clean, bounded, per-entry-error-context parse
to model `[[game.disc]]` on.

Then the rest of the phase:

- Packaging and the setup-host zip ask for N discs and stage N generated
  programs.
- `retcomm-catalog`: one entry declaring N disc requirements, so the storefront
  shows a single title.
- `project_studio` is a fourth surface: `models.py:81` `disc: str | None` and the
  GUI's single `disc_var` (`gui.py:388`).
- `docs/GAME_PROJECT_SETUP.md` documents the singular probe contract and needs
  updating with it.

> **Gate.** A fresh three-disc project scaffolds, generates, builds and ships as
> one entry, with no hand-editing of generated output anywhere in the path.
> Specifically: no probe output is overwritten by a later disc, and a scaffold
> given N discs that the pipeline cannot yet build **refuses by name** rather
> than quietly producing a one-program project.

---

## 4. Open decisions

Each of these changes the shape of the work rather than its details. They are
recorded here unresolved; the recommendation is the author's, not a ruling.

### One fat binary, or one binary per disc?

Per-disc binaries skip P2 entirely — the launcher just execs the right one, and
`LNG_ACTION_RELAUNCH` already exists. The cost is three copies of the runtime, a
visible relaunch on every swap, and no path to a mid-session handoff.

**Recommend** — single binary with the registry. It is the only shape that
satisfies "a single launcher entry" end to end, and it reuses a mechanism this
codebase already ships. Revisit only if P2's measured build cost is prohibitive.

### Disc swap during netplay: forbid, or synchronise?

A swap is a large asynchronous state change on one peer. Making it a lockstep
event is possible but adds a rollback-visible transition nobody has needed yet.

**Recommend** — forbid mid-session swaps in netplay, gate on matching disc index
at join, and say so in the UI. Revisit if a multi-disc title turns out to have
multiplayer that spans discs.

### Loading a state whose program is not the active one

Refuse with a clear message, or auto-remount the right disc and re-select the
backend? The second is friendlier and strictly more machinery.

**Recommend** — refuse loudly in P6, and revisit auto-remount as an enhancement
once the swap path itself is proven.

### Keep the flat `discs = [...]` form?

Redefining it as "one program, N images" gives data-only multi-disc titles a
one-line config. Dropping it makes `[[game.disc]]` the single spelling.

**Recommend** — keep it. Data-only multi-disc is a real and much simpler case,
and it deserves to look simpler in config.

---

## 5. How this stays inside the rules

| Rule | How this plan satisfies it |
|------|----------------------------|
| No per-game hacks | The disc set is a general mechanism keyed to documented PSX structures — serials, TOCs, the drive's shell state. No title IDs enter the framework. |
| No stubs | Every wrong-disc and missing-backend path is a named fatal. Nothing silently falls back to disc 1's program or synthesises a sector. |
| Oracle first | P0 gates everything. The lid state machine in P3 is validated against Beetle, not against our own second opinion. |
| Unknown, not guessed | Whether a swap re-executes the boot EXE is explicitly unknown right now. P4 exists conditionally and gets deleted if P0 says it should. |
| Never edit generated output | All of P2 is emitter and cmake work. The stem prefixing happens in the game emitter, the way it already does in the BIOS emitter. |
| No printf, no log files | Swap observability is a debug-server ring, following the existing CD command-history and sector-history rings. |
| Fix the tool | If Beetle's disc-control path is not reachable from `beetle_libretro.cpp`, wiring it up becomes the first task of P0 — not a caveat carried into P3. |

---

## Provenance

Nothing here is implemented. Every file:line in §2 was read from the working
tree at `c0139b45` and verified on 2026-08-28; the recomp-ui rows are the
exception and are marked as unverified from this tree. Line numbers rot —
re-grep the symbol rather than trusting the number.
