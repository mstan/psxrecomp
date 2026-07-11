/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "iso_reader.h"
#include <cstdio>

extern "C" {

void* iso_open(const char* path) {
    auto* reader = new PS1::ISOReader();
    if (!reader->Open(path)) {
        delete reader;
        return nullptr;
    }
    return reader;
}

int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    (void)size; /* ReadSector always reads 2048 bytes */
    return reader->ReadSector(lba, buffer) ? 1 : 0;
}

int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || size < 2352) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader->ReadRawSector(lba, buffer) ? 1 : 0;
}

uint32_t iso_sector_count(void* handle) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader->GetSectorCount();
}

/* CD-track TOC accessors (multi-track / CD-DA support). track is 1-based. */
int iso_track_count(void* handle) {
    if (!handle) return 1;
    return static_cast<PS1::ISOReader*>(handle)->TrackCount();
}

uint32_t iso_track_start_lba(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackStartLBA(track);
}

uint32_t iso_track_pregap_lba(void* handle, int track) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader ? reader->TrackPregapLBA(track) : 0;
}

int iso_track_is_audio(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackIsAudio(track) ? 1 : 0;
}

void iso_close(void* handle) {
    if (!handle) return;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    reader->Close();
    delete reader;
}

} /* extern "C" */
