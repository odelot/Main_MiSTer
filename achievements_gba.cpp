// achievements_gba.cpp — RetroAchievements GBA-specific implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// GBA State
// ---------------------------------------------------------------------------

static console_state_t g_gba_state = {};

// ---------------------------------------------------------------------------
// GBA Option C Diagnostics
// ---------------------------------------------------------------------------

static void gba_optionc_dump_valcache(const char *label, void *map)
{
        if (!map) return;
        const uint8_t *base = (const uint8_t *)map;
        int addr_count = ra_snes_addrlist_count();
        const uint32_t *addrs = ra_snes_addrlist_addrs();

        const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
        const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;

        int dump_len = addr_count < 32 ? addr_count : 32;
        if (dump_len <= 0) dump_len = 32;
        char hex[200];
        int pos = 0, non_zero = 0;
        for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
                if (vals[i]) non_zero++;
        }

        ra_log_write("GBA DUMP[%s] resp_id=%u resp_frame=%u addrs=%d non_zero=%d\n",
                label, resp->response_id, resp->response_frame, addr_count, non_zero);
        ra_log_write("GBA DUMP[%s] VALCACHE[0..%d]: %s\n", label, dump_len - 1, hex);

        // FPGA debug words at 0x10 / 0x18
        const uint8_t *dbg8 = base + 0x10;
        uint16_t iwram_cnt   = dbg8[0] | (dbg8[1] << 8);
        uint8_t  dispatch_cnt = dbg8[6];
        uint8_t  fpga_ver     = dbg8[7];
        const uint8_t *dbg8b = base + 0x18;
        uint16_t ewram_cnt  = dbg8b[4] | (dbg8b[5] << 8);
        uint16_t cart_cnt   = dbg8b[2] | (dbg8b[3] << 8);
        uint16_t first_addr = dbg8b[6] | (dbg8b[7] << 8);

        ra_log_write("GBA DUMP[%s] FPGA ver=0x%02X dispatch=%u iwram=%u ewram=%u cart=%u faddr=0x%04X\n",
                label, fpga_ver, dispatch_cnt, iwram_cnt, ewram_cnt, cart_cnt, first_addr);

        // Address+value pairs (first 10)
        int show = addr_count < 10 ? addr_count : 10;
        for (int i = 0; i < show; i++)
                ra_log_write("GBA DUMP[%s]   [%d] addr=0x%05X val=0x%02X\n", label, i, addrs[i], vals[i]);
}

// ---------------------------------------------------------------------------
// GBA Implementation
// ---------------------------------------------------------------------------

static void gba_init(void)
{
        memset(&g_gba_state, 0, sizeof(g_gba_state));
}

static void gba_reset(void)
{
        memset(&g_gba_state, 0, sizeof(g_gba_state));
}

static uint32_t gba_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
        if (g_gba_state.optionc) {
                if (g_gba_state.collecting) {
                        for (uint32_t i = 0; i < num_bytes; i++)
                                ra_snes_addrlist_add(address + i);
                }
                if (g_gba_state.cache_ready) {
                        for (uint32_t i = 0; i < num_bytes; i++)
                                buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
                        return num_bytes;
                }
                memset(buffer, 0, num_bytes);
                return num_bytes;
        }
        return 0;
}

static int gba_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
        if (!client || !game_loaded || !map || !g_gba_state.optionc)
                return 0;

        rc_client_t *rc_client = (rc_client_t *)client;

        if (ra_snes_addrlist_count() == 0 && !g_gba_state.cache_ready) {
                // Bootstrap: run one do_frame with zeros to discover needed addresses
                g_gba_state.collecting = 1;
                ra_snes_addrlist_begin_collect();
                rc_client_do_frame(rc_client);
                g_gba_state.collecting = 0;
                int changed = ra_snes_addrlist_end_collect(map);
                if (changed) {
                        ra_log_write("GBA OptionC: Bootstrap collection done, %d addrs written to DDRAM\n",
                                ra_snes_addrlist_count());
                        gba_optionc_dump_valcache("bootstrap", map);
                } else {
                        ra_log_write("GBA OptionC: No addresses collected\n");
                }
        } else if (!g_gba_state.cache_ready) {
                // Wait for FPGA response
                if (ra_snes_addrlist_is_ready(map)) {
                        g_gba_state.cache_ready = 1;
                        g_gba_state.last_resp_frame = 0;
                        g_gba_state.game_frames = 0;
                        g_gba_state.poll_logged = 0;
                        clock_gettime(CLOCK_MONOTONIC, &g_gba_state.cache_time);
                        ra_log_write("GBA OptionC: Cache active! FPGA response matched request.\n");
                        gba_optionc_dump_valcache("cache-active", map);
                }
        } else {
                // Normal frame processing from cache
                uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
                if (resp_frame > g_gba_state.last_resp_frame) {
                        g_gba_state.last_resp_frame = resp_frame;
                        g_gba_state.game_frames++;
                        ra_frame_processed(resp_frame);

                        if (g_gba_state.game_frames <= 5) {
                                ra_log_write("GBA OptionC: GameFrame %u (resp_frame=%u)\n",
                                        g_gba_state.game_frames, resp_frame);
                                gba_optionc_dump_valcache("early-frame", map);
                        }

                        // Re-collect every ~5 min
                        int re_collect = (g_gba_state.game_frames % 18000 == 0) && (g_gba_state.game_frames > 0);
                        if (re_collect) {
                                g_gba_state.collecting = 1;
                                ra_snes_addrlist_begin_collect();
                        }

                        rc_client_do_frame(rc_client);

                        if (re_collect) {
                                g_gba_state.collecting = 0;
                                if (ra_snes_addrlist_end_collect(map)) {
                                        ra_log_write("GBA OptionC: Address list refreshed, %d addrs\n",
                                                ra_snes_addrlist_count());
                                }
                        }
                }
        }

        // Periodic debug logging
        uint32_t milestone = g_gba_state.game_frames / 300;
        if (milestone > 0 && milestone != g_gba_state.poll_logged) {
                g_gba_state.poll_logged = milestone;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed = (now.tv_sec - g_gba_state.cache_time.tv_sec)
                        + (now.tv_nsec - g_gba_state.cache_time.tv_nsec) / 1e9;
                double ms_per_cycle = (g_gba_state.game_frames > 0) ?
                        (elapsed * 1000.0 / g_gba_state.game_frames) : 0.0;
                ra_log_write("POLL(GBA): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
                        g_gba_state.last_resp_frame, g_gba_state.game_frames, elapsed, ms_per_cycle,
                        ra_snes_addrlist_count());
                if ((g_gba_state.game_frames % 1800) < 300)
                        gba_optionc_dump_valcache("periodic", map);
        }

        return 1; // GBA Option C handled
#else
        return 0;
#endif
}

static int gba_calculate_hash(const char *rom_path, char *md5_hex_out)
{
#ifdef HAS_RCHEEVOS
        char abs_path[1024];
        if (rom_path[0] == '/') {
                snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
        } else {
                extern const char *getRootDir(void);
                snprintf(abs_path, sizeof(abs_path), "%s/%s", getRootDir(), rom_path);
        }

        if (rc_hash_generate_from_file(md5_hex_out, 5, abs_path)) {
                ra_log_write("GBA hash: %s\n", md5_hex_out);
                return 1;
        }
        ra_log_write("GBA: rc_hash_generate_from_file failed for %s\n", abs_path);
#endif
        return 0;
}

static void gba_set_hardcore(int enabled)
{
        // GBA: disable cheats and save states in hardcore mode
        // TODO: verify correct status bit indices for GBA core
        ra_log_write("GBA: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static void gba_detect_protocol(void *map)
{
        if (!map) return;
        // GBA always uses Option C
        g_gba_state.optionc = 1;
        ra_log_write("GBA FPGA protocol: Option C (selective address reading)\n");
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_gba = {
        .init = gba_init,
        .reset = gba_reset,
        .read_memory = gba_read_memory,
        .poll = gba_poll,
        .calculate_hash = gba_calculate_hash,
        .set_hardcore = gba_set_hardcore,
        .detect_protocol = gba_detect_protocol,
        .console_id = 5,  // RC_CONSOLE_GAME_BOY_ADVANCE
        .name = "GBA"
};
