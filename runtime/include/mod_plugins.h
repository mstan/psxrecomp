#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSXModVBlankCallback)(void);
typedef void (*PSXModActivationCallback)(void);
struct CPUState;
typedef void (*PSXModFunctionEntryCallback)(struct CPUState* cpu,
                                            uint32_t address);

/*
 * Register a trusted, statically linked plugin implementation. Package
 * manifests select implementations by this stable id; archives never provide
 * native code or symbol names. Activation / VBlank / function-entry ids all
 * count as registered for package resolve; function-entry callbacks only run
 * when the committed plan lists that id.
 */
int psx_mod_register_activation_plugin(const char* id,
                                       PSXModActivationCallback callback);
int psx_mod_register_vblank_plugin(const char* id,
                                   PSXModVBlankCallback callback);
int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback);
/* Called only from generated functions explicitly listed by the game config. */
void psx_mod_function_entry(struct CPUState* cpu, uint32_t address);

/* Narrow guest services available to trusted plugin callbacks. */
int psx_mod_game_started(void);
uint8_t psx_mod_read_byte(uint32_t address);
void psx_mod_write_byte(uint32_t address, uint8_t value);
uint16_t psx_mod_read_half(uint32_t address);
void psx_mod_write_half(uint32_t address, uint16_t value);
uint32_t psx_mod_read_word(uint32_t address);
void psx_mod_write_word(uint32_t address, uint32_t value);
/*
 * Replace one guest instruction and route that address through the runtime's
 * executable-RAM path. Use this instead of psx_mod_write_word for code so a
 * restored save state cannot leave the compiled instruction stale.
 */
void psx_mod_write_code_word(uint32_t address, uint32_t value);

/*
 * Allocate opt-in enhancement memory from Expansion 1. Until the first
 * allocation, the region remains hardware-faithful open bus. The returned
 * KSEG0 address is accessible through normal generated guest loads.
 */
uint32_t psx_mod_alloc_guest_memory(uint32_t size, uint32_t alignment);

/*
 * Allocate guest memory that is also addressable by 24-bit GPU linked-list
 * tags. This is intended for opt-in enhanced primitive/ordering-table arenas;
 * without an allocation the aperture remains unmapped and DMA stays faithful.
 */
uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t size, uint32_t alignment);

/* Current per-side widescreen reveal in native game pixels (zero at 4:3). */
int32_t psx_mod_widescreen_x_margin(void);

/*
 * Read the committed value of one of this package's declared options, as the
 * player left it in the launcher (or the manifest default when untouched).
 * Writes a NUL-terminated string into `out` and returns 1; returns 0 with
 * out[0] = '\0' when the plan is not committed, the ids do not resolve, or the
 * value does not fit — the caller then applies its own default rather than
 * treating an empty string as a selection.
 *
 * Why this exists: the manifest schema already carries typed, validated,
 * launcher-rendered, persisted options ([[option]] boolean/choice/integer), but
 * an activation callback takes no arguments and had no way to read them, so a
 * trusted plugin could only ever be an on/off switch. A parameterised feature
 * then had to be modelled as one feature per value — and `constraint` only
 * expresses ordered_integer WITHIN a feature, so those pseudo-features could
 * not even be made mutually exclusive. This closes that gap: one feature, one
 * option, the plugin reads what was chosen.
 *
 * Ids are passed explicitly because registration is by plugin id alone and the
 * callback carries no package/feature context.
 */
int psx_mod_option_value(const char* package_id, const char* feature_id,
                         const char* option_id, char* out, uint32_t out_size);

/*
 * Request a fixed host display aspect before renderer/window initialization.
 * Intended for activation callbacks that move a game's widescreen enhancement
 * out of generic Settings and into its mod catalog.
 */
int psx_mod_set_fixed_display_aspect(uint32_t numerator,
                                     uint32_t denominator);
/*
 * Request resize-driven widescreen, capped at the supplied maximum aspect.
 * The current fixed aspect continues to shape the initial game window, so a
 * plugin may select that first with psx_mod_set_fixed_display_aspect().
 */
int psx_mod_set_adaptive_display_aspect(uint32_t max_numerator,
                                        uint32_t max_denominator);
/*
 * Set the wall-clock cadence of simulated guest VBlanks. A value of zero
 * removes frontend pacing; 60 and higher request that many native guest
 * update opportunities per host second. This intentionally changes whole-
 * machine realtime speed and is for experimental game-owned frame-rate mods.
 */
int psx_mod_set_native_vblank_rate(uint32_t frames_per_second);

/*
 * GooseStation-style Nx CRTC: divide guest cycles per VBlank by `multiplier`
 * (1 = stock, 2 = NTSC 120 Hz / PAL 100 Hz). Host pacing follows the live
 * resulting period automatically. psx_mod_set_native_vblank_rate() is a
 * separate explicit override for mods that intentionally own the wall target.
 */
int psx_mod_set_crtc_refresh_multiplier(uint32_t multiplier);
/*
 * Force the guest VBlank IRQ period to a fixed CRTC rate (50 or 60 Hz), or
 * pass 0 to follow GP1(08h) again. Does not change the GPU video-mode bit —
 * NTSC display programming can keep a 50 Hz IRQ for audio-safe PAL ports.
 */
int psx_mod_set_crtc_vblank_hz(uint32_t hz);

/*
 * Enable presentation-only frame interpolation while leaving guest VBlank,
 * game logic, timers, and audio at their stock cadence. The OpenGL presenter
 * blends between completed guest frames at the requested output rate.
 * A value of zero follows the measured host-display refresh rate.
 */
int psx_mod_set_frame_interpolation(uint32_t frames_per_second);
/*
 * Choose how the OpenGL presenter combines completed frames. Linear is the
 * legacy full-frame crossfade. Motion-adaptive retains interpolation for
 * small temporal changes but switches large changes cleanly to reduce the
 * double-image trails produced by moving objects.
 */
enum {
    PSX_MOD_FRAME_INTERPOLATION_LINEAR = 0,
    PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE = 1
};
int psx_mod_set_frame_interpolation_blend(uint32_t blend_mode);
int psx_mod_set_auto_skip_fmv(int enabled);

/*
 * Upper bounds for the two loading-speed knobs below. Both are generous on
 * purpose: games surface them to players as free-form integers, and neither
 * can corrupt guest state (see the notes on each setter). They exist to reject
 * nonsense, not to curate a list of "blessed" speeds.
 */
#define PSX_MOD_LOAD_ACCEL_MAX  1024u
#define PSX_MOD_DISC_SPEED_MAX  1024u

/*
 * Accelerate only the wall-clock pacing of sustained non-XA data loads while
 * preserving every guest VBlank, CD deadline, interrupt, and callback.
 * wall_clock_multiplier accepts 1..PSX_MOD_LOAD_ACCEL_MAX, or zero for
 * uncapped host speed (1 is a no-op, i.e. authentic pacing).
 * release_frames controls how many guest frames acceleration may remain active
 * after the load predicate clears; zero is the precise/speedrun-safe policy.
 */
int psx_mod_set_load_acceleration(uint32_t wall_clock_multiplier,
                                  uint32_t release_frames);

/*
 * Select guest-visible CD timing for a game-owned loading feature. divisor
 * divides the emulated sector delay, so it IS the speed multiplier: 1 is
 * authentic timing, higher is faster, up to PSX_MOD_DISC_SPEED_MAX. Zero
 * selects the bounded "instant" scheduler, and instant_max_per_frame (1..256)
 * applies only in that case. cdrom.c floors the divided delay at
 * CDROM_MIN_DELAY and leaves XA streaming at authentic timing, so no value
 * here can produce a zero-delay storm or speed up FMV audio. Unlike host load
 * acceleration this changes WHEN the guest receives CD interrupts and can
 * expose game timing bugs, which is why it is a separate, opt-in knob.
 */
int psx_mod_set_disc_speed(uint32_t divisor,
                           uint32_t instant_max_per_frame);

/*
 * DuckStation-style unique 8 MiB main RAM (full high window registered).
 * Must be called from an activation plugin before memory_init. Default is
 * retail 2 MB. Still-aliased code PCs fold to low 2 MiB for AOT. Both
 * netplay peers must match.
 */
int psx_mod_set_main_ram_8mb(int enabled);

/* Fingerprint of the resolved mod plan for this session ("" when no mods
 * runtime is active). Savestates embed it (BS_SEC_MODSET) so a load into a
 * session with a different enabled mod set refuses cleanly. */
const char* psx_mod_runtime_fingerprint_cstr(void);

/* Mark a guest main-RAM range unique under 8 MB mode (optional narrowing). */
void psx_ram_register_unique(uint32_t addr, uint32_t len);

/*
 * Override one player's resolved controller presentation mode for this launch.
 * This is intentionally a trusted-plugin API, not a generic launcher setting:
 * games may hide Hybrid from their normal selector while offering it as an
 * explicit game-owned mod.
 */
enum {
    PSX_MOD_CONTROLLER_HYBRID = 0,
    PSX_MOD_CONTROLLER_ANALOG = 1,
    PSX_MOD_CONTROLLER_DIGITAL = 2
};
int psx_mod_set_controller_mode_override(uint32_t player,
                                         uint32_t controller_mode);

/*
 * Register a C plugin before main() on the compilers supported by the runtime.
 * The registry itself uses function-local initialization, so constructor order
 * between game sources and the framework is safe.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                        \
    __declspec(allocate(".CRT$XCU"))                                       \
    static void (__cdecl* name##_constructor)(void) = name;                \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "PSX mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
