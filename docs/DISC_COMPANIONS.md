# Disc subchannel companions

An SBI companion contains disc subchannel metadata that a main-track image can omit.
`tools/prepare_disc.py` retains a user-supplied companion when it copies or converts a disc.
`psxrecomp_cli.py verify-disc` and `generate` check it before generation.
These tools do not supply, download, or license SBI files.

## Supply a companion

1. Obtain the matching SBI file through a lawful source.
2. Put it beside the input image with the same basename and the `.sbi` extension.
3. For CUE input, use the CUE basename, even when its first BIN has a different name.
4. Run preparation or verification again.

For example, `Album.cue` uses `Album.sbi`.
Preparation accepts `.SBI` too, then writes the output CUE basename with a lowercase `.sbi` extension.
It retains the companion bytes unchanged, including when the output CUE has a new name.
The runtime loads that companion beside the mounted CUE or image.
An SBI beside the first track does not substitute for one beside the selected CUE.

Preparation refuses conflicting output companions before it writes disc files.
Use an empty output directory or move the conflicting companion before retrying.
An identical existing companion remains untouched.
Concurrent preparation into the same directory is unsupported.

## Exact revision coverage

The registry in `tools/disc_companion.py` requires both measured main-track size and SHA-1.
It does not infer protection from a region, filename, or configured serial.
The initial supported identity is:

| Field | Measured identity |
|---|---|
| Title / serial | Resident Evil 3: Nemesis / SLES-02529 |
| Revision key | Europe; exact data track below, no claim for other pressings |
| Main-track size | `712491360` bytes |
| Main-track SHA-1 | `b3ec6631e1c9ed3b0f554b3a5f61166bac8ebb9d` |
| Qualified SBI SHA-256 | `ada8877a2a964eff2743d53cc043be0b7b148469b60de82fd1069f32700670eb` |

For that identity, a missing or different SBI stops setup with an actionable error.
`--skip-hash-check` does not bypass the companion gate.
The matching companion must also pass the supported nonempty SBI type-1 record checks.
A different valid SBI representation needs maintainer qualification before this gate accepts it.

Unknown identities remain `unknown` when no SBI exists.
A supplied, structurally valid SBI for an unknown identity is `format_valid_unqualified`.
Neither result proves complete subchannels or compatibility with that revision.
Cooked ISO and 2448-byte conversions preserve supplied SBI files but do not inherit the raw-track registry identity.
The existing 2448-byte conversion discards embedded subchannels; this change does not extract them into SBI.
CHD preparation and automatic protection detection for other revisions are outside this tool's coverage.

## Receipts and callers

Preparation writes `<output-cue-basename>.disc-receipt.json` beside the output.
It records the source image, measured data-track hashes, output CUE, and separate `subchannel` identity.
The companion record contains its source path, byte size, SHA-256, record count, status, and output path.
For a qualified revision, it also contains the registry identity and evidence reference.
Keep this receipt private with the user's inputs because it includes local paths.

The existing `RESULT_CUE=` output remains unchanged.
CLI disc results add the same `subchannel` record.
The legacy `verified` field describes only the existing main-track check; it never proves complete subchannels.
Missing required companions return preparation exit code 1 or CLI verification exit code 3.
No configuration keys changed.
Existing setup hosts need the updated Python tools, including `tools/disc_companion.py`, to receive this gate.
Older installed setup kits retain their existing behavior.

## Native launcher

The native launcher also checks the exact data-track SHA-256 before Play.
With the matching recomp-ui update, the verification card shows `SBI File`:
`Missing` for a required missing or mismatched companion, `OK` for a match,
and `N/A` when the registry has no requirement for this disc.
`N/A` is not proof that an unknown revision has complete subchannels.

Select the disc first. Use **Browse For Disc / SBI** to select its matching SBI.
The native host checks the SBI hash before staging a private CUE and SBI under
`sbi-input` beside the executable. CUE track references become absolute paths;
the original tracks and companions remain untouched. The launcher selects the
staged CUE and uses its normal settings persistence. A rejected SBI leaves the
disc selection intact and shows an error below the button.

This importer supports registered CUE/BIN inputs. It does not infer a match for
unknown revisions. Build the UI and host together after changing the launcher C
structures. `RECOMP_LAUNCHER_HAS_SBI_STATUS` enables the status and import callback.
Regenerate `runtime/include/sbi_registry.h` with `tools/generate_sbi_registry.py`
after changing the metadata registry; it contains identities, not companion data.

## Provenance and validation

The registry metadata comes from the private 2026-09-05 SLES-02529 same-executable control and operator acceptance.
The unchanged Wave 3 source was `63446a28111a21a0cb7e8b5106c3614c898fc4b5`.
The accepted executable SHA-256 was `131e8ecbd18a425ba2b5b49a30133fc0afed888af13666b653b3b5e927f3b0fe`.
It closed normally at frame 14841 with `sdl_window_close` after the operator reported correct behavior.
The same main track without SBI stalled at the warning screen.
The private portfolio report and operator receipt retain the detailed evidence; no retail or SBI payload accompanies this source.
That technical acceptance does not grant redistribution rights for the companion.

Primary format reference: [PSX-SPX CDROM format](https://psx-spx.consoledev.net/cdromformat/#cdrom-protection-libcrypt).
The existing runtime reader supplies the type-1 parsing contract in `ISOReader::LoadSBICompanion`.
Runtime support came from `cebe738fd17cc4c37a76729462e093ff16ee457a`.
Upstream `17f49ad3b20dc30917a881a02baaa25374c13d18` already contains that commit, but lacks the setup gate and copy step.
This change starts from that upstream identity. Native setup changes affect
launcher validation and input staging; guest execution and the SBI reader are unchanged.

Run the source-owned setup tests from the framework root:

```sh
python tools/tests/test_disc_companion.py -v
python tools/tests/test_sbi_registry.py -v
```

The fixtures create synthetic ISO directory records, an inert executable header, audio bytes, and SBI records.
Tests bind their own synthetic revision through an in-memory registry entry.
No fixture copies retail game data, BIOS data, or a third-party SBI.
