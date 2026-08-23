/* text_xlate.cpp — on-the-fly string translation / localization (framework).
 * See text_xlate.h + docs/STRING_TRANSLATION.md. */

#include "text_xlate.h"
#include "netplay_ram_dirty.h"
#include "cpu_state.h"
#include "psx_ram.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <filesystem>

#include "toml.hpp"

extern "C" uint8_t* memory_get_ram_ptr(void);
extern "C" { extern uint64_t s_frame_count; }
/* Renderer VRAM facade (gpu_render.h) — the same read/write path the GP0 0xA0
 * upload uses, so vram-strip patches stay coherent across software/GL backends
 * and the supersampling mirror. */
extern "C" uint16_t gr_vram_read(int x, int y);
extern "C" void     gr_vram_write(int x, int y, uint16_t pixel);
// Bless an in-place RAM patch into the text-image guard so it isn't mistaken for
// self-modifying code (memory.c). Our patches (string/glyph tables) share pages
// with real compiled functions; without this the guard blocks their native
// dispatch and the game fatally routes to psx_unknown_dispatch.
extern "C" void     dirty_ram_text_bless(uint32_t phys, const uint8_t* bytes, uint32_t len);

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Guest RAM access (little-endian, no swizzle). Fold KUSEG/KSEG0/KSEG1 into
// live DRAM (2 MB mirrors or unique 8 MB). Returns 0 / no-op for out-of-range.
// ---------------------------------------------------------------------------
inline bool ram_fold(uint32_t va, uint32_t* pa_out) {
    uint32_t p = va & 0x1FFFFFFFu;
    if (p < 0x00800000u) { *pa_out = psx_ram_map_read(p); return true; }
    return false;  // I/O / BIOS / scratchpad — not translatable text storage
}
inline uint8_t grb(uint8_t* ram, uint32_t va) {
    uint32_t pa; return ram_fold(va, &pa) ? ram[pa] : 0u;
}
inline void gwb(uint8_t* ram, uint32_t va, uint8_t v) {
    uint32_t p = va & 0x1FFFFFFFu;
    if (p >= 0x00800000u) return;
    p = psx_ram_map_write(p);
    ram[p] = v;
    np_ram_dig_note_write(p);
}
inline bool va_in_ram(uint32_t va) { uint32_t pa; return ram_fold(va, &pa); }
// Bless a just-written patch region into the text-image guard (see the extern
// above). gwb writes RAM directly; this syncs the guard's reference so the patch
// reads as intentional data, not self-modifying code. Reads the now-current bytes
// straight from guest RAM, so it always blesses exactly what was written.
inline void bless_patch(uint8_t* ram, uint32_t va, uint32_t len) {
    uint32_t pa;
    if (len && ram_fold(va, &pa)) dirty_ram_text_bless(va & 0x1FFFFFFFu, ram + pa, len);
}

// ---------------------------------------------------------------------------
// FNV-1a 64 over raw source bytes — the translation KV key.
// ---------------------------------------------------------------------------
inline uint64_t fnv1a(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ===========================================================================
// EncodingProfile — general text-encoding abstraction (validator + terminator
// rules + UTF-8 -> game-glyph transcoder). A title selects one; Tsumu uses
// shift_jis. Keeps the capture/apply core encoding-agnostic.
// ===========================================================================
constexpr uint32_t kSrcMax = 512;   // max source record bytes captured/hashed

enum class Term { None, Nul, FFFF };

struct EncodingProfile {
    const char* name;
    // Cheap first-byte gate: could `c` begin a text record?
    bool (*first_byte_textish)(uint8_t c);
    // Read a NUL/terminator-delimited record from guest RAM at va. Fills
    // out[0..*len) with the raw content bytes (terminator excluded), sets
    // *term. Returns true only for a plausible text record (>= min real chars).
    bool (*read_record)(uint8_t* ram, uint32_t va, uint8_t* out, uint32_t* len, Term* term);
    // Transcode a UTF-8 target string into game glyph bytes appended to `out`.
    void (*transcode)(const std::string& utf8, std::vector<uint8_t>& out);
    // Authoring quality gate: is this decoded record worth recording in the
    // always-on capture inventory (vs. binary that passed the reader by chance)?
    bool (*capture_worthy)(const uint8_t* bytes, uint32_t len);
};

// ---- Shift-JIS profile (Tsumu) --------------------------------------------
inline bool sj_lead(uint8_t c)  { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); }
inline bool sj_trail(uint8_t c) { return (c >= 0x40 && c <= 0x7E) || (c >= 0x80 && c <= 0xFC); }
inline bool sj_kana(uint8_t c)  { return c >= 0xA1 && c <= 0xDF; }               // halfwidth katakana
inline bool sj_ascii(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

bool sj_first_byte_textish(uint8_t c) {
    return sj_ascii(c) || sj_kana(c) || sj_lead(c);
}

// A record ends at NUL (menu/UI labels) or the game's 0xFFFF message terminator
// (framed multi-line messages). Content bytes are kept VERBATIM — including the
// game's control framing: a lone SJIS-lead byte with no valid trail, 0x8x/0xFE,
// and the "FE FF" line-break code — so the hash is byte-exact and the original
// renderer sees identical framing. Binary structs are filtered by requiring a
// minimum count of decodable 2-byte SJIS units.
bool sj_read_record(uint8_t* ram, uint32_t va, uint8_t* out, uint32_t* len, Term* term) {
    if (!va_in_ram(va)) return false;
    uint8_t b0 = grb(ram, va);
    if (!sj_first_byte_textish(b0)) return false;
    uint32_t n = 0, real = 0, two = 0;  // real=text units; two=decodable 2-byte SJIS
    Term t = Term::None;
    for (uint32_t i = 0; i < kSrcMax; ++i) {
        uint8_t c = grb(ram, va + i);
        if (c == 0x00) { t = Term::Nul; break; }
        if (c == 0xFF && grb(ram, va + i + 1) == 0xFF) { t = Term::FFFF; break; }
        if (sj_lead(c) && sj_trail(grb(ram, va + i + 1))) {
            if (n + 2 > kSrcMax) break;
            out[n++] = c; out[n++] = grb(ram, va + i + 1); ++i; ++real; ++two; continue;
        }
        if (sj_kana(c) || sj_ascii(c) || c == 0x0A) {
            if (n + 1 > kSrcMax) break;
            out[n++] = c; if (c != 0x0A) ++real; continue;
        }
        // Control / framing byte: lone SJIS lead (0x81..0x9F/0xE0..0xFC with no
        // valid trail — e.g. the "81 FF" / "FE FF" framing), 0x01..0x1F, 0x80,
        // 0xFD/0xFE, or a lone 0xFF (second half of a FE-FF line break). Keep it.
        if (n + 1 > kSrcMax) break;
        out[n++] = c;
    }
    // Reject binary: a genuine string carries at least two decodable 2-byte SJIS
    // chars (or, for a short pure label, at least three text units).
    if (t == Term::None) return false;
    if (two < 2 && real < 3) return false;
    *len = n; *term = t;
    return true;
}

// UTF-8 -> fullwidth-Shift-JIS. Tsumu's font already contains fullwidth Latin +
// digits (proven by "ＴＳＵＭＵ ｌｉｇｈｔ"), so English renders with zero glyph
// injection. '\n' -> raw 0x0A (the drawer's line break); unknown codepoints ->
// fullwidth space. (Halfwidth-ASCII path is a future EncodingProfile variant
// once MOJI.BIN confirms a halfwidth Latin block.)
uint16_t fw_sjis_for_ascii(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return (uint16_t)(0x8260 + (cp - 'A'));
    if (cp >= 'a' && cp <= 'z') return (uint16_t)(0x8281 + (cp - 'a'));
    if (cp >= '0' && cp <= '9') return (uint16_t)(0x824F + (cp - '0'));
    switch (cp) {
        case ' ':  return 0x8140;  // fullwidth (ideographic) space
        case '!':  return 0x8149;
        case '?':  return 0x8148;
        case '.':  return 0x8144;
        case ',':  return 0x8143;
        case ':':  return 0x8146;
        case ';':  return 0x8147;
        case '\'': return 0x8166;
        case '"':  return 0x8168;
        case '(':  return 0x8169;
        case ')':  return 0x816A;
        case '[':  return 0x816D;
        case ']':  return 0x816E;
        case '-':  return 0x817C;  // fullwidth minus
        case '/':  return 0x815E;
        case '&':  return 0x8195;
        case '$':  return 0x8190;  // fullwidth $ (Tsumu font: ○ button glyph)
        case '%':  return 0x8193;  // fullwidth % (Tsumu font: ✕ button glyph)
        case '+':  return 0x817B;
        case '=':  return 0x8181;
        case '*':  return 0x8196;
        case '@':  return 0x8197;
        case '<':  return 0x8183;
        case '>':  return 0x8184;
        case '_':  return 0x8151;
        default:   return 0x8140;  // unknown -> fullwidth space
    }
}

void sj_transcode(const std::string& utf8, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp; size_t adv;
        if (c < 0x80)                    { cp = c;                                   adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F);                             adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F); adv = 3;
        } else                           { cp = ' ';                                 adv = 1; }
        i += adv;
        // Framing tokens the author writes into the English (remapped to the
        // game's control codes in apply_to_reg): \n line break, \f page break,
        // \r prompt/wait. Preserve them as their raw ASCII control bytes here.
        if (cp == '\n') { out.push_back(0x0A); continue; }
        if (cp == 0x0C) { out.push_back(0x0C); continue; }   // \f -> page break
        if (cp == 0x0D) { out.push_back(0x0D); continue; }   // \r -> prompt
        // A codepoint already in the fullwidth SJIS range would be authored as
        // raw JP; here targets are Latin. Map ASCII-range codepoints; fullwidth
        // Unicode letters (U+FF01..) map back to their ASCII then to SJIS.
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp = cp - 0xFF00 + 0x20;
        if (cp == 0x3000) cp = ' ';
        uint16_t g = fw_sjis_for_ascii(cp <= 0x7E ? cp : ' ');
        // Endianness of the 16-bit glyph code as the game's drawer reads it.
        // EXE menu labels are big-endian SJIS; the message-table text is stored
        // little-endian. Selectable per run (PSX_XLATE_LE=1) until confirmed;
        // once known this becomes an EncodingProfile flag.
        static const bool le = [] { const char* e = std::getenv("PSX_XLATE_LE");
                                    return e && e[0] == '1'; }();
        if (le) { out.push_back((uint8_t)(g & 0xFF)); out.push_back((uint8_t)(g >> 8)); }
        else    { out.push_back((uint8_t)(g >> 8));   out.push_back((uint8_t)(g & 0xFF)); }
    }
}

// Transcode a UTF-8 label into fixed-width little-endian fullwidth-Shift-JIS for
// an in-place RAM source patch. Each ASCII char maps to its fullwidth glyph
// (2 bytes, stored low/high as the game's per-glyph drawer reads it), truncated
// to whole cells so at most `width` bytes are produced, then padded with the
// fullwidth space (0x8140 -> bytes 40 81) to exactly `width` bytes. NEVER emits
// more than `width` bytes — the hard guarantee against corrupting the next slot.
void label_transcode_le(const std::string& utf8, uint32_t width, std::vector<uint8_t>& out) {
    out.clear();
    const uint32_t maxbytes = width & ~1u;   // whole 2-byte cells only
    size_t i = 0;
    while (i < utf8.size() && out.size() + 2 <= maxbytes) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp; size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F); adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F); adv = 3;
        } else { cp = ' '; adv = 1; }
        i += adv;
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp = cp - 0xFF00 + 0x20;   // fullwidth Latin -> ASCII
        if (cp == 0x3000) cp = ' ';                                  // ideographic space
        uint16_t g = fw_sjis_for_ascii(cp <= 0x7E ? cp : ' ');
        out.push_back((uint8_t)(g & 0xFF));   // trail (low)  — little-endian storage
        out.push_back((uint8_t)(g >> 8));     // lead  (high)
    }
    while (out.size() + 2 <= maxbytes) { out.push_back(0x40); out.push_back(0x81); }  // fw-space pad
}

// Capture-inventory quality gate (authoring signal only — APPLY is KV-gated and
// unaffected). Real Tsumu text is hiragana/katakana/fullwidth-alnum/kanji heavy,
// whose SJIS lead bytes concentrate in the 0x81/0x82/0x83 and 0x88-0x9F/0xE0-0xEA
// rows; vertex/coordinate binary that passes the record reader by chance has
// diverse, out-of-row leads. Gate: (a) a minimum real length, (b) >= 2 real
// kana/fullwidth-alnum chars, and (c) the decoded 2-byte units are >= 80% in
// valid Shift-JIS text rows — so the always-on inventory stays a clean
// enumeration of drawn strings and binary noise is dropped.
bool sj_capture_worthy(const uint8_t* b, uint32_t n) {
    if (n < 4) return false;
    uint32_t units = 0, good = 0, kanafw = 0, unit_bytes = 0;
    for (uint32_t i = 0; i + 1 < n; ) {
        uint8_t c = b[i], d = b[i + 1];
        if (sj_lead(c) && sj_trail(d)) {
            ++units;
            // Valid Shift-JIS text rows: 0x81 symbols/punct, 0x82 hiragana +
            // fullwidth alnum, 0x83 katakana, 0x88-0x9F + 0xE0-0xEA kanji.
            if (c == 0x81 || c == 0x82 || c == 0x83 ||
                (c >= 0x88 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEA)) { ++good; unit_bytes += 2; }
            if (c == 0x83 || (c == 0x82 && d >= 0x4F)) ++kanafw;  // kana / fullwidth alnum
            i += 2;
        } else i += 1;
    }
    if (kanafw < 2) return false;                       // needs real JP / fullwidth content
    if (units == 0 || good * 5 < units * 4) return false;  // >= 80% of units in valid rows
    if (unit_bytes * 2 < n) return false;               // >= 50% of record is 2-byte SJIS text
                                                        // (drops long single-byte fill buffers)
    return true;
}

// Canonical record-START gate (capture only). A capture is ingested only when
// `va` is a genuine record boundary — the unit immediately preceding it is a
// terminator (NUL / 0xFFFF) or a region edge. This collapses the partial-offset
// explosion: a register that lands mid-string (or mid-binary-struct) is preceded
// by content bytes, not a terminator, so the SAME record is no longer re-ingested
// at every byte offset a register happens to point. APPLY is unaffected — it must
// still match the exact bytes the game draws at whatever offset the register
// carries, so this gate is never consulted on the apply path.
inline bool sj_at_record_start(uint8_t* ram, uint32_t va) {
    if (!va_in_ram(va - 1)) return true;                         // region edge
    uint8_t p1 = grb(ram, va - 1);
    if (p1 == 0x00) return true;                                 // after NUL terminator
    if (p1 == 0xFF && grb(ram, va - 2) == 0xFF) return true;     // after 0xFFFF terminator
    return false;                                                // mid-record offset — skip
}

// Struct-vs-text safety: a genuine message record contains only text units and
// the known control framing (0x0A newline; 0x10/0x30 page-break leads; 0xFC/0xFE/
// 0xFF framing; fullwidth-space pad). A fixed-width struct field packs binary
// params after the text (e.g. the "Tutorial N" label's trailing 09 08 01 02 F4),
// which shows up as low-control bytes 0x01..0x09 / 0x0B..0x0F / 0x11..0x1F.
// Replacing such a record corrupts the struct and derails the game — so apply is
// refused for records carrying those wild bytes even when a table entry exists.
bool sj_clean_text(const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t c = b[i];
        if (sj_lead(c) && i + 1 < n && sj_trail(b[i + 1])) { ++i; continue; }
        if (c == 0x0A || c == 0x10 || c == 0x30) continue;          // newline / page-break leads
        if (c >= 0x20) continue;                                     // text / framing / high bytes
        return false;                                                // wild low-control => struct
    }
    return true;
}

const EncodingProfile kShiftJisProfile = {
    "shift_jis", sj_first_byte_textish, sj_read_record, sj_transcode, sj_capture_worthy
};

// ===========================================================================
// Module state
// ===========================================================================
struct TableEntry {
    std::string target;   // active-language UTF-8 target (empty => fall back to source)
    Term        term = Term::Nul;
};
struct CapRec {
    uint8_t  bytes[96];
    uint8_t  sample_len;   // bytes stored in `bytes` (<=96)
    uint16_t full_len;     // full record length
    uint32_t addr;         // last source VA seen
    uint32_t pc;           // a dispatch target that carried it
    uint32_t ra;
    uint32_t first_frame;
    uint64_t count;
    Term     term;         // terminator that delimited the record (nul|ffff)
    bool     translated;
};

// A per-glyph label source-patch entry. Some UI text (level/stage names) is
// drawn glyph-by-glyph as sprites from a fixed-stride, space-padded table in the
// EXE — there is NO string pointer and NO terminator, so the dispatch/arg-scan
// message hook above can never see it. Instead we patch the label's bytes in
// guest RAM once the EXE region is resident: overwrite the confirmed slot with
// the target-language text (fullwidth-Latin Shift-JIS, little-endian) re-padded
// to the exact slot width, and the game's OWN per-glyph routine then draws
// English. Verify-before-patch (RAM must equal `src` first) guarantees we never
// write unless the exact expected JP label is present, and writing exactly
// `width` bytes (== the table stride) guarantees we never spill into the next
// slot. See docs/STRING_TRANSLATION.md Appendix Y.
struct GlyphLabel {
    uint32_t             addr = 0;      // slot base VA
    uint32_t             width = 0;     // slot stride in bytes (never exceeded)
    std::vector<uint8_t> src;           // expected JP source bytes (verify key)
    std::string          target;        // active-language UTF-8 (empty => skip)
    bool                 patched = false;
};

// A VRAM-strip patch entry. Some UI text is pre-rendered pixels inside a
// texture asset the game uploads to VRAM and samples with quads (Tsumu's HUD
// labels live in the glyph-sheet image) — no glyph codes exist anywhere, so
// neither the message hook nor the glyph-label patch can reach it. The patch
// rides the game's own upload path instead: whenever a GP0 0xA0 CPU→VRAM
// transfer completes and fully contains the patch rect, the rect is verified
// halfword-for-halfword against the expected JP pixels and, only on an exact
// match, rewritten with the target-language pixels through the renderer's own
// VRAM write facade. Because it re-applies on every matching upload it is a
// source-path patch (survives scene reloads / re-uploads), not a one-shot
// VRAM poke.
struct VramPatch {
    int                   x = 0, y = 0;  // rect origin, VRAM halfword coords
    int                   w = 0, h = 0;  // rect size (halfwords × rows)
    std::vector<uint16_t> src;           // expected JP pixels (verify key), w*h
    std::vector<uint16_t> rep;           // replacement pixels, w*h
    uint64_t              applied = 0;   // times applied (per-upload re-apply)
};

// Table-driven message record. Some UI text is addressed through a POINTER
// TABLE the renderer indexes internally — e.g. Tsumu's memory-card dialogs:
// show_card_dialog(id) loads table[id] and draws it, so the string pointer is
// never a dispatch argument and the a0..a3 message hook can never see it (nor
// can it be captured). The fix is the glyph-label pattern applied to an SJIS
// message record: verify the exact JP bytes are resident at a known address,
// then overwrite them IN PLACE with the transcoded target text (little-endian
// fullwidth-Shift-JIS body + the game's FE-FF line-break / 30-FC page-break /
// 10-FC prompt framing), terminated, never exceeding the source byte length —
// so the game's own message renderer reads the translation. Verify-before-
// patch + the length cap guarantee no corruption; idempotent (a patched slot
// no longer matches `src`). Authored via a [[entry]] carrying a `ram_addr`.
struct MsgInplace {
    uint32_t             addr = 0;      // message record VA (game data region)
    std::vector<uint8_t> src;           // expected JP record bytes (verify key)
    std::string          target;        // active-language UTF-8
    Term                 term = Term::Nul; // record terminator (nul / ffff)
    bool                 patched = false;
};

const EncodingProfile* g_prof = &kShiftJisProfile;

std::unordered_map<uint64_t, TableEntry> g_table;      // hash -> translation
std::unordered_map<uint64_t, CapRec>     g_inv;        // hash -> capture record
std::vector<GlyphLabel>                  g_glyph_labels;// per-glyph RAM patches
std::atomic<int>                         g_glyph_pending{0}; // unpatched count (0 => idle)
std::vector<VramPatch>                   g_vram_patches; // pre-rendered strip patches
std::atomic<int>                         g_vram_patch_n{0};  // count (0 => upload hook idle)
std::atomic<int>                         g_table_n{0};       // string entries (0 => dispatch scan idle)
std::vector<MsgInplace>                  g_msg_inplace; // table-driven message RAM patches
std::atomic<int>                         g_msg_inplace_pending{0}; // unpatched count (0 => idle)
// Card-manager messages are packed into NUL-terminated *chunks*; within a chunk,
// distinct messages (each with its own pointer-table entry) are separated ONLY by
// runs of the fullwidth space (0x8140), with no terminator between them. The
// game's renderer reads a message from its pointer until the next NUL, so any
// non-last message in a chunk bleeds through its neighbours into the chunk tail
// (e.g. the "Reading Card" / "Saving" status messages rendered the format prompt
// and its Yes/No wait). A separator drops a NUL at the start of the trailing
// space-run so each message reads to its own terminator. Verify-then-patch: only
// a resident fullwidth space is ever overwritten, so drift can never corrupt text.
struct NulSep { uint32_t addr = 0; bool patched = false; };
std::vector<NulSep>                      g_msg_seps;    // inter-message NUL separators
std::atomic<int>                         g_msg_sep_pending{0}; // unpatched count (0 => idle)
std::mutex g_mtx;

std::atomic<bool>     g_apply_armed{false};   // table non-empty AND language enabled
/* The capture inventory is an AUTHORING tool: it ingests every source string the
 * game draws so a translator can enumerate them. It is not needed to APPLY a
 * shipped translation (that is g_apply_armed, decided by the loaded tables).
 *
 * It is not free. text_xlate_on_dispatch runs at every dispatch, and with
 * capture on it scans a0..a3, classifies each as text-ish, reads the record and
 * hashes it. On the WipEout 3 ntscfull8 mod (~33k dispatches/frame) that was the
 * single largest CPU consumer in the intro FMV — gprofng put it at 18.6% of
 * samples, and disabling it measured +22% fps over guest frames 800-2200.
 *
 * So: on by default only where the rest of the authoring/debug tooling lives,
 * off in a release build. PSX_XLATE_CAPTURE=1 turns it back on for authoring;
 * PSX_XLATE_CAPTURE=0 turns it off in a debug-tools build. */
#ifdef PSX_NO_DEBUG_TOOLS
std::atomic<bool>     g_capture_on{false};    // release: authoring inventory off
#else
std::atomic<bool>     g_capture_on{true};     // debug-tools build: always-on
#endif
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_hits{0};
std::string           g_lang = "en";
std::string           g_dir;                  // translations/ directory (for reload)

// Recently-written scratch VAs — so our own transcoded (fullwidth-SJIS) English
// scratch isn't re-captured as a bogus "Japanese" record.
uint32_t g_scratch_ring[32] = {0};
std::atomic<uint32_t> g_scratch_pos{0};
bool scratch_recent(uint32_t va) {
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t s = g_scratch_ring[i];
        if (s && (va >= s) && (va < s + kSrcMax + 4)) return true;
    }
    return false;
}
void scratch_note(uint32_t va) {
    g_scratch_ring[g_scratch_pos.fetch_add(1, std::memory_order_relaxed) & 31u] = va;
}

// ---------------------------------------------------------------------------
// Table loading (all translations/*.toml). Hot-reloadable via debug "reload".
// ---------------------------------------------------------------------------
std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> b;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hv(hex[i]), lo = hv(hex[i+1]);
        if (hi < 0 || lo < 0) { b.clear(); break; }
        b.push_back((uint8_t)((hi << 4) | lo));
    }
    return b;
}

// hex string -> little-endian halfwords (VRAM pixel stream order).
std::vector<uint16_t> hex_to_halfwords(const std::string& hex) {
    auto b = hex_to_bytes(hex);
    std::vector<uint16_t> hw;
    if (b.empty() || (b.size() & 1)) return hw;
    hw.reserve(b.size() / 2);
    for (size_t i = 0; i + 1 < b.size(); i += 2)
        hw.push_back((uint16_t)(b[i] | (b[i + 1] << 8)));
    return hw;
}

void load_tables_locked() {
    g_table.clear();
    g_glyph_labels.clear();
    g_glyph_pending.store(0, std::memory_order_relaxed);
    g_vram_patches.clear();
    g_vram_patch_n.store(0, std::memory_order_relaxed);
    g_table_n.store(0, std::memory_order_relaxed);
    g_msg_inplace.clear();
    g_msg_inplace_pending.store(0, std::memory_order_relaxed);
    g_msg_seps.clear();
    g_msg_sep_pending.store(0, std::memory_order_relaxed);
    for (auto& kv : g_inv) kv.second.translated = false;
    if (g_dir.empty() || !fs::exists(g_dir)) { g_apply_armed.store(false); return; }
    const bool lang_off = g_lang.empty() || g_lang == "jp" || g_lang == "off";
    size_t files = 0, entries = 0, glyphs = 0, vpatches = 0;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(g_dir, ec)) {
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".toml") continue;
        toml::value data;
        try { data = toml::parse(de.path().string()); }
        catch (...) { continue; }
        ++files;
        /* A fixes-only file (vram_patch / glyph-label / msg blocks, no string
         * table) is valid: gate each block on its own key, never the file. */
        const auto arr = data.contains("entry") ? toml::find(data, "entry")
                                                : toml::value(toml::array{});
        if (arr.is_array()) for (const auto& e : arr.as_array()) {
            if (!e.contains("src_hex")) continue;
            std::string hex = toml::find_or<std::string>(e, "src_hex", "");
            auto bytes = hex_to_bytes(hex);
            if (bytes.empty()) continue;
            std::string tgt;
            if (e.contains(g_lang)) tgt = toml::find_or<std::string>(e, g_lang, "");
            else if (e.contains("en")) tgt = toml::find_or<std::string>(e, "en", "");
            if (tgt.empty()) continue;  // no translation for this lang => source shown
            Term term = Term::Nul;
            std::string ts = toml::find_or<std::string>(e, "term", "nul");
            if (ts == "ffff" || ts == "FFFF") term = Term::FFFF;
            else if (ts == "none") term = Term::None;  // mid-blob sub-record: no
                                                       // terminator write (content follows)
            // Optional in-place RAM patch: a record the game addresses through a
            // pointer table (never a dispatch arg) can't be reached by the a0..a3
            // hook, so overwrite the JP bytes in place at `ram_addr`. The entry
            // still joins g_table (harmless if the record is also ever passed).
            if (e.contains("ram_addr")) {
                uint32_t ra = (uint32_t)toml::find_or<int64_t>(e, "ram_addr", 0);
                if (ra) g_msg_inplace.push_back(MsgInplace{ ra, bytes, tgt, term, false });
            }
            uint64_t key = fnv1a(bytes.data(), (uint32_t)bytes.size());
            g_table[key] = TableEntry{ std::move(tgt), term };
            ++entries;
        }
        // Per-glyph label source-patch entries (RAM-patch layer). Only load when
        // a target for the active language exists; otherwise the slot stays JP.
        if (!lang_off && data.contains("glyph_label")) {
            const auto& garr = toml::find(data, "glyph_label");
            if (garr.is_array()) for (const auto& g : garr.as_array()) {
                if (!g.contains("addr") || !g.contains("src_hex")) continue;
                std::string ghex = toml::find_or<std::string>(g, "src_hex", "");
                auto gbytes = hex_to_bytes(ghex);
                if (gbytes.empty()) continue;
                std::string tgt;
                if (g.contains(g_lang)) tgt = toml::find_or<std::string>(g, g_lang, "");
                else if (g.contains("en")) tgt = toml::find_or<std::string>(g, "en", "");
                if (tgt.empty()) continue;   // no translation for this lang => slot stays JP
                GlyphLabel gl;
                gl.addr   = (uint32_t)toml::find_or<int64_t>(g, "addr", 0);
                gl.width  = (uint32_t)toml::find_or<int64_t>(g, "width", (int64_t)gbytes.size());
                if (gl.addr == 0 || gl.width == 0 || gbytes.size() > gl.width) continue;
                gl.src    = std::move(gbytes);
                gl.target = std::move(tgt);
                g_glyph_labels.push_back(std::move(gl));
                ++glyphs;
            }
        }
        // VRAM-strip patch entries (pre-rendered-pixel layer). The replacement
        // pixels come from a per-language "<lang>_hex" key ("en_hex" fallback);
        // no key for the active language => the strip stays JP.
        if (!lang_off && data.contains("vram_patch")) {
            const auto& varr = toml::find(data, "vram_patch");
            if (varr.is_array()) for (const auto& v : varr.as_array()) {
                if (!v.contains("src_hex")) continue;
                VramPatch vp;
                vp.x = (int)toml::find_or<int64_t>(v, "x", -1);
                vp.y = (int)toml::find_or<int64_t>(v, "y", -1);
                vp.w = (int)toml::find_or<int64_t>(v, "w", 0);
                vp.h = (int)toml::find_or<int64_t>(v, "h", 0);
                if (vp.x < 0 || vp.y < 0 || vp.w <= 0 || vp.h <= 0) continue;
                if (vp.x + vp.w > 1024 || vp.y + vp.h > 512) continue;
                vp.src = hex_to_halfwords(toml::find_or<std::string>(v, "src_hex", ""));
                std::string rk = g_lang + "_hex";
                if (v.contains(rk)) vp.rep = hex_to_halfwords(toml::find_or<std::string>(v, rk, ""));
                else if (v.contains("en_hex")) vp.rep = hex_to_halfwords(toml::find_or<std::string>(v, "en_hex", ""));
                size_t need = (size_t)vp.w * (size_t)vp.h;
                if (vp.src.size() != need || vp.rep.size() != need) continue;
                g_vram_patches.push_back(std::move(vp));
                ++vpatches;
            }
        }
        // Inter-message NUL separators (structure fix for packed message chunks).
        // Each entry names a VA that currently holds a fullwidth space at the start
        // of an inter-message gap; a NUL is dropped there so the preceding message
        // reads to its own terminator instead of bleeding into its neighbour.
        if (!lang_off && data.contains("msg_sep")) {
            const auto& sarr = toml::find(data, "msg_sep");
            if (sarr.is_array()) for (const auto& s : sarr.as_array()) {
                if (!s.contains("addr")) continue;
                uint32_t a = (uint32_t)toml::find_or<int64_t>(s, "addr", 0);
                if (a) g_msg_seps.push_back(NulSep{ a, false });
            }
        }
    }
    g_table_n.store((int)g_table.size(), std::memory_order_relaxed);
    g_glyph_pending.store((int)g_glyph_labels.size(), std::memory_order_relaxed);
    g_vram_patch_n.store((int)g_vram_patches.size(), std::memory_order_relaxed);
    g_msg_inplace_pending.store((int)g_msg_inplace.size(), std::memory_order_relaxed);
    g_msg_sep_pending.store((int)g_msg_seps.size(), std::memory_order_relaxed);
    g_apply_armed.store(!lang_off && (!g_table.empty() || !g_glyph_labels.empty() ||
                                      !g_vram_patches.empty() || !g_msg_inplace.empty() ||
                                      !g_msg_seps.empty()),
                        std::memory_order_relaxed);
    // Mark inventory records that now have a translation.
    for (auto& kv : g_inv)
        kv.second.translated = (g_table.find(kv.first) != g_table.end());
    std::fprintf(stderr,
        "[xlate] loaded %zu entries + %zu glyph-labels + %zu vram-patches + %zu msg-inplace from %zu file(s) in %s (lang=%s apply=%s)\n",
        entries, glyphs, vpatches, g_msg_inplace.size(), files, g_dir.c_str(), g_lang.c_str(),
        g_apply_armed.load() ? "on" : "off");
}

// ---------------------------------------------------------------------------
// Capture inventory upsert.
// ---------------------------------------------------------------------------
void inv_upsert_locked(uint64_t key, const uint8_t* bytes, uint32_t len,
                       uint32_t addr, uint32_t pc, uint32_t ra, Term term) {
    auto it = g_inv.find(key);
    if (it != g_inv.end()) { it->second.count++; it->second.addr = addr;
                             it->second.pc = pc; it->second.ra = ra; return; }
    if (g_inv.size() >= 60000) return;  // sane bound
    CapRec r{};
    uint32_t n = len < 96 ? len : 96;
    std::memcpy(r.bytes, bytes, n);
    r.sample_len = (uint8_t)n; r.full_len = (uint16_t)len; r.term = term;
    r.addr = addr; r.pc = pc; r.ra = ra;
    r.first_frame = (uint32_t)s_frame_count; r.count = 1;
    r.translated = (g_table.find(key) != g_table.end());
    g_inv[key] = r;
}

// ---------------------------------------------------------------------------
// Glyph-label RAM source-patch: scan the label table and patch any slot whose
// confirmed JP source bytes are currently resident in guest RAM. Idempotent —
// a patched slot no longer matches `src`, so it is written exactly once. Runs
// under g_mtx.
// ---------------------------------------------------------------------------
void glyph_labels_patch_locked(uint8_t* ram) {
    int pending = 0;
    for (auto& gl : g_glyph_labels) {
        if (gl.patched || gl.target.empty() || gl.width == 0) continue;
        // Verify-before-patch: the exact expected JP label must be resident and
        // the whole slot must fit in RAM. If not, the region isn't loaded yet —
        // leave it JP and try again on a later scan (never corrupt).
        if (!va_in_ram(gl.addr) || !va_in_ram(gl.addr + gl.width - 1)) { ++pending; continue; }
        bool match = gl.src.size() <= gl.width;
        for (uint32_t i = 0; match && i < gl.src.size(); ++i)
            if (grb(ram, gl.addr + i) != gl.src[i]) match = false;
        if (!match) { ++pending; continue; }
        std::vector<uint8_t> enc;
        label_transcode_le(gl.target, gl.width, enc);
        if (enc.empty() || enc.size() > gl.width) { ++pending; continue; }  // paranoia
        for (uint32_t i = 0; i < enc.size(); ++i) gwb(ram, gl.addr + i, enc[i]);
        bless_patch(ram, gl.addr, (uint32_t)enc.size());
        gl.patched = true;
    }
    g_glyph_pending.store(pending, std::memory_order_relaxed);
}

// Build a message body: little-endian fullwidth-Shift-JIS (as the message-table
// drawer reads it) with the author's \n/\f/\r framing tokens mapped to the
// game's control codes (FE-FF line break, 30-FC page break, 10-FC prompt).
// Mirrors sj_transcode's glyph mapping + apply_to_reg's framing, fused for the
// in-place path (which writes the framed bytes directly rather than repointing).
void msg_inplace_build(const std::string& utf8, std::vector<uint8_t>& out) {
    out.clear();
    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = (uint8_t)utf8[i];
        uint32_t cp; size_t adv;
        if (c < 0x80)                    { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (utf8[i+1] & 0x3F); adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i+1] & 0x3F) << 6) | (utf8[i+2] & 0x3F); adv = 3;
        } else                           { cp = ' '; adv = 1; }
        i += adv;
        if (cp == '\n') { out.push_back(0xFE); out.push_back(0xFF); continue; }  // line break
        if (cp == 0x0C) { out.push_back(0x30); out.push_back(0xFC); continue; }  // \f page break
        if (cp == 0x0D) { out.push_back(0x10); out.push_back(0xFC); continue; }  // \r prompt/wait
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp = cp - 0xFF00 + 0x20;   // fullwidth Latin -> ASCII
        if (cp == 0x3000) cp = ' ';
        uint16_t g = fw_sjis_for_ascii(cp <= 0x7E ? cp : ' ');
        out.push_back((uint8_t)(g & 0xFF));   // little-endian storage
        out.push_back((uint8_t)(g >> 8));
    }
}

// Table-driven message in-place RAM patch. Mirrors glyph_labels_patch_locked
// but for SJIS message records. Runs under g_mtx.
void msg_inplace_patch_locked(uint8_t* ram) {
    int pending = 0;
    for (auto& m : g_msg_inplace) {
        if (m.patched || m.target.empty() || m.src.empty()) continue;
        uint32_t len = (uint32_t)m.src.size();
        if (!va_in_ram(m.addr) || !va_in_ram(m.addr + len - 1)) { ++pending; continue; }
        // Verify-before-patch: the exact JP record must be resident (else the
        // region isn't loaded yet, or it's already patched — never corrupt).
        bool match = true;
        for (uint32_t i = 0; match && i < len; ++i)
            if (grb(ram, m.addr + i) != m.src[i]) match = false;
        if (!match) { ++pending; continue; }
        std::vector<uint8_t> body;
        msg_inplace_build(m.target, body);
        // Structure-preserving in-place patch. The author writes the SAME control
        // framing as the source (\n \f \r), so an English body sized to the exact
        // source length reproduces the record's control layout — critically the
        // trailing \r (0x10-FC prompt/wait) an interactive dialog needs to accept
        // input and advance. Truncate to whole 2-byte cells if over-long, then pad
        // with the fullwidth space to the source length so no stale JP tail
        // remains, and reuse the record's own terminator slot (offset len) so the
        // NUL/0xFFFF lands exactly where the game expects it.
        while (!body.empty() && body.size() > len) body.resize(body.size() - 2);
        if (body.empty()) { m.patched = true; continue; }  // nothing fit; don't spin
        // term=none marks a mid-blob sub-record (e.g. a Yes/No prompt whose \r is
        // followed by another message in the same NUL/FFFF span): write the body
        // exactly, WITHOUT padding or a terminator, so the following content is
        // left intact. Otherwise pad to the record length and stamp the terminator
        // in the record's own terminator slot.
        if (m.term != Term::None)
            while (body.size() + 2 <= len) { body.push_back(0x40); body.push_back(0x81); }
        for (uint32_t i = 0; i < body.size(); ++i) gwb(ram, m.addr + i, body[i]);
        uint32_t ti = (uint32_t)body.size();
        if (m.term == Term::FFFF) {
            if (va_in_ram(m.addr + ti + 1)) { gwb(ram, m.addr + ti, 0xFF); gwb(ram, m.addr + ti + 1, 0xFF); }
        } else if (m.term == Term::Nul && va_in_ram(m.addr + ti)) {
            gwb(ram, m.addr + ti, 0x00);
        }
        // Bless the whole written span (body + up to a 2-byte terminator) so the
        // guard sees the patched record as intentional data, not modified code.
        bless_patch(ram, m.addr, ti + 2);
        m.patched = true;
    }
    g_msg_inplace_pending.store(pending, std::memory_order_relaxed);
}

// Drop an end-of-message terminator (0xFFFF) at each inter-message separator VA
// once the region is resident. The text renderer (FUN at 0x80058xxx, message
// pointer at gp+0x26c) stops a message at the \r prompt-wait (0xFC10) or the
// 0xFFFF end marker — NOT at 0x0000 (that byte is drawn as a glyph). 0xFFFF ends
// the message with no input-wait (as the game's own term=ffff status messages
// do), so the preceding packed message reads to its own terminator instead of
// bleeding through its neighbours to the next \r. Verify-then-patch: the two
// bytes must currently be a fullwidth space (LE 0x40 0x81) — never overwrite
// text — so an already-patched or drifted slot is simply skipped.
void msg_seps_patch_locked(uint8_t* ram) {
    int pending = 0;
    for (auto& s : g_msg_seps) {
        if (s.patched) continue;
        if (!va_in_ram(s.addr) || !va_in_ram(s.addr + 1)) { ++pending; continue; }
        if (grb(ram, s.addr) != 0x40 || grb(ram, s.addr + 1) != 0x81) { ++pending; continue; }
        gwb(ram, s.addr,     0xFF);
        gwb(ram, s.addr + 1, 0xFF);
        bless_patch(ram, s.addr, 2);
        s.patched = true;
    }
    g_msg_sep_pending.store(pending, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// VRAM-strip patch: verify the rect holds the exact expected JP pixels, then
// rewrite it with the target-language pixels through the renderer facade.
// `force` skips the upload-containment gate (debug "vpatch" re-apply). Runs
// under g_mtx. Returns patches applied this call.
// ---------------------------------------------------------------------------
int vram_patches_apply_locked(int ux, int uy, int uw, int uh, bool force) {
    int applied = 0;
    for (auto& vp : g_vram_patches) {
        if (!force &&
            (vp.x < ux || vp.y < uy || vp.x + vp.w > ux + uw || vp.y + vp.h > uy + uh))
            continue;
        // Verify-before-patch: every halfword must match the expected JP
        // pixels. A different asset at the same coords (or an already-patched
        // strip) fails the compare and is left untouched.
        bool match = true;
        for (int r = 0; match && r < vp.h; ++r)
            for (int c = 0; match && c < vp.w; ++c)
                if (gr_vram_read(vp.x + c, vp.y + r) != vp.src[(size_t)r * vp.w + c])
                    match = false;
        if (!match) continue;
        for (int r = 0; r < vp.h; ++r)
            for (int c = 0; c < vp.w; ++c)
                gr_vram_write(vp.x + c, vp.y + r, vp.rep[(size_t)r * vp.w + c]);
        vp.applied++;
        ++applied;
    }
    return applied;
}

// ---------------------------------------------------------------------------
// Apply: transcode + write guest scratch + repoint the arg register.
// ---------------------------------------------------------------------------
bool apply_to_reg(uint8_t* ram, CPUState* cpu, uint32_t sp, uint32_t* reg,
                  const TableEntry& te, Term src_term) {
    std::vector<uint8_t> enc;
    g_prof->transcode(te.target, enc);
    if (enc.empty()) return false;
    Term t = (te.term != Term::Nul) ? te.term : src_term;  // preserve source terminator
    // For framed (0xFFFF) messages the game's line break is the "FE FF" code, not
    // a raw 0x0A — remap so multi-line English reflows in the text box.
    std::vector<uint8_t> body;
    body.reserve(enc.size() + 16);
    for (uint8_t c : enc) {
        if      (c == 0x0A && t == Term::FFFF) { body.push_back(0xFE); body.push_back(0xFF); }  // line break
        else if (c == 0x0A)                    { /* NUL record: drop stray newline */ }
        else if (c == 0x0C) { body.push_back(0x30); body.push_back(0xFC); }  // \f page break
        else if (c == 0x0D) { body.push_back(0x10); body.push_back(0xFC); }  // \r prompt/wait
        else                 body.push_back(c);
    }
    if (body.empty() || body.size() > kSrcMax) return false;
    // Scratch below the caller's $sp, 8-byte aligned, with headroom.
    if (sp < 0x80001000u) return false;
    uint32_t scratch = (sp - 0x900u) & ~7u;
    if (!va_in_ram(scratch) || !va_in_ram(scratch + (uint32_t)body.size() + 2)) return false;
    for (size_t i = 0; i < body.size(); ++i) gwb(ram, scratch + (uint32_t)i, body[i]);
    if (t == Term::FFFF) { gwb(ram, scratch + (uint32_t)body.size(),     0xFF);
                           gwb(ram, scratch + (uint32_t)body.size() + 1, 0xFF); }
    else                 { gwb(ram, scratch + (uint32_t)body.size(),     0x00); }
    scratch_note(scratch);
    *reg = scratch;
    return true;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================
extern "C" void text_xlate_on_dispatch(CPUState* cpu, uint32_t target) {
    const bool cap = g_capture_on.load(std::memory_order_relaxed);
    const bool app = g_apply_armed.load(std::memory_order_relaxed);
    if (!cap && !app) return;
    /* g_apply_armed covers EVERY apply mechanism, but the only apply work this
     * DISPATCH hook performs is string-arg swapping plus the throttled
     * in-place RAM patches; vram_patch blocks apply through the VRAM upload
     * hook instead. A fixes-only translation set (WipEout 3 ships one for the
     * track palette gap) used to leave this armed with an EMPTY string table,
     * and the a0..a3 record scan below then hashed four registers on every
     * dispatch against a table that could never hit -- 20% of a stock FMV run.
     * No dispatch-relevant work => stay out of the hot path entirely. */
    if (!cap &&
        g_table_n.load(std::memory_order_relaxed) == 0 &&
        g_glyph_pending.load(std::memory_order_relaxed) <= 0 &&
        g_msg_inplace_pending.load(std::memory_order_relaxed) <= 0 &&
        g_msg_sep_pending.load(std::memory_order_relaxed) <= 0)
        return;
    uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return;
    uint64_t call_n = g_calls.fetch_add(1, std::memory_order_relaxed);

    // Per-glyph label RAM source-patch (load-time). Runs while any label is still
    // unpatched — i.e. until the EXE label region becomes resident and every
    // confirmed slot has been overwritten. Once g_glyph_pending hits 0 this is a
    // single relaxed load on the hot path. Throttled so the (cheap) verify scan
    // costs nothing per-dispatch before the region loads.
    if (app && g_glyph_pending.load(std::memory_order_relaxed) > 0 &&
        (call_n & 0x1FFu) == 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        glyph_labels_patch_locked(ram);
    }
    // Table-driven message records (never a dispatch arg): patch in place while
    // any is still unpatched. Same cheap throttle as glyph labels.
    if (app && g_msg_inplace_pending.load(std::memory_order_relaxed) > 0 &&
        (call_n & 0x1FFu) == 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        msg_inplace_patch_locked(ram);
    }
    // Inter-message NUL separators: same cheap throttle; idle once all placed.
    if (app && g_msg_sep_pending.load(std::memory_order_relaxed) > 0 &&
        (call_n & 0x1FFu) == 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        msg_seps_patch_locked(ram);
    }

    const uint32_t sp = cpu->gpr[29];
    // Scan the argument registers a0..a3 for source-text pointers. KV-gated
    // apply means swapping only the reg that pointed at a KNOWN record — a
    // coincidental non-text arg won't be in the table, so it is never touched.
    uint32_t* argregs[4] = { &cpu->gpr[4], &cpu->gpr[5], &cpu->gpr[6], &cpu->gpr[7] };
    for (int a = 0; a < 4; ++a) {
        uint32_t va = *argregs[a];
        if (!va_in_ram(va)) continue;
        if (!g_prof->first_byte_textish(grb(ram, va))) continue;
        if (scratch_recent(va)) continue;  // don't recapture our own English scratch
        uint8_t buf[kSrcMax]; uint32_t len = 0; Term term = Term::None;
        if (!g_prof->read_record(ram, va, buf, &len, &term)) continue;
        uint64_t key = fnv1a(buf, len);

        // CAPTURE (always-on inventory): ingest each real string ONCE at its
        // genuine record start, and only if it clears the noise gate. The
        // canonical-start test collapses the partial-offset explosion (the same
        // record was previously re-ingested at every offset a register landed on).
        // APPLY below is deliberately NOT gated by record-start — it matches the
        // exact bytes the game draws wherever the pointer lands.
        if (cap && sj_at_record_start(ram, va) &&
            (!g_prof->capture_worthy || g_prof->capture_worthy(buf, len))) {
            std::lock_guard<std::mutex> lk(g_mtx);
            inv_upsert_locked(key, buf, len, va, target, cpu->gpr[31], term);
        }
        if (app) {
            // Safety: by default only substitute standalone framed (0xFFFF)
            // messages. NUL-terminated records are frequently fixed-width struct
            // fields with binary params packed after the text (e.g. the "Tutorial
            // N" label carries trailing coordinate bytes); replacing those
            // corrupts the struct and derails the game. A NUL record is only
            // safe when its table entry opts in with term="nul" AND matches
            // byte-for-byte (the author vetted it). PSX_XLATE_ALLOW_NUL=1 lifts
            // the gate for experimentation.
            TableEntry te;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                auto it = g_table.find(key);
                if (it == g_table.end()) continue;
                te = it->second;
            }
            // The table entry is author-vetted, but guard against corrupting a
            // struct that shares the key: apply only when the source record is
            // framed (0xFFFF, always standalone) OR pure clean text (no wild
            // binary params). PSX_XLATE_ALLOW_NUL=1 forces through for debugging.
            static const bool allow_nul = [] {
                const char* e = std::getenv("PSX_XLATE_ALLOW_NUL"); return e && e[0] == '1'; }();
            if (term != Term::FFFF && !allow_nul && !sj_clean_text(buf, len)) continue;
            if (apply_to_reg(ram, cpu, sp, argregs[a], te, term))
                g_hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

extern "C" void text_xlate_vram_upload(int x, int y, int w, int h) {
    // Hot path: one relaxed load when no patches are configured / lang off.
    if (g_vram_patch_n.load(std::memory_order_relaxed) <= 0) return;
    if (!g_apply_armed.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    vram_patches_apply_locked(x, y, w, h, false);
}

extern "C" void text_xlate_init(const char* project_root, const char* language) {
    const char* env = std::getenv("PSX_LANG");
    if (env && *env) g_lang = env;
    else if (language && *language) g_lang = language;
    if (project_root && *project_root)
        g_dir = (fs::path(project_root) / "translations").string();
    /* Explicit env wins in both directions over the build-flavour default. */
    const char* capenv = std::getenv("PSX_XLATE_CAPTURE");
    if (capenv && capenv[0]) g_capture_on.store(capenv[0] != '0');
    std::lock_guard<std::mutex> lk(g_mtx);
    load_tables_locked();
}

extern "C" void text_xlate_set_language(const char* language) {
    // PSX_LANG env is an authoring/testing override — if set it pins the language
    // and the launcher's choice is ignored (matches text_xlate_init precedence).
    const char* env = std::getenv("PSX_LANG");
    std::string want = (env && *env) ? env : (language ? language : "");
    std::lock_guard<std::mutex> lk(g_mtx);
    if (want == g_lang) return;      // no change => skip the reload
    g_lang = want;
    load_tables_locked();            // re-selects the target column + re-arms APPLY
}

extern "C" int text_xlate_debug_json(const char* subcmd, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::string sc = subcmd ? subcmd : "stats";
    std::lock_guard<std::mutex> lk(g_mtx);

    if (sc == "reload") { load_tables_locked();
        return std::snprintf(out, cap, "{\"reloaded\":true,\"entries\":%zu}", g_table.size()); }

    if (sc == "stats") {
        size_t gp = 0; for (auto& gl : g_glyph_labels) if (gl.patched) ++gp;
        uint64_t va = 0; for (auto& vp : g_vram_patches) va += vp.applied;
        size_t mp = 0; for (auto& m : g_msg_inplace) if (m.patched) ++mp;
        return std::snprintf(out, cap,
            "{\"lang\":\"%s\",\"apply\":%s,\"capture\":%s,\"table_entries\":%zu,"
            "\"distinct_captured\":%zu,\"calls\":%llu,\"hits\":%llu,"
            "\"glyph_labels\":%zu,\"glyph_patched\":%zu,\"glyph_pending\":%d,"
            "\"vram_patches\":%zu,\"vram_applied\":%llu,"
            "\"msg_inplace\":%zu,\"msg_inplace_patched\":%zu,\"msg_inplace_pending\":%d}",
            g_lang.c_str(), g_apply_armed.load() ? "true" : "false",
            g_capture_on.load() ? "true" : "false",
            g_table.size(), g_inv.size(),
            (unsigned long long)g_calls.load(), (unsigned long long)g_hits.load(),
            g_glyph_labels.size(), gp, g_glyph_pending.load(),
            g_vram_patches.size(), (unsigned long long)va,
            g_msg_inplace.size(), mp, g_msg_inplace_pending.load());
    }

    // vpatch: force-apply every VRAM-strip patch against current VRAM content
    // (verify still gates each one) and report per-patch status. Lets a freshly
    // reloaded patch be exercised without waiting for the game to re-upload.
    if (sc == "vpatch") {
        int n = g_apply_armed.load() ? vram_patches_apply_locked(0, 0, 0, 0, true) : 0;
        int w = 0;
        w += std::snprintf(out + w, cap - w, "{\"applied_now\":%d,\"patches\":[", n);
        bool first = true;
        for (auto& vp : g_vram_patches) {
            if (w > cap - 160) break;
            if (!first) w += std::snprintf(out + w, cap - w, ",");
            first = false;
            w += std::snprintf(out + w, cap - w,
                "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"applied\":%llu}",
                vp.x, vp.y, vp.w, vp.h, (unsigned long long)vp.applied);
        }
        w += std::snprintf(out + w, cap - w, "]}");
        return w;
    }

    // glyph: per-label patch status (JP addr -> patched? + target). Observability
    // for the RAM source-patch layer.
    if (sc == "glyph") {
        int w = 0;
        w += std::snprintf(out + w, cap - w, "[");
        bool first = true;
        for (auto& gl : g_glyph_labels) {
            if (w > cap - 256) break;
            if (!first) w += std::snprintf(out + w, cap - w, ",");
            first = false;
            w += std::snprintf(out + w, cap - w,
                "{\"addr\":\"%08x\",\"width\":%u,\"patched\":%s,\"en\":\"%s\"}",
                gl.addr, gl.width, gl.patched ? "true" : "false", gl.target.c_str());
        }
        w += std::snprintf(out + w, cap - w, "]");
        return w;
    }

    // dump / todo: JSON array of captured records.
    const bool todo_only = (sc == "todo");
    int w = 0;
    w += std::snprintf(out + w, cap - w, "[");
    bool first = true;
    for (auto& kv : g_inv) {
        const CapRec& r = kv.second;
        if (todo_only && r.translated) continue;
        if (w > cap - 512) break;   // leave room; truncate cleanly
        if (!first) w += std::snprintf(out + w, cap - w, ",");
        first = false;
        w += std::snprintf(out + w, cap - w,
            "{\"hash\":\"%016llx\",\"addr\":\"%08x\",\"pc\":\"%08x\",\"ra\":\"%08x\","
            "\"len\":%u,\"count\":%llu,\"term\":\"%s\",\"xl\":%s,\"hex\":\"",
            (unsigned long long)kv.first, r.addr, r.pc, r.ra,
            (unsigned)r.full_len, (unsigned long long)r.count,
            r.term == Term::FFFF ? "ffff" : "nul",
            r.translated ? "true" : "false");
        for (uint32_t i = 0; i < r.sample_len && w < cap - 4; ++i)
            w += std::snprintf(out + w, cap - w, "%02x", r.bytes[i]);
        w += std::snprintf(out + w, cap - w, "\"}");
    }
    w += std::snprintf(out + w, cap - w, "]");
    return w;
}
