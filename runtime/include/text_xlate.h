/* text_xlate.h — on-the-fly string translation / localization (framework).
 *
 * A reusable psxrecomp feature: capture the game's source strings from guest
 * memory and substitute translated bytes on the fly so a JP-only title can be
 * played in the target language. See docs/STRING_TRANSLATION.md for the design.
 *
 * Mechanism (all always-on, hung off the psx_dispatch chokepoint):
 *  - CAPTURE: on every dispatch, scan the arg registers for pointers to source
 *    text records (encoding-profile validated). Every distinct record is hashed
 *    (FNV-1a64) and recorded into an in-memory inventory ring queryable over the
 *    TCP debug server (the "enumerate every string" authoring source). Never
 *    arm-then-run — the ring records from boot; the debug command queries it.
 *  - APPLY: when an arg points at a record present in the loaded translation
 *    table, transcode the target-language string to the game's glyph codes,
 *    write it into transient guest-stack scratch (below $sp), and repoint the
 *    arg register — the ORIGINAL draw routine then renders the replacement.
 *    KV-gated, so a stray pointer that isn't in the table is left untouched.
 *  - GLYPH-LABEL PATCH: UI text drawn glyph-by-glyph from a fixed-stride,
 *    space-padded EXE table (level/stage names) has no string pointer and no
 *    terminator, so the arg-scan hook can never see it. Instead each confirmed
 *    slot is patched in guest RAM once the EXE region is resident: verify the
 *    expected JP bytes are present, then overwrite exactly the slot width with
 *    target-language fullwidth Shift-JIS re-padded to that width. The game's own
 *    per-glyph routine then draws the translation. Fed by [[glyph_label]] TOML
 *    entries; gated on language; never spills past the confirmed slot bounds.
 *  - VRAM-STRIP PATCH: some UI text is PRE-RENDERED PIXELS in a texture asset
 *    (e.g. Tsumu's HUD labels live in the glyph-sheet image the game uploads to
 *    VRAM and samples with quads) — there are no glyph codes anywhere, so
 *    neither hook above can ever see it. Instead the patch rides the game's own
 *    upload: on completion of every CPU→VRAM image transfer (GP0 0xA0) any
 *    configured rect contained in the upload is verified against its expected
 *    JP pixels and, only on an exact match, rewritten with the target-language
 *    pixels through the same renderer write path (software/GL/supersample
 *    coherent). Re-applies automatically whenever the game re-uploads the
 *    asset — this is a source-path patch, not a one-shot VRAM poke. Fed by
 *    [[vram_patch]] TOML entries; gated on language; verify-before-patch.
 *
 * Encoding is abstracted behind an EncodingProfile (validator + terminator rules
 * + UTF-8 -> glyph transcoder) selected per title; Tsumu uses the shift_jis
 * profile. No generated-code edits, no regen: the hook is called from
 * fntrace_record (runtime source), so it works in Release.
 */
#ifndef PSXRECOMP_TEXT_XLATE_H
#define PSXRECOMP_TEXT_XLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Called at the top of every psx_dispatch iteration (from fntrace_record),
 * BEFORE the target function runs, with cpu->gpr[4..7] holding this call's
 * args. Cheap no-op when the module is uninitialised or disarmed. */
void text_xlate_on_dispatch(struct CPUState* cpu, uint32_t target);

/* Aggregate hot-path gate for fntrace_record. True only while capture, string
 * substitution, or an unsettled RAM text patch needs dispatch callbacks. */
int text_xlate_dispatch_armed(void);

/* One-time init. Loads every translations/ *.toml under project_root and
 * selects the active `language` column. language "jp"/"off"/"" (or no tables)
 * disables APPLY; CAPTURE still runs (always-on inventory). The PSX_LANG env
 * var overrides `language` when set. Safe to call once at startup; copies what
 * it needs. */
void text_xlate_init(const char* project_root, const char* language);

/* Switch the active language and reload the translation tables. For the launcher:
 * text_xlate_init runs at config-load (before the launcher), so the launcher's
 * chosen language is re-applied through this after it returns. "jp"/"off"/""
 * disables APPLY (untranslated native game). PSX_LANG env still wins if set.
 * No-op if `language` matches the already-active one. Thread-safe. */
void text_xlate_set_language(const char* language);

/* Called by the GPU on completion of every CPU→VRAM image upload (GP0 0xA0)
 * with the upload rect in VRAM halfword coords. Applies any [[vram_patch]]
 * whose rect is contained in the upload (verify-then-patch). Cheap no-op when
 * no patches are loaded or the language is off. */
void text_xlate_vram_upload(int x, int y, int w, int h);

/* Debug-server surface (observability; no log files — Rule 3). subcmd:
 *   "stats"   -> counters (calls, hits, distinct captured, table size, lang)
 *   "dump"    -> JSON array of every captured record {hash, addr, pc, len,
 *                count, translated, sjis_hex} — the authoring inventory
 *   "todo"    -> JSON array of captured records with NO table entry yet
 *   "glyph"   -> JSON array of per-glyph label RAM-patch slots {addr, width,
 *                patched, en} — status of the glyph-label source-patch layer
 *   "vpatch"  -> force-apply every [[vram_patch]] against current VRAM (verify
 *                still gates each) + per-patch status {x,y,w,h,applied}
 *   "reload"  -> re-read the translation table from disk
 * Returns bytes written into out (<= cap). */
int text_xlate_debug_json(const char* subcmd, char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_TEXT_XLATE_H */
