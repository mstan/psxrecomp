#include "overlay_posix.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ScanResult {
    int count;
    uint32_t addr;
    uint32_t crc;
} ScanResult;

static int record_cache_file(const PsxOverlayCacheFile *file, void *opaque) {
    ScanResult *result = (ScanResult *)opaque;
    result->count++;
    if (strcmp(file->name, "80010000_DEADBEEF.so") == 0) {
        result->addr = file->region_start;
        result->crc = file->content_crc;
    }
    return 0;
}

static void check_name_parser(void) {
    uint32_t addr = 1, crc = 1;
    assert(psx_overlay_cache_name_parse("00000000_00000000.so", &addr, &crc));
    assert(addr == 0 && crc == 0);
    assert(psx_overlay_cache_name_parse("89abcdef_ABCDEF01.so", &addr, &crc));
    assert(addr == 0x89ABCDEFu && crc == 0xABCDEF01u);
    assert(psx_overlay_cache_name_parse(
        "89abcdef_ABCDEF01_13579BDF.so", &addr, &crc));
    assert(addr == 0x89ABCDEFu && crc == 0xABCDEF01u);

    assert(!psx_overlay_cache_name_parse("", &addr, &crc));
    assert(!psx_overlay_cache_name_parse(".so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse("G0010000_DEADBEEF.so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse("80010000_DEADBEG0.so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse(
        "80010000_DEADBEEF_DEADBEG0.so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse(
        "80010000_DEADBEEFDEADBEEF.so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse("80010000_DEADBEEF.dll", &addr, &crc));
    assert(!psx_overlay_cache_name_parse("80010000-DEADBEEF.so", &addr, &crc));
    assert(!psx_overlay_cache_name_parse("80010000_DEADBEEF.so.extra", &addr, &crc));
}

int main(int argc, char **argv) {
    assert(argc == 5);
    check_name_parser();

    ScanResult result = {0};
    assert(psx_overlay_posix_scan_cache_dir(argv[1], record_cache_file, &result) == 3);
    assert(result.count == 3);
    assert(result.addr == 0x80010000u);
    assert(result.crc == 0xDEADBEEFu);

    char found[64] = {0};
    assert(psx_overlay_posix_find_other_cache_tag(argv[2], argv[3], found,
                                                  sizeof(found)) == 1);
    assert(strcmp(found, argv[4]) == 0);

    char error[256] = {0};
    void *handle = psx_overlay_posix_library_open(argv[1], error, sizeof(error));
    assert(handle == NULL); /* A directory is not a loadable overlay library. */

    char fixture[1024];
    snprintf(fixture, sizeof(fixture), "%s/80010000_DEADBEEF.so", argv[1]);
    handle = psx_overlay_posix_library_open(fixture, error, sizeof(error));
    assert(handle != NULL);
    typedef int (*AbiFn)(void);
    typedef uint32_t (*EntryFn)(void);
    AbiFn abi = (AbiFn)psx_overlay_posix_library_symbol(handle, "overlay_abi");
    EntryFn entry = (EntryFn)psx_overlay_posix_library_entry(handle, 0x80012345u);
    assert(abi && abi() == 0x12345678);
    assert(entry && entry() == 0xC0DEF00Du);
    assert(psx_overlay_posix_library_entry(handle, 0x80099999u) == NULL);
    assert(psx_overlay_posix_library_symbol(handle, "func_80099999") == NULL);
    psx_overlay_posix_library_close(handle);

    /* A close followed by a fresh open must remain usable; the production
     * loader closes rejected/empty libraries and retains successful handles. */
    handle = psx_overlay_posix_library_open(fixture, error, sizeof(error));
    assert(handle != NULL);
    psx_overlay_posix_library_close(handle);

    puts("overlay POSIX discovery tests passed");
    return 0;
}
