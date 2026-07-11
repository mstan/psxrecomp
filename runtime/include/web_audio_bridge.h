#ifndef PSXRECOMP_WEB_AUDIO_BRIDGE_H
#define PSXRECOMP_WEB_AUDIO_BRIDGE_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

int psx_web_audio_init_subsystem(Uint32 flags);
SDL_AudioDeviceID psx_web_audio_open_device(
    const char *device, int iscapture, const SDL_AudioSpec *desired,
    SDL_AudioSpec *obtained, int allowed_changes);
void psx_web_audio_pause_device(SDL_AudioDeviceID device, int pause_on);
int psx_web_audio_queue(SDL_AudioDeviceID device, const void *data, Uint32 len);
Uint32 psx_web_audio_queued_size(SDL_AudioDeviceID device);
void psx_web_audio_clear(SDL_AudioDeviceID device);
void psx_web_audio_close(SDL_AudioDeviceID device);
void psx_web_audio_lock(SDL_AudioDeviceID device);
void psx_web_audio_unlock(SDL_AudioDeviceID device);

#ifdef __cplusplus
}
#endif

#endif
