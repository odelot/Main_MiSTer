// achievements_tgfx16.cpp — RetroAchievements TurboGrafx-16 / PC Engine implementation

#include "achievements_console.h"
#include "achievements.h"
#include "ra_ramread.h"
#include "user_io.h"
#include "lib/md5/md5.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#endif

// ---------------------------------------------------------------------------
// TG16 State
// ---------------------------------------------------------------------------

static console_state_t g_tgfx16_state = {0};

// ---------------------------------------------------------------------------
// TG16 Option C Diagnostics
// ---------------------------------------------------------------------------

static void tgfx16_optionc_dump_valcache(const char *label, void *map)
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

ra_log_write("TGFX16 DUMP[%s] resp_id=%u resp_frame=%u addrs=%d non_zero=%d\n",
label, resp->response_id, resp->response_frame, addr_count, non_zero);
ra_log_write("TGFX16 DUMP[%s] VALCACHE[0..%d]: %s\n", label, dump_len - 1, hex);

// FPGA debug words
const uint8_t *dbg8 = base + 0x10;
uint16_t ok_cnt      = dbg8[0] | (dbg8[1] << 8);
uint16_t timeout_cnt = dbg8[2] | (dbg8[3] << 8);
uint8_t  dispatch_cnt = dbg8[6];
uint8_t  fpga_ver     = dbg8[7];
const uint8_t *dbg8b = base + 0x18;
uint16_t wram_cnt = dbg8b[0] | (dbg8b[1] << 8);
uint16_t cdram_cnt = dbg8b[2] | (dbg8b[3] << 8);
uint16_t first_addr = dbg8b[6] | (dbg8b[7] << 8);

ra_log_write("TGFX16 DUMP[%s] FPGA ver=0x%02X ok=%u timeout=%u dispatch=%u wram=%u cdram=%u faddr=0x%04X\n",
label, fpga_ver, ok_cnt, timeout_cnt, dispatch_cnt, wram_cnt, cdram_cnt, first_addr);

int show = addr_count < 10 ? addr_count : 10;
for (int i = 0; i < show; i++)
ra_log_write("TGFX16 DUMP[%s]   [%d] addr=0x%05X val=0x%02X\n", label, i, addrs[i], vals[i]);
}

// ---------------------------------------------------------------------------
// TG16 Implementation
// ---------------------------------------------------------------------------

static void tgfx16_init(void)
{
memset(&g_tgfx16_state, 0, sizeof(g_tgfx16_state));
}

static void tgfx16_reset(void)
{
memset(&g_tgfx16_state, 0, sizeof(g_tgfx16_state));
}

static uint32_t tgfx16_read_memory(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes)
{
if (g_tgfx16_state.optionc) {
if (g_tgfx16_state.collecting) {
for (uint32_t i = 0; i < num_bytes; i++)
ra_snes_addrlist_add(address + i);
}
if (g_tgfx16_state.cache_ready) {
for (uint32_t i = 0; i < num_bytes; i++)
buffer[i] = ra_snes_addrlist_read_cached(map, address + i);
return num_bytes;
}
memset(buffer, 0, num_bytes);
return num_bytes;
}
memset(buffer, 0, num_bytes);
return num_bytes;
}

static int tgfx16_poll(void *map, void *client, int game_loaded)
{
#ifdef HAS_RCHEEVOS
if (!client || !game_loaded || !map || !g_tgfx16_state.optionc)
return 0;

rc_client_t *rc_client = (rc_client_t *)client;

if (ra_snes_addrlist_count() == 0 && !g_tgfx16_state.cache_ready) {
// Bootstrap: collect addresses
g_tgfx16_state.collecting = 1;
ra_snes_addrlist_begin_collect();
rc_client_do_frame(rc_client);
g_tgfx16_state.collecting = 0;
int changed = ra_snes_addrlist_end_collect(map);
if (changed) {
ra_log_write("TGFX16 OptionC: Bootstrap done, %d addrs\n",
ra_snes_addrlist_count());
tgfx16_optionc_dump_valcache("bootstrap", map);
} else {
ra_log_write("TGFX16 OptionC: No addresses collected\n");
}
} else if (!g_tgfx16_state.cache_ready) {
// Wait for FPGA response
if (ra_snes_addrlist_is_ready(map)) {
g_tgfx16_state.cache_ready = 1;
g_tgfx16_state.last_resp_frame = 0;
g_tgfx16_state.game_frames = 0;
g_tgfx16_state.poll_logged = 0;
clock_gettime(CLOCK_MONOTONIC, &g_tgfx16_state.cache_time);
ra_log_write("TGFX16 OptionC: Cache active!\n");
tgfx16_optionc_dump_valcache("cache-active", map);
}
} else {
// Normal frame processing
uint32_t resp_frame = ra_snes_addrlist_response_frame(map);
if (resp_frame > g_tgfx16_state.last_resp_frame) {
g_tgfx16_state.last_resp_frame = resp_frame;
g_tgfx16_state.game_frames++;
ra_frame_processed(resp_frame);

if (g_tgfx16_state.game_frames <= 5) {
ra_log_write("TGFX16 OptionC: GameFrame %u (resp_frame=%u)\n",
g_tgfx16_state.game_frames, resp_frame);
tgfx16_optionc_dump_valcache("early-frame", map);
}

// Re-collect every ~5 min
int re_collect = (g_tgfx16_state.game_frames % 18000 == 0) && (g_tgfx16_state.game_frames > 0);
if (re_collect) {
g_tgfx16_state.collecting = 1;
ra_snes_addrlist_begin_collect();
}

rc_client_do_frame(rc_client);

if (re_collect) {
g_tgfx16_state.collecting = 0;
if (ra_snes_addrlist_end_collect(map)) {
ra_log_write("TGFX16 OptionC: Address list refreshed, %d addrs\n",
ra_snes_addrlist_count());
}
}
}
}

// Periodic debug logging
uint32_t milestone = g_tgfx16_state.game_frames / 300;
if (milestone > 0 && milestone != g_tgfx16_state.poll_logged) {
g_tgfx16_state.poll_logged = milestone;
struct timespec now;
clock_gettime(CLOCK_MONOTONIC, &now);
double elapsed = (now.tv_sec - g_tgfx16_state.cache_time.tv_sec)
+ (now.tv_nsec - g_tgfx16_state.cache_time.tv_nsec) / 1e9;
double ms_per_cycle = (g_tgfx16_state.game_frames > 0) ?
(elapsed * 1000.0 / g_tgfx16_state.game_frames) : 0.0;
ra_log_write("POLL(TGFX16): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d\n",
g_tgfx16_state.last_resp_frame, g_tgfx16_state.game_frames, elapsed, ms_per_cycle,
ra_snes_addrlist_count());
if ((g_tgfx16_state.game_frames % 1800) < 300)
tgfx16_optionc_dump_valcache("periodic", map);
}

return 1;
#else
return 0;
#endif
}

static int tgfx16_calculate_hash(const char *rom_path, char *md5_hex_out)
{
// PC Engine ROMs: .pce files may have a 512-byte header if size % 1024 == 512
FILE *f = fopen(rom_path, "rb");
if (!f) {
ra_log_write("TGFX16: Failed to open ROM for hashing: %s\n", rom_path);
return 0;
}

fseek(f, 0, SEEK_END);
long file_size = ftell(f);
fseek(f, 0, SEEK_SET);

if (file_size <= 0) { fclose(f); return 0; }

uint8_t *rom_data = (uint8_t *)malloc(file_size);
if (!rom_data) { fclose(f); return 0; }

size_t nread = fread(rom_data, 1, file_size, f);
fclose(f);

if ((long)nread != file_size) { free(rom_data); return 0; }

const uint8_t *hash_data = rom_data;
long hash_size = file_size;

// Skip 512-byte copier header if present
if ((file_size % 1024) == 512 && file_size > 512) {
ra_log_write("TGFX16: Copier header detected, skipping 512 bytes\n");
hash_data = rom_data + 512;
hash_size = file_size - 512;
}

MD5_CTX ctx;
MD5Init(&ctx);
MD5Update(&ctx, hash_data, hash_size);
unsigned char md5_bin[16];
MD5Final(md5_bin, &ctx);
for (int i = 0; i < 16; i++)
sprintf(&md5_hex_out[i * 2], "%02x", md5_bin[i]);
md5_hex_out[32] = '\0';

free(rom_data);
return 1;
}

static void tgfx16_set_hardcore(int enabled)
{
// TG16 core: disable cheats (status bit 5)
user_io_status_set("[5]", enabled ? 1 : 0);
ra_log_write("TGFX16: Hardcore mode %s\n", enabled ? "enabled" : "disabled");
}

static void tgfx16_detect_protocol(void *map)
{
if (!map) return;
const ra_header_t *hdr = (const ra_header_t *)map;
if (hdr->region_count == 0) {
g_tgfx16_state.optionc = 1;
ra_log_write("TGFX16 FPGA protocol: Option C (selective address reading)\n");
} else {
g_tgfx16_state.optionc = 0;
ra_log_write("TGFX16 FPGA protocol: VBlank-gated mirror (region_count=%d)\n",
hdr->region_count);
}
}

// ---------------------------------------------------------------------------
// Console handler definition
// ---------------------------------------------------------------------------

const console_handler_t g_console_tgfx16 = {
.init = tgfx16_init,
.reset = tgfx16_reset,
.read_memory = tgfx16_read_memory,
.poll = tgfx16_poll,
.calculate_hash = tgfx16_calculate_hash,
.set_hardcore = tgfx16_set_hardcore,
.detect_protocol = tgfx16_detect_protocol,
.console_id = 8,  // RC_CONSOLE_PC_ENGINE
.name = "TGFX16"
};
