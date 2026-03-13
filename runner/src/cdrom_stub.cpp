#include "cdrom_stub.h"
#include "iso_reader.h"
#include "xa_audio.h"
#include "fmv_player.h"
#include "spu.h"
#include <cstdio>

static PS1::ISOReader g_iso;

extern "C" {

int psx_cdrom_init(const char* cue_path) {
    if (!g_iso.Open(cue_path)) {
        fprintf(stderr, "[CDROM] Failed to open: %s\n", cue_path);
        return 0;
    }
    fprintf(stderr, "[CDROM] Opened disc: volume='%s'\n", g_iso.GetVolumeID().c_str());
    xa_audio_init(g_iso.GetBinPath().c_str());
    fmv_player_init(g_iso.GetBinPath().c_str());
    spu_init();
    return 1;
}

int psx_cdrom_read_sector(uint32_t lba, uint8_t* buffer) {
    return g_iso.ReadSector(lba, buffer) ? 1 : 0;
}

} /* extern "C" */
