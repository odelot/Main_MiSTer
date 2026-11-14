// achievements_saturn.cpp — RetroAchievements Saturn-specific implementation
//
// Memory layout in DDR3 (accessible via shmem_map):
//   RAML (Work RAM Low,  1MB): physical 0x30200000
//   RAMH (Work RAM High, 1MB): physical 0x30300000
//
// The core's ddram.sv RAML port is shared by three SH-2 areas, selected by
// raml_addr[21:19] = {~RAML_CS_N, ~SRAM_CS_N, ...}: BIOS ROM at +0x000000,
// internal backup RAM at +0x100000, Work RAM Low at +0x200000. So 0x30000000
// is the BIOS, NOT Work RAM Low (reading it returns static BIOS bytes).
//
// Byte order: Saturn is big-endian; the core's DDR3 stores SH-2 byte K at
// offset K^7 (64-bit big-endian packing). RA sets are authored against
// beetle-saturn, whose exposed WorkRAM stores SH-2 byte K at index K^1, so:
//   rcheevos address A → DDR3 byte at offset (A^1)^7 = A ^ 6
//
// rcheevos Saturn memory map (RC_CONSOLE_SATURN = 39):
//   0x000000–0x0FFFFF: Work RAM Low  (1MB)
//   0x100000–0x1FFFFF: Work RAM High (1MB)

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include "shmem.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// Saturn constants
// ---------------------------------------------------------------------------

#define SATURN_RAML_PHYS     0x30200000u
#define SATURN_RAML_SIZE     0x00100000u  // 1MB Work RAM Low
#define SATURN_RAMH_PHYS     0x30300000u
#define SATURN_RAMH_SIZE     0x00100000u  // 1MB Work RAM High

// rcheevos memory window boundaries
#define SATURN_RA_RAML_END   0x0FFFFFu   // 1MB Work RAM Low window end
#define SATURN_RA_RAMH_START 0x100000u
#define SATURN_RA_RAMH_END   0x1FFFFFu

// ---------------------------------------------------------------------------
// Saturn state
// ---------------------------------------------------------------------------

static console_state_t g_saturn_state = {};
static void *g_saturn_raml = NULL;  // mmap of physical 0x30200000, 1MB
static void *g_saturn_ramh = NULL;  // mmap of physical 0x30300000, 1MB

// Debug watch (retroachievements.cfg: watch=...): last seen value per address
static uint8_t g_watch_last[16];
static int     g_watch_init = 0;

// ---------------------------------------------------------------------------
// Saturn implementation
// ---------------------------------------------------------------------------

static void saturn_init(void)
{
        memset(&g_saturn_state, 0, sizeof(g_saturn_state));
        g_saturn_raml = NULL;
        g_saturn_ramh = NULL;
        // RA header lives in the 12MB gap in Saturn DDR3: physical 0x30F00000
        // (between RAMH end at 0x303FFFFF and cdbuf at 0x31000000)
        ra_ramread_set_base(0x30F00000u);

        // The Saturn FPGA channel only writes the 64-bit word at offset 0x08
        // (frame counter + core version + build flags); the static RACH header
        // word is written once here by the ARM. The core version placed here is
        // a placeholder: saturn_detect_protocol() replaces it with the one the
        // RBF reports, so the User-Agent follows the core, not the Main.
        void *hdr = shmem_map(0x30F00000u, 4096);
        if (hdr) {
                volatile uint32_t *w = (volatile uint32_t *)hdr;
                w[1] = 0x01000000u;   // region_count=0, flags=0, core_version=1.0 (placeholder)
                w[2] = 0;             // frame counter (FPGA overwrites every VBlank)
                w[3] = 0;             // core version + build flags (FPGA >= 2.0 overwrites;
                                      // must start at 0 so a stale value can't fake a version)
                w[0] = 0x52414348u;   // magic "RACH" — written last
                shmem_unmap(hdr, 4096);
                ra_log_write("Saturn: RACH header written by ARM at 0x30F00000\n");
        } else {
                ra_log_write("Saturn: WARNING - failed to map RA header area!\n");
        }
}

static void saturn_reset(void)
{
        memset(&g_saturn_state, 0, sizeof(g_saturn_state));
        g_watch_init = 0;
        if (g_saturn_raml) {
                shmem_unmap(g_saturn_raml, SATURN_RAML_SIZE);
                g_saturn_raml = NULL;
        }
        if (g_saturn_ramh) {
                shmem_unmap(g_saturn_ramh, SATURN_RAMH_SIZE);
                g_saturn_ramh = NULL;
        }
}

static uint32_t saturn_read_memory(void *map, uint32_t address, uint8_t *buffer,
        uint32_t num_bytes)
{
        (void)map;

        for (uint32_t i = 0; i < num_bytes; i++) {
                uint32_t addr = address + i;
                uint8_t byte = 0;

                // Byte order: RA sets are authored against beetle-saturn, whose
                // exposed WorkRAM array stores SH-2 byte K at index K^1 (16-bit
                // host swap, ne16_ptr_be). The core's DDR stores SH-2 byte K at
                // offset K^7 (big-endian packing in 64-bit words). So RA addr A
                // maps to DDR offset (A^1)^7 = A^6.
                // (Proof: "Millionaire" scores = b[0x197c59]*25600 + b[0x197c58]*100
                //  — high byte at the ODD address = beetle-swapped convention.)
                if (addr <= SATURN_RA_RAML_END) {
                        // Work RAM Low (1MB)
                        if (g_saturn_raml) {
                                uint32_t ddr_off = addr ^ 6;
                                if (ddr_off < SATURN_RAML_SIZE)
                                        byte = ((const uint8_t *)g_saturn_raml)[ddr_off];
                        }
                } else if (addr >= SATURN_RA_RAMH_START && addr <= SATURN_RA_RAMH_END) {
                        // Work RAM High (1MB)
                        if (g_saturn_ramh) {
                                uint32_t k = addr - SATURN_RA_RAMH_START;
                                uint32_t ddr_off = k ^ 6;
                                if (ddr_off < SATURN_RAMH_SIZE)
                                        byte = ((const uint8_t *)g_saturn_ramh)[ddr_off];
                        }
                }

                buffer[i] = byte;
        }

        return num_bytes;
}

static int saturn_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
        if (!client || !game_loaded) return 0;
        if (!g_saturn_raml && !g_saturn_ramh) return 0;

        rc_client_t *rc_client = (rc_client_t *)client;

        // Gate on FPGA VBlank frame counter
        uint32_t frame = ra_ramread_frame(map);
        if (frame == g_saturn_state.last_resp_frame) return 1;

        g_saturn_state.last_resp_frame = frame;
        g_saturn_state.game_frames++;
        ra_frame_processed(g_saturn_state.game_frames);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // Initialize on first frame
        if (g_saturn_state.game_frames == 1 && !g_saturn_state.cache_ready) {
                g_saturn_state.cache_ready = 1;
                g_saturn_state.poll_logged = 0;
                g_saturn_state.cache_time = now;
                ra_log_write("Saturn: VBlank-gated polling active\n");
        }

        if (g_saturn_state.game_frames <= 5)
                ra_log_write("Saturn: Frame %u (fpga=%u)\n",
                        g_saturn_state.game_frames, frame);

        // Debug watch: sample the configured RA addresses through the same
        // read path rcheevos uses and log every change with the frame number.
        {
                const uint32_t *waddrs;
                int wcount = achievements_watch_list(&waddrs);
                if (wcount > 16) wcount = 16;
                for (int i = 0; i < wcount; i++) {
                        uint8_t v = 0;
                        saturn_read_memory(NULL, waddrs[i], &v, 1);
                        if (!g_watch_init || v != g_watch_last[i]) {
                                ra_log_write("SAT WATCH f=%u 0x%06X: %02X->%02X\n",
                                        frame, waddrs[i],
                                        g_watch_init ? g_watch_last[i] : v, v);
                                g_watch_last[i] = v;
                        }
                }
                if (wcount) g_watch_init = 1;
        }

        rc_client_do_frame(rc_client);

        // Periodic logging every 5 seconds (300 frames at 60Hz)
        uint32_t milestone = g_saturn_state.game_frames / 300;
        if (milestone > 0 && milestone != g_saturn_state.poll_logged) {
                g_saturn_state.poll_logged = milestone;
                double elapsed = (now.tv_sec - g_saturn_state.cache_time.tv_sec)
                        + (now.tv_nsec - g_saturn_state.cache_time.tv_nsec) / 1e9;
                double ms_per_cycle = (g_saturn_state.game_frames > 0) ?
                        (elapsed * 1000.0 / g_saturn_state.game_frames) : 0.0;
                ra_log_write("POLL(SAT): game_frames=%u elapsed=%.1fs ms/cycle=%.1f\n",
                        g_saturn_state.game_frames, elapsed, ms_per_cycle);
        }

        return 1;
#else
        return 0;
#endif
}

static int saturn_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
        char abs_path[1024];
        if (rom_path[0] == '/') {
                snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
        } else {
                extern const char *getRootDir(void);
                snprintf(abs_path, sizeof(abs_path), "%s/%s",
                        getRootDir(), rom_path);
        }

        if (rc_hash_generate_from_file(md5_hex_out, RC_CONSOLE_SATURN, abs_path)) {
                ra_log_write("Saturn hash: %s\n", md5_hex_out);
                return 1;
        }
        // Not a valid Saturn disc image (e.g. the BIOS .bin selected in the
        // OSD). A whole-file MD5 fallback is meaningless for disc consoles,
        // so report "handled" with an empty hash — the load is skipped
        // quietly instead of querying the server with a bogus hash.
        ra_log_write("Saturn: not a Saturn disc image, skipping identify: %s\n",
                abs_path);
        md5_hex_out[0] = '\0';
        return 1;
#else
        return 0;
#endif
}

static void saturn_set_hardcore(int enabled)
{
        (void)enabled;
        // Saturn MiSTer core has no hardware cheat engine; no OSD toggle needed
}

static int saturn_detect_protocol(void *map)
{
        if (!ra_ramread_active(map)) {
                ra_log_write("Saturn: FPGA mirror not detected -- RA support unavailable\n");
                return 0;
        }

        // Core version and build flags come from the RBF (ddram.sv RA channel),
        // written next to the frame counter. This runs after the frame counter
        // advanced, so the FPGA has rewritten the word since saturn_init() zeroed it.
        //   0x0C: core version (major << 8 | minor); 0 = RBF older than 2.0
        //   0x0E: build flags; bit0 = WRAM-H lives in SDRAM2 (dual-SDRAM build)
        {
                volatile ra_header_t *h = (volatile ra_header_t *)map;
                uint32_t info  = h->reserved2;
                uint16_t ver   = (uint16_t)(info & 0xFFFF);
                uint8_t  flags = (uint8_t)((info >> 16) & 0xFF);
                if (flags & 0x01) {
                        ra_log_write("Saturn: dual-SDRAM build (WRAM-H not in DDR3) -- RA support unavailable\n");
                        return 0;
                }
                h->core_version = ver ? ver : 0x0100;
                ra_log_write("Saturn: core reports version %u.%u%s\n",
                        (h->core_version >> 8) & 0xFF, h->core_version & 0xFF,
                        ver ? "" : " (RBF without version field, assuming 1.0)");
        }

        // Map Work RAM Low (RAML) directly: physical 0x30200000, 1MB
        if (!g_saturn_raml) {
                g_saturn_raml = shmem_map(SATURN_RAML_PHYS, SATURN_RAML_SIZE);
                if (g_saturn_raml)
                        ra_log_write("Saturn: RAML mapped at 0x%08X (%uKB)\n",
                                SATURN_RAML_PHYS, SATURN_RAML_SIZE / 1024);
                else
                        ra_log_write("Saturn: WARNING - failed to map RAML!\n");
        }

        // Map Work RAM High (RAMH) directly: physical 0x30300000, 1MB
        if (!g_saturn_ramh) {
                g_saturn_ramh = shmem_map(SATURN_RAMH_PHYS, SATURN_RAMH_SIZE);
                if (g_saturn_ramh)
                        ra_log_write("Saturn: RAMH mapped at 0x%08X (%uKB)\n",
                                SATURN_RAMH_PHYS, SATURN_RAMH_SIZE / 1024);
                else
                        ra_log_write("Saturn: WARNING - failed to map RAMH!\n");
        }

        ra_log_write("Saturn: Direct RAM mode (RAML=%s RAMH=%s)\n",
                g_saturn_raml ? "OK" : "FAIL",
                g_saturn_ramh ? "OK" : "FAIL");

        return (g_saturn_raml || g_saturn_ramh) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_saturn = {
        .init             = saturn_init,
        .reset            = saturn_reset,
        .read_memory      = saturn_read_memory,
        .poll             = saturn_poll,
        .calculate_hash   = saturn_calculate_hash,
        .set_hardcore     = saturn_set_hardcore,
        .detect_protocol  = saturn_detect_protocol,
        .console_id       = 39,  // RC_CONSOLE_SATURN
        .name             = "Saturn",
        .hardcore_protected = 0
};
