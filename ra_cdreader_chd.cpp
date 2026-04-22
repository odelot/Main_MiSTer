// ra_cdreader_chd.cpp
//
// Universal CHD CD-reader bridge for rcheevos.
//
// Registers a custom rc_hash_cdreader that handles:
//   .chd  -> mister_load_chd / mister_chd_read_sector (libchdr-based)
//   .cue / .gdi -> rcheevos default handlers (original behaviour, unchanged)
//
// One registration in achievements_init() fixes all disc-based consoles:
//   PSX (12), MegaCD (9), PCE-CD (76), NeoGeoCD (56)

#ifdef HAS_RCHEEVOS

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "support/chd/mister_chd.h"    // mister_load_chd, mister_chd_read_sector
#include "lib/rcheevos/include/rc_hash.h"

// ── magic tags to distinguish handle types at runtime ──────────────────────
#define RA_CHD_MAGIC     0xC4D0C4D0u
#define RA_DEFAULT_MAGIC 0xDE4FA117u

// ── handle for CHD tracks ──────────────────────────────────────────────────
struct ra_chd_handle {
    unsigned int magic;      // RA_CHD_MAGIC
    toc_t        toc;
    int          track_idx;  // 0-based index into toc.tracks[]
    uint8_t*     hunkbuf;    // decompressed hunk cache (chd_hunksize bytes)
    int          hunknum;    // currently cached hunk (-1 = none)
};

// ── handle wrapper for .cue / .gdi tracks (opaque inner handle) ────────────
struct ra_default_handle {
    unsigned int magic;      // RA_DEFAULT_MAGIC
    void*        inner;      // opaque handle returned by the default cdreader
};

// ── saved default handlers ─────────────────────────────────────────────────
static struct rc_hash_cdreader s_default;

// ── helpers ────────────────────────────────────────────────────────────────
static bool path_is_chd(const char* path)
{
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    // case-insensitive compare ".chd"
    return (dot[1] == 'c' || dot[1] == 'C') &&
           (dot[2] == 'h' || dot[2] == 'H') &&
           (dot[3] == 'd' || dot[3] == 'D') &&
           dot[4] == '\0';
}

// Returns how many bytes to skip from the start of the raw CHD sector frame
// (which stores 2352 bytes) to reach the user-data payload.
//
//   MODE1/2352 : sync(12) + header(4) = skip 16  -> 2048 bytes data
//   MODE2/2352 : sync(12) + header(4) + sub-hdr(8) = skip 24 -> XA data
//   MODE2/2336 : sub-header(8) = skip 8  -> XA data (no sync stored)
//   MODE1/2048 : no overhead, skip 0
//   CDDA/2352  : raw audio, skip 0
static uint32_t sector_data_offset(const cd_track_t* t)
{
    if (t->type == TT_MODE1) {
        return (t->sector_size == 2352) ? 16 : 0;
    }
    if (t->type == TT_MODE2) {
        if (t->sector_size == 2352) return 24;
        if (t->sector_size == 2336) return 8;
        return 0;
    }
    return 0;  // TT_CDDA: raw audio
}

// ── CHD callbacks ──────────────────────────────────────────────────────────
static void* ra_chd_open_track(const char* path, uint32_t track_id)
{
    ra_chd_handle* h = (ra_chd_handle*)calloc(1, sizeof(ra_chd_handle));
    if (!h) return NULL;

    h->magic   = RA_CHD_MAGIC;
    h->hunknum = -1;

    if (mister_load_chd(path, &h->toc) != CHDERR_NONE) {
        free(h);
        return NULL;
    }

    h->hunkbuf = (uint8_t*)malloc((size_t)h->toc.chd_hunksize);
    if (!h->hunkbuf) {
        chd_close(h->toc.chd_f);
        free(h);
        return NULL;
    }

    int num = h->toc.last;   // number of tracks
    int idx = -1;

    if (track_id == RC_HASH_CDTRACK_FIRST_DATA) {
        for (int i = 0; i < num; i++) {
            if (h->toc.tracks[i].type != TT_CDDA) { idx = i; break; }
        }
    }
    else if (track_id == RC_HASH_CDTRACK_LAST) {
        idx = num - 1;
    }
    else if (track_id == RC_HASH_CDTRACK_LARGEST) {
        int best_len = -1;
        // prefer largest data track
        for (int i = 0; i < num; i++) {
            if (h->toc.tracks[i].type == TT_CDDA) continue;
            int len = h->toc.tracks[i].end - h->toc.tracks[i].start;
            if (len > best_len) { best_len = len; idx = i; }
        }
        if (idx < 0) {
            // fallback: largest audio track
            for (int i = 0; i < num; i++) {
                int len = h->toc.tracks[i].end - h->toc.tracks[i].start;
                if (len > best_len) { best_len = len; idx = i; }
            }
        }
    }
    else if (track_id == RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION) {
        // CHD doesn't preserve session boundaries; return NULL so rcheevos
        // falls back to its alternative strategy automatically.
        chd_close(h->toc.chd_f);
        free(h->hunkbuf);
        free(h);
        return NULL;
    }
    else {
        // 1-based track number
        int i = (int)track_id - 1;
        if (i >= 0 && i < num) idx = i;
    }

    if (idx < 0) {
        chd_close(h->toc.chd_f);
        free(h->hunkbuf);
        free(h);
        return NULL;
    }

    h->track_idx = idx;
    return h;
}

static size_t ra_chd_read_sector(void* handle, uint32_t sector,
                                  void* buffer, size_t requested_bytes)
{
    ra_chd_handle* h = (ra_chd_handle*)handle;
    cd_track_t*    t = &h->toc.tracks[h->track_idx];

    uint32_t s_off = sector_data_offset(t);

    // cap to available payload bytes in the sector
    size_t max_bytes = (t->sector_size > (int)s_off)
                       ? (size_t)(t->sector_size - (int)s_off)
                       : 0;
    if (requested_bytes > max_bytes) requested_bytes = max_bytes;
    if (requested_bytes == 0) return 0;

    // sector is an absolute disc LBA; t->offset converts to CHD-physical LBA
    // (same formula used in psx.cpp and megacd.cpp)
    int chd_lba = (int)sector + t->offset;

    chd_error err = mister_chd_read_sector(
        h->toc.chd_f,
        chd_lba,
        0,                      // d_offset: write at byte 0 of destbuf
        s_off,                  // s_offset: skip sync/header within raw frame
        (int)requested_bytes,
        (uint8_t*)buffer,
        h->hunkbuf,
        &h->hunknum);

    return (err == CHDERR_NONE) ? requested_bytes : 0;
}

static void ra_chd_close_track(void* handle)
{
    ra_chd_handle* h = (ra_chd_handle*)handle;
    if (h->toc.chd_f) chd_close(h->toc.chd_f);
    free(h->hunkbuf);
    free(h);
}

static uint32_t ra_chd_first_track_sector(void* handle)
{
    ra_chd_handle* h = (ra_chd_handle*)handle;
    return (uint32_t)h->toc.tracks[h->track_idx].start;
}

// ── Unified dispatcher: CHD takes priority, .cue/.gdi goes to default ──────
static void* ra_unified_open_track(const char* path, uint32_t track_id)
{
    if (path_is_chd(path))
        return ra_chd_open_track(path, track_id);

    if (!s_default.open_track) return NULL;

    void* inner = s_default.open_track(path, track_id);
    if (!inner) return NULL;

    ra_default_handle* h = (ra_default_handle*)malloc(sizeof(ra_default_handle));
    if (!h) { s_default.close_track(inner); return NULL; }
    h->magic = RA_DEFAULT_MAGIC;
    h->inner = inner;
    return h;
}

static size_t ra_unified_read_sector(void* handle, uint32_t sector,
                                      void* buffer, size_t requested_bytes)
{
    if (*(unsigned int*)handle == RA_CHD_MAGIC)
        return ra_chd_read_sector(handle, sector, buffer, requested_bytes);

    ra_default_handle* h = (ra_default_handle*)handle;
    return s_default.read_sector(h->inner, sector, buffer, requested_bytes);
}

static void ra_unified_close_track(void* handle)
{
    if (*(unsigned int*)handle == RA_CHD_MAGIC) {
        ra_chd_close_track(handle);
        return;
    }
    ra_default_handle* h = (ra_default_handle*)handle;
    s_default.close_track(h->inner);
    free(h);
}

static uint32_t ra_unified_first_track_sector(void* handle)
{
    if (*(unsigned int*)handle == RA_CHD_MAGIC)
        return ra_chd_first_track_sector(handle);

    ra_default_handle* h = (ra_default_handle*)handle;
    return s_default.first_track_sector(h->inner);
}

// ── Public API ─────────────────────────────────────────────────────────────
void ra_cdreader_chd_register(void)
{
    // Save the default (cue/gdi) handlers so we can delegate to them
    rc_hash_get_default_cdreader(&s_default);

    struct rc_hash_cdreader unified;
    unified.open_track         = ra_unified_open_track;
    unified.read_sector        = ra_unified_read_sector;
    unified.close_track        = ra_unified_close_track;
    unified.first_track_sector = ra_unified_first_track_sector;
    rc_hash_init_custom_cdreader(&unified);
}

#endif // HAS_RCHEEVOS
