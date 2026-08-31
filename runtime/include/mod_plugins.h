#pragma once

#include <stdint.h>

#include "func_override.h" /* FO_CREDIT_SELF for the credit argument below */

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
 * native code or symbol names.
 */
int psx_mod_register_activation_plugin(const char* id,
                                       PSXModActivationCallback callback);
int psx_mod_register_vblank_plugin(const char* id,
                                   PSXModVBlankCallback callback);
int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback);
/* Called only from generated functions explicitly listed by the game config. */
void psx_mod_function_entry(struct CPUState* cpu, uint32_t address);

/*
 * Package-gated function override (see runtime/include/func_override.h for
 * the full contract). Registration queues the override under the plugin id;
 * it is ARMED into the dispatcher tier only when a resolved package plan
 * selects that plugin — the same gating as vblank/activation callbacks. The
 * callback obeys the guest ABI (args cpu->gpr[4..7], result cpu->gpr[2]);
 * return 1 = handled (guest resumes at $ra), 0 = decline (original runs).
 * Wrap semantics via func_override_call_original(); optional residency
 * guard via expected_words (NULL/0 = unguarded).
 *
 * `id` may carry an optional ":label" suffix ("pkg.feature:aim") that names
 * the override in the `func_override` TCP inventory; gating and manifest
 * matching use only the part before the ':', so several overrides can sit
 * under one [[plugin]] entry and still read apart in diagnostics.
 *
 * `credit` is the REQUIRED guest-cycle policy (func_override.h, CYCLE
 * ACCOUNTING): >= 0 cycles charged per handled call, or FO_CREDIT_SELF when
 * the body charges its own. For a mod whose behavior has no hardware analog
 * an explicit 0 is the honest value; a wrap that runs the original via
 * func_override_call_original must pass FO_CREDIT_SELF (the original
 * self-charges by executing — a fixed credit would double-count).
 */
typedef int (*PSXModFunctionOverrideFn)(struct CPUState* cpu);
int psx_mod_register_function_override(const char* id, uint32_t address,
                                       PSXModFunctionOverrideFn fn,
                                       const uint32_t* expected_words,
                                       int n_words, int32_t credit);

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
 * Width, in native game pixels, of the picture the guest is currently
 * scanning out -- the same value the presenter uses, derived from the display
 * mode and the GP1(06h) horizontal range.
 *
 * Why this exists: a plugin that draws its own overlay primitives needs to
 * know where the right-hand edge of the screen is, and it cannot work that
 * out for itself. GPUSTAT carries the horizontal-resolution bits, so a plugin
 * can recover the coarse MODE width (256/320/512/640, or 368), but the
 * visible width also depends on the GP1(06h) X1/X2 range, which is write-only
 * and mirrored nowhere the plugin can read. Ape Escape is the worked example:
 * it scans out 384 while its mode width is 368, and a plugin that assumed the
 * usual 320 put its HUD row 68 pixels short of the edge.
 *
 * Returns 0 if the display geometry is not yet established, in which case the
 * caller should skip drawing rather than substitute a guess.
 */
uint32_t psx_mod_display_width(void);

/* Height companion to psx_mod_display_width(); same conventions. */
uint32_t psx_mod_display_height(void);

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
 * Read the committed owner-selected path for a resource declared by the
 * package feature whose trusted plugin is currently running. Returns 0 when
 * the feature has no selected path for that resource; plugins then leave the
 * stock presentation unchanged.
 */
int psx_mod_current_resource_path(const char* resource_id,
                                  char* out, uint32_t out_size);

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
 * Enable presentation-only frame interpolation while leaving guest VBlank,
 * game logic, timers, and audio at their stock cadence. The OpenGL presenter
 * temporally blends completed guest frames at the requested output rate on its
 * owning render thread/context. It does not derive motion vectors or generate
 * true intermediate object positions.
 * A value of zero follows the measured host-display refresh rate.
 */
int psx_mod_set_frame_interpolation(uint32_t frames_per_second);
/*
 * Choose how the OpenGL presenter combines completed frames. Linear is a
 * full-frame crossfade. Motion-adaptive retains temporal blending for
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
 * Draw still artwork behind the game image in OpenGL letterbox/pillarbox
 * margins. The image path is an owner-selected mod resource; with no enabled
 * mod/resource path, the margins remain the historical black clear.
 */
int psx_mod_set_bezel_artwork(const char* path);

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

/* Controller presentation values exposed to trusted game-owned plugins. */
enum {
    PSX_MOD_CONTROLLER_ANALOG = 1,
    PSX_MOD_CONTROLLER_DIGITAL = 2
};
/*
 * Per-sample input facts for an opt-in controller presentation policy. The
 * runtime owns SDL sampling and SIO delivery; the game-owned plugin owns only
 * the policy decision of whether this sample should present as DualShock
 * analog or a digital pad.
 */
typedef struct PSXModControllerInput {
    uint32_t struct_size;
    uint32_t player;
    uint32_t sio_slot;
    uint32_t configured_mode;
    uint32_t current_mode;
    uint32_t stick_active;
    uint32_t dpad_active;
    uint32_t buttons;
    uint32_t lx;
    uint32_t ly;
    uint32_t rx;
    uint32_t ry;
} PSXModControllerInput;
typedef uint32_t (*PSXModControllerPresentationCallback)(
    const PSXModControllerInput* input);
/*
 * Override one player's resolved controller presentation mode for this launch.
 * This is intentionally a trusted-plugin API, not a generic launcher setting.
 */
int psx_mod_set_controller_mode_override(uint32_t player,
                                         uint32_t controller_mode);
/*
 * Let a game-owned plugin choose analog/digital presentation for one player on
 * every input sample. The initial mode is used for boot/hotplug before the
 * first sample. config_capable should be non-zero when the selected policy may
 * present a DualShock, even if a later sample currently reports digital.
 */
int psx_mod_set_controller_presentation_policy(
    uint32_t player,
    PSXModControllerPresentationCallback callback,
    uint32_t initial_mode,
    int config_capable);

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
