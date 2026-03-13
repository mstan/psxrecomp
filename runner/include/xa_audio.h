#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize XA audio system. bin_path = path to .bin disc image (raw 2352-byte sectors). */
void xa_audio_init(const char* bin_path);

/* Notify that the game seeked to a new LBA. If that region contains XA audio
 * sectors, begin/restart streaming playback from that position.
 * If no audio sectors are found within a short scan window, playback stops. */
void xa_audio_seek(uint32_t lba);

/* Set playback volume. 0.0 = silent, 1.0 = full scale. Default 0.5. */
void xa_audio_set_volume(float v);

/* Clean up resources. Call at exit. */
void xa_audio_shutdown(void);

#ifdef __cplusplus
}
#endif
