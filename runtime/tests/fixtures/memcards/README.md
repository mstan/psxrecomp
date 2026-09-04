# Memory-card test fixtures

Four pre-populated 128 KiB PlayStation memory-card images, used to exercise the
memcard read/write paths with a card that already has saves on it. Every image
is exactly `131072` bytes and starts with the `MC` header magic; every directory
entry below is state `0x51` (in use, first block of its file).

These lived at the repo root until they were moved here — nothing in the build
reads them from a fixed path, so they are inert data a human or a test copies
where it needs them.

| File | Consumer | Directory entries |
| --- | --- | --- |
| `card1.mcd` | recomp runtime, slot 1 | `BISLPS-01234SAVE00`, `BASLUS-56789DATA01`, `BESLES-11111SLOT02` |
| `card2.mcd` | recomp runtime, slot 2 | `BESLES-11111SLOT02` |
| `dummy.0.mcr` | Beetle-PSX oracle, slot 1 | `BISLPS-01234SAVE00`, `BASLUS-56789DATA01`, `BESLES-11111SLOT02`, `BASLUS-0125100000-00` |
| `dummy.1.mcr` | Beetle-PSX oracle, slot 2 | `BISLPS-01234SAVE00`, `BASLUS-0125100000-00` |

The `BI`/`BA`/`BE` prefixes are the standard region byte pairs; the synthetic
`SLPS-01234` / `SLUS-56789` / `SLES-11111` product codes are fabricated test
data. `SLUS-01251` (Tomba! 2) is a real code, retained because the oracle cards
were captured while driving that title.

## Using them with the Beetle oracle

`runtime/src/beetle_libretro.cpp` asks the libretro core for its save directory
and answers `"."` (see the `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY` case), so the
core opens `dummy.0.mcr` / `dummy.1.mcr` **relative to the oracle process's
working directory**, not relative to this repo. To boot the oracle with
populated cards, copy them next to where you launch it:

```sh
cp runtime/tests/fixtures/memcards/dummy.[01].mcr <psx-beetle-cwd>/
```

Without them the core still boots; it prints a warn-only
`WARNING: dummy.N.mcr missing — slot N will be blank` line (same file, the
memcard sanity-check block) and both slots come up empty.

## Using them with the recomp runtime

`card1.mcd` / `card2.mcd` are the recomp-side counterparts. Copy them to
whichever card paths the run is configured with — memcard paths resolve against
the project root, not the executable, so an unconfigured run writes
`card1.mcd` / `card2.mcd` at the root (which `.gitignore` deliberately ignores,
so a run can never dirty the tree by touching these fixtures).
