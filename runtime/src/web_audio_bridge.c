/* Minimal queued-audio bridge for the pthread web runtime.
 *
 * SDL2 2.32's Emscripten backend creates its AudioContext with
 * MAIN_THREAD_EM_ASM, then reads Module.SDL2.audioContext from the worker with
 * plain EM_ASM. JavaScript globals are per-thread, so PROXY_TO_PTHREAD builds
 * fault before the device opens. This bridge keeps every Web Audio operation
 * on the browser thread and exposes the small SDL queued-audio surface used by
 * the runtime. Guest samples remain 44.1 kHz; Web Audio performs any host-rate
 * conversion.
 */

#include "web_audio_bridge.h"

#include <emscripten.h>

int psx_web_audio_init_subsystem(Uint32 flags) {
    (void)flags;
    return MAIN_THREAD_EM_ASM_INT({
        if (!Module.psxWebAudio) {
            var Context = globalThis.AudioContext || globalThis.webkitAudioContext;
            if (!Context) return -1;
            var context = new Context();
            Module.psxWebAudio = {};
            Module.psxWebAudio.context = context;
            Module.psxWebAudio.nextTime = context.currentTime;
            Module.psxWebAudio.sources = new Set();
        }
        return 0;
    });
}

SDL_AudioDeviceID psx_web_audio_open_device(
    const char *device, int iscapture, const SDL_AudioSpec *desired,
    SDL_AudioSpec *obtained, int allowed_changes) {
    (void)device;
    (void)allowed_changes;
    if (iscapture || !desired || !obtained || desired->channels != 2 ||
        desired->format != AUDIO_S16SYS) {
        return 0;
    }
    *obtained = *desired;
    obtained->callback = NULL;
    return 1;
}

void psx_web_audio_pause_device(SDL_AudioDeviceID device, int pause_on) {
    (void)device;
    MAIN_THREAD_EM_ASM({
        var state = Module.psxWebAudio;
        if (!state) return;
        if ($0) state.context.suspend();
        else state.context.resume();
    }, pause_on);
}

int psx_web_audio_queue(SDL_AudioDeviceID device, const void *data, Uint32 len) {
    (void)device;
    if (!data || len < 4) return 0;
    MAIN_THREAD_EM_ASM({
        var state = Module.psxWebAudio;
        if (!state) return;
        var frames = ($1 / 4) | 0;
        if (!frames) return;
        var buffer = state.context.createBuffer(2, frames, 44100);
        var left = buffer.getChannelData(0);
        var right = buffer.getChannelData(1);
        growMemViews();
        var base = $0 >> 1;
        for (var i = 0; i < frames; ++i) {
            left[i] = HEAP16[base + i * 2] / 32768.0;
            right[i] = HEAP16[base + i * 2 + 1] / 32768.0;
        }
        var source = state.context.createBufferSource();
        source.buffer = buffer;
        source.connect(state.context.destination);
        var floor = state.context.currentTime + 0.015;
        var start = Math.max(floor, state.nextTime || floor);
        source.start(start);
        state.nextTime = start + frames / 44100.0;
        state.sources.add(source);
        source.onended = function() { state.sources.delete(source); };
    }, data, len);
    return 0;
}

Uint32 psx_web_audio_queued_size(SDL_AudioDeviceID device) {
    (void)device;
    return (Uint32)MAIN_THREAD_EM_ASM_INT({
        var state = Module.psxWebAudio;
        if (!state) return 0;
        var seconds = Math.max(0, state.nextTime - state.context.currentTime);
        return Math.min(0x7fffffff, Math.round(seconds * 44100 * 4));
    });
}

void psx_web_audio_clear(SDL_AudioDeviceID device) {
    (void)device;
    MAIN_THREAD_EM_ASM({
        var state = Module.psxWebAudio;
        if (!state) return;
        state.sources.forEach(function(source) {
            try { source.stop(); } catch (_) {}
        });
        state.sources.clear();
        state.nextTime = state.context.currentTime;
    });
}

void psx_web_audio_close(SDL_AudioDeviceID device) {
    psx_web_audio_clear(device);
}

void psx_web_audio_lock(SDL_AudioDeviceID device) { (void)device; }
void psx_web_audio_unlock(SDL_AudioDeviceID device) { (void)device; }
