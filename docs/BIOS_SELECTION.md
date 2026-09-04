# Which BIOS a build uses

A PlayStation game needs a BIOS. PSXRecomp can supply one — **OpenBIOS**, an
MIT-licensed, from-scratch PS1 BIOS from the PCSX-Redux project that we are
allowed to redistribute — so a player can be handed a build and a disc image and
just play. A player who prefers their own dumped retail BIOS can use that
instead.

Both recompiled BIOS backends are linked into every normal build. The OpenBIOS
image itself and its MIT notice are staged in `bios/` beside the executable;
the retail image is never shipped and comes from the player. Which backend runs
is decided when the game launches, not when it is built.

## The rule

> **No BIOS chosen → OpenBIOS. A BIOS the player explicitly chose → that BIOS.**

That is the whole user-facing contract. A player never has to find a BIOS file
to start playing, and never loses the ability to use their own.

A developer can switch this off for a title that is known not to work on
OpenBIOS; then a retail BIOS is required and the player is prompted for one.

## What counts as "chosen"

A deliberate act by the player:

- `--bios <path>` on the command line, or
- a BIOS selected in the launcher / settings, which is then remembered.

**Setup / Generate first-run discovery.** When there is no `bios.cfg` yet (and
no settings pick), install & build looks next to the executable / project for a
validated retail dump (`SCPH1001.BIN` and common filename variants; size + CRC
must match the compiled-in retail image). If found, that path is written to
`bios.cfg` and used for Generate & Play. If not found, the build stays on
**OpenBIOS** with no prompt.

Discovery does **not** run when `bios.cfg` already exists — including after the
player cleared the pick to OpenBIOS. Play-time resolution still never invents a
BIOS from random files on disk once a choice has been recorded.

A player who has chosen a BIOS can clear that choice and go back to OpenBIOS;
the choice is not permanent.

## "A retail BIOS" means one specific image

A build compiles in exactly one retail image (currently SCPH-1001) alongside
OpenBIOS. The compiled-in code is generated *from that exact image*, so a
different dump — SCPH-101, SCPH-5552, a bad rip — cannot be substituted: the
recompiled code would execute against data it does not match, and crash.

Every build therefore checks the identity of a chosen BIOS before using it, and
says which image it expects if the check fails. This is not DRM and not a
preference; it is the difference between running and wild-jumping.

The acceptance gate compares **file size and CRC32** against each linked
backend's recorded identity (`bios_backend_for_file` in `runtime/src/main.cpp`).
A SHA-256 is also recorded per image (`image_sha256` in
`runtime/include/psx_bios_image.h`) and is surfaced for provenance and bug
reports, but it is **not** part of the accept/reject decision.

If a chosen BIOS does not match:

- with OpenBIOS available, the player is told why and can continue on OpenBIOS;
- with OpenBIOS disabled for the title, the choice is rejected and re-prompted.

## Developer setting

One optional line in `game.toml`:

```toml
[runtime]
openbios = true    # default. false = this title requires a retail BIOS.
```

Set `openbios = false` only for a title with a **verified** OpenBIOS
incompatibility. Per-title compatibility is not implied by the framework
supporting OpenBIOS — verify a title before shipping it that way.

This setting is deliberately **not** overridable by a player's `settings.toml`,
unlike most runtime options. It records a developer's compatibility finding, not
a preference. A player re-enabling OpenBIOS on a title known to break under it
produces confusing bug reports and no benefit.

There is no third mode. A title either allows OpenBIOS (and still honours an
explicit retail choice) or requires retail. "OpenBIOS only, retail forbidden"
would take away a working option from players for no reason.

## Boot behaviour does not depend on which BIOS runs

Whichever BIOS is active, `[runtime] bios_hle` (on by default) skips the BIOS
boot sequence and goes straight to the game. That is deliberate and enforced: the
skip is not a synthesized handoff, it just returns immediately from the BIOS's
call into its shell, so all it needs is the per-image address of that shell entry
— which every linked BIOS records. A player who switches BIOS gets the same
boot, and the flag means one thing everywhere.

The HLE tier's *other* half — servicing a few kernel calls (the B0 event family)
in the runtime instead of the recompiled BIOS — **is** per-image, because it needs
the kernel's own `DeliverEvent` return address to send a delivered callback back
to. Retail SCPH-1001 exports it; OpenBIOS deliberately does not until its event
semantics are validated, so OpenBIOS runs those calls on the recompiled kernel and
prints one line at startup saying so. That refusal is scoped to kernel calls and
never cancels the boot-skip.

Until 2026-07 it did: the boot-skip was derived from the already-refused
kernel-call decision, so choosing OpenBIOS silently sat the player in the boot
animation while the identical build on retail went straight to the game. The two
axes are now resolved in one pure, unit-tested function
(`psx_bios_hle_plan()`, `runtime/include/bios_hle_plan.h`), and the test asserts
the boot decision is byte-identical across the two images.

## Save data

**Memory cards are unaffected.** They are card images; they carry no BIOS state
and move freely between BIOSes and between builds.

**Savestates are BIOS-specific.** The kernel occupies RAM, and OpenBIOS and
retail lay that RAM out differently, so a savestate captured under one BIOS is
not valid under the other. Savestates therefore record which BIOS was active and
refuse to load under a different one. Without that check, switching BIOS would
silently corrupt a restored state — the only quiet failure mode in this design,
which is why it is enforced rather than warned about.

Slot files are also isolated on disk under the per-game memcard root:

- `<memcard_dir>/openbios/state_*_slot*.pst`
- `<memcard_dir>/scph1001/state_*_slot*.pst`

On first configure, any legacy loose `<memcard_dir>/state_*_slot*.pst` files are
moved once by `.pst` header `bios_checksum` (bundled OpenBIOS wordsum →
`openbios/`, otherwise → `scph1001/`). Memory cards stay in `<memcard_dir>/`;
netplay guest sandbox remains `<memcard_dir>/netplay/` (unscoped).

## Netplay lobby settle

Online and LAN lobbies advertise a per-peer BIOS offer and freeze a single
session BIOS at host Start (`openbios` or `scph1001`):

- **Online:** `bios_offer` on `set_ready` → host publishes
  `match_caps.session_bios`.
- **LAN:** peers append offer fields on `MOTK3 JOIN`; host broadcasts them on
  `MOTK4 UPDATE` and includes the settled token on `MOTK1 START`.

Settle rule (same for both):

- **OpenBIOS** if any seated peer prefers OpenBIOS, or any peer cannot run
  SCPH-1001 (no linked retail backend and/or no validated dump), or a peer
  sends no offer (legacy client).
- **SCPH-1001** only when every seated peer can run it and nobody selected
  OpenBIOS.

Every peer applies that session BIOS before boot. Mixed BIOSes are invalid for
rollback (kernel RAM layout differs). If the session settles to SCPH-1001 but a
peer has no validated dump, that peer **aborts the launch** rather than silently
falling back to OpenBIOS (which would desync immediately).

Session BIOS is **ephemeral**: it affects only that match’s runtime boot. It
does **not** rewrite `bios.cfg`, `settings.toml`, or the launcher Settings BIOS
row. Soft-return shows the player’s durable preference again; the next Start
re-settles from offers as usual.

## Why both are compiled in

The alternative — a build flavour per BIOS — was the original design, and it
required a game to state its BIOS in three places that nothing cross-checked
(`bios_config` in `game.toml`, plus two CMake variables). Setting one and
forgetting another produced a build that linked one BIOS's code against another
BIOS's generated game code, and it compiled cleanly.

Carrying both costs roughly 20 MB of binary and removes that entire class of
mistake: one build configuration, one source of truth, the choice made at
runtime where it can be validated.

It is affordable because the game's own recompiled code is **identical**
regardless of which BIOS it was generated against — verified byte-for-byte on
Tomba!, Tomba! 2 and Ape Escape. Only the BIOS code is duplicated, and the two
images collide on just 22 symbols, which are namespaced per BIOS.

## Attribution

OpenBIOS is MIT-licensed. Its notice is vendored at `bios/OpenBIOS.LICENSE`,
with the upstream source pin and build recipe in `bios/OpenBIOS.toml` and
attribution in `THIRD_PARTY_ATTRIBUTION.md`. Builds that ship it credit the
PCSX-Redux authors in the launcher, whether or not the licence compels it.

Native runtime builds automatically stage both `bios/openbios.bin` and
`bios/OpenBIOS.LICENSE`. Release packaging must copy that directory as a unit;
shipping the image without its notice violates the distribution contract.

Retail BIOS images are **not** redistributable and are never shipped. A player
using one supplies their own dump.
