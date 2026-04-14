// achievements.cpp — RetroAchievements integration for MiSTer FPGA
//
// Phase 4: Full pipeline with OSD notifications — achievement popups,
// login/game status, progress indicators, status info panel.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>

#include "achievements.h"
#include "ra_ramread.h"
#include "ra_http.h"
#include "user_io.h"
#include "file_io.h"
#include "menu.h"
#include "hardware.h"
#include "shmem.h"
#include "lib/md5/md5.h"

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_api_request.h"
#include "rc_hash.h"
#endif

// ---------------------------------------------------------------------------
// Debug logging
// ---------------------------------------------------------------------------

static FILE *g_logfile = NULL;

#define RA_LOG(fmt, ...) ra_log_impl("RA: " fmt "\n", ##__VA_ARGS__)

void ra_log_write(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	printf("\033[1;35m");
	vprintf(fmt, args);
	printf("\033[0m");
	va_end(args);
	if (g_logfile) {
		va_start(args, fmt);
		vfprintf(g_logfile, fmt, args);
		va_end(args);
		fflush(g_logfile);
	}
}

static void ra_log_impl(const char *fmt, ...)
{
	va_list args;

	// stdout with color
	va_start(args, fmt);
	printf("\033[1;35m");
	vprintf(fmt, args);
	printf("\033[0m");
	va_end(args);

	// log file without color
	if (g_logfile) {
		va_start(args, fmt);
		vfprintf(g_logfile, fmt, args);
		va_end(args);
		fflush(g_logfile);
	}
}

static void ra_log_open(void)
{
	if (!g_logfile) {
		g_logfile = fopen("/tmp/ra_debug.log", "w");
		if (g_logfile) {
			time_t now = time(NULL);
			fprintf(g_logfile, "=== RetroAchievements Debug Log ===\n");
			fprintf(g_logfile, "Started: %s\n", ctime(&now));
			fflush(g_logfile);
		}
	}
}

static void ra_log_close(void)
{
	if (g_logfile) {
		time_t now = time(NULL);
		fprintf(g_logfile, "Closed: %s\n", ctime(&now));
		fclose(g_logfile);
		g_logfile = NULL;
	}
}

// ---------------------------------------------------------------------------
// Crash signal handler — writes backtrace to log before dying
// ---------------------------------------------------------------------------
static void ra_crash_handler(int sig)
{
	const char *name = (sig == SIGSEGV) ? "SIGSEGV" :
	                   (sig == SIGBUS)  ? "SIGBUS"  :
	                   (sig == SIGABRT) ? "SIGABRT" :
	                   (sig == SIGFPE)  ? "SIGFPE"  : "UNKNOWN";

	// Write directly to log file (async-signal-safe is best-effort here)
	if (g_logfile) {
		fprintf(g_logfile, "\n!!! CRASH: signal %s (%d) !!!\n", name, sig);
		void *bt[32];
		int n = backtrace(bt, 32);
		backtrace_symbols_fd(bt, n, fileno(g_logfile));
		fflush(g_logfile);
	}

	// Also print to stderr
	fprintf(stderr, "\n!!! RA CRASH: signal %s (%d) !!!\n", name, sig);
	void *bt[32];
	int n = backtrace(bt, 32);
	backtrace_symbols_fd(bt, n, STDERR_FILENO);

	// Re-raise to get default behavior (core dump)
	signal(sig, SIG_DFL);
	raise(sig);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static void *g_ra_map = NULL;        // DDRAM mirror mmap pointer
static uint32_t g_last_frame = 0;    // Last processed frame counter
static uint32_t g_first_frame = 0;   // First valid frame seen (for uptime tracking)
static int g_game_loaded = 0;        // Game is loaded and identified
static int g_mirror_validated = 0;   // DDRAM mirror has been validated at least once
static char g_rom_md5[33] = {};      // MD5 hex string of current ROM
static char g_rom_path[1024] = {};   // Path to current ROM

// SNES state
static int g_snes_optionc = 0;       // 1 if FPGA has Option C protocol, 0 for VBlank-gated
static int g_snes_collecting = 0;    // 1 during address collection do_frame
static int g_snes_cache_ready = 0;   // 1 when FPGA response matches request
static uint32_t g_snes_last_resp_frame = 0; // Last processed response frame
static uint32_t g_snes_game_frames = 0;     // Frames processed since cache active
static uint32_t g_snes_poll_logged = 0;     // Last g_snes_game_frames milestone logged
static struct timespec g_snes_cache_time;   // Timestamp when cache became active

// PSX state (uses same Option C infrastructure as SNES)
static int g_psx_optionc = 0;
static int g_psx_collecting = 0;
static int g_psx_cache_ready = 0;
static int g_psx_needs_recollect = 0; // pointer-resolution pass after boot
static uint32_t g_psx_last_resp_frame = 0;
static uint32_t g_psx_game_frames = 0;
static uint32_t g_psx_poll_logged = 0;
static struct timespec g_psx_cache_time;

// MegaDrive/Genesis state (Option C, same infrastructure)
static int g_md_optionc = 0;
static int g_md_collecting = 0;
static int g_md_cache_ready = 0;
static uint32_t g_md_last_resp_frame = 0;
static uint32_t g_md_game_frames = 0;
static uint32_t g_md_poll_logged = 0;
static struct timespec g_md_cache_time;

// N64 state (Option C, same infrastructure)
static int g_n64_optionc = 0;
static int g_n64_collecting = 0;
static int g_n64_cache_ready = 0;
static int g_n64_needs_recollect = 0;
static uint32_t g_n64_last_resp_frame = 0;
static uint32_t g_n64_game_frames = 0;
static uint32_t g_n64_poll_logged = 0;
static struct timespec g_n64_cache_time;

// Gameboy/Gameboy Color state (Option C, same infrastructure)
static int g_gb_optionc = 0;
static int g_gb_collecting = 0;
static int g_gb_cache_ready = 0;
static uint32_t g_gb_last_resp_frame = 0;
static uint32_t g_gb_game_frames = 0;
static uint32_t g_gb_poll_logged = 0;
static struct timespec g_gb_cache_time;

// SMS / Game Gear state (Option C, same infrastructure)
static int g_sms_optionc = 0;
static int g_sms_collecting = 0;
static int g_sms_cache_ready = 0;
static uint32_t g_sms_last_resp_frame = 0;
static uint32_t g_sms_game_frames = 0;
static uint32_t g_sms_poll_logged = 0;
static struct timespec g_sms_cache_time;

// GBA state (Option C, same infrastructure)
static int g_gba_optionc = 0;
static int g_gba_collecting = 0;
static int g_gba_cache_ready = 0;
static uint32_t g_gba_last_resp_frame = 0;
static uint32_t g_gba_game_frames = 0;
static uint32_t g_gba_poll_logged = 0;
static struct timespec g_gba_cache_time;

// NeoGeo state (Option C, same infrastructure)
static int g_neogeo_optionc = 0;
static int g_neogeo_collecting = 0;
static int g_neogeo_cache_ready = 0;
static uint32_t g_neogeo_last_resp_frame = 0;
static uint32_t g_neogeo_game_frames = 0;
static uint32_t g_neogeo_poll_logged = 0;
static struct timespec g_neogeo_cache_time;

#ifdef HAS_RCHEEVOS
static rc_client_t *g_client = NULL;
#endif

// Credentials
static char g_ra_user[128] = {};
static char g_ra_password[128] = {};
static int g_logged_in = 0;
static int g_login_pending = 0;
static int g_game_load_pending = 0;

// Debug counters
static uint32_t g_frames_processed = 0;
static uint32_t g_frames_skipped = 0;  // frames where busy flag was set
static time_t g_load_time = 0;

// Config file path
#define RA_CFG_PATH  "/media/fat/retroachievements.cfg"
#define RA_SFX_PATH  "/media/fat/achievement.wav"

// Popup display settings (from retroachievements.cfg)
static int g_show_challenge_show_popup = 1; // 1 = show popup on challenge SHOW event
static int g_show_challenge_hide_popup = 1; // 1 = show popup on challenge HIDE event
static int g_show_progress_popups      = 1; // 1 = show progress indicator popups
static int g_show_progress_name        = 1; // 1 = include achievement name in progress popup
static int g_hardcore                  = 0; // 1 = hardcore mode (disables cheats & save states)
static char g_ua_clause[64]            = ""; // rcheevos user-agent clause (e.g. "rcheevos/11.6")

// ---------------------------------------------------------------------------
// Achievement Sound
// ---------------------------------------------------------------------------

static void *ra_play_thread(void *arg)
{
	(void)arg;
	// Only play if the file exists — silent no-op otherwise
	if (access(RA_SFX_PATH, R_OK) == 0)
		system("aplay -q " RA_SFX_PATH " 2>/dev/null");
	return NULL;
}

static void ra_play_achievement_sound(void)
{
	pthread_t th;
	if (pthread_create(&th, NULL, ra_play_thread, NULL) == 0)
		pthread_detach(th);
}

// ---------------------------------------------------------------------------
// OSD Notification — two-tier system
//
// Tier 1 URGENT (queued): achievement unlocked, game completed.
//   → Multiple unlocks accumulate; each is shown in order, never interrupted.
//
// Tier 2 INSTANT (single slot, last-wins): progress, challenge, etc.
//   → Shows immediately, overwriting any currently displayed instant.
//   → Silently discarded if a Tier 1 notification is on screen.
// ---------------------------------------------------------------------------

#define NOTIF_QUEUE_CAP 8
#define NOTIF_TEXT_MAX  200

struct ra_notif {
	char text[NOTIF_TEXT_MAX];
	int duration_ms;
};

// Tier 1 — urgent queue
static ra_notif s_urgent_queue[NOTIF_QUEUE_CAP];
static int s_urgent_head = 0;
static int s_urgent_tail = 0;
static int s_urgent_showing = 0;
static unsigned long s_urgent_timer = 0;

// Tier 2 — instant slot
static char s_instant_text[NOTIF_TEXT_MAX] = {0};
static int  s_instant_duration_ms = 3000;
static int  s_instant_pending = 0;
static int  s_instant_showing = 0;
static unsigned long s_instant_timer = 0;

// Add to urgent queue (never dropped by instant notifications)
static void ra_notify_urgent(const char *text, int duration_ms = 4000)
{
	int count = s_urgent_head - s_urgent_tail;
	if (count >= NOTIF_QUEUE_CAP) {
		s_urgent_tail++;
		RA_LOG("OSD: Urgent queue full, dropping oldest");
	}
	ra_notif *n = &s_urgent_queue[s_urgent_head % NOTIF_QUEUE_CAP];
	snprintf(n->text, NOTIF_TEXT_MAX, "%s", text);
	n->duration_ms = duration_ms;
	s_urgent_head++;
}

// Set instant slot — last event wins; discarded in poll if urgent is showing
static void ra_notify_instant(const char *text, int duration_ms = 3000)
{
	snprintf(s_instant_text, NOTIF_TEXT_MAX, "%s", text);
	s_instant_duration_ms = duration_ms;
	s_instant_pending = 1;
}

// Aliases kept for call-site readability
static void ra_notify(const char *text, int duration_ms = 3000)
{
	ra_notify_instant(text, duration_ms);
}

static void ra_notify_progress(const char *text)
{
	ra_notify_instant(text, 2500);
}

// Drive OSD display — called every achievements_poll() tick
static void ra_osd_poll(void)
{
	// Expire timers
	if (s_urgent_showing && CheckTimer(s_urgent_timer))
		s_urgent_showing = 0;
	if (s_instant_showing && CheckTimer(s_instant_timer))
		s_instant_showing = 0;

	if (menu_present()) return;

	// Tier 1: show next urgent as soon as previous one expires
	if (!s_urgent_showing && s_urgent_head != s_urgent_tail) {
		ra_notif *n = &s_urgent_queue[s_urgent_tail % NOTIF_QUEUE_CAP];
		s_urgent_tail++;
		Info(n->text, n->duration_ms + 500, 0, 0, 1);
		s_urgent_timer    = GetTimer(n->duration_ms);
		s_urgent_showing  = 1;
		// Urgent takes over the display — discard any pending instant
		s_instant_pending = 0;
		s_instant_showing = 0;
		RA_LOG("OSD: Showing urgent notification (%dms)", n->duration_ms);
		return;
	}

	// Tier 2: instant slot — show immediately; discard if urgent is on screen
	if (s_instant_pending) {
		s_instant_pending = 0;
		if (!s_urgent_showing) {
			Info(s_instant_text, s_instant_duration_ms + 500, 0, 0, 1);
			s_instant_timer   = GetTimer(s_instant_duration_ms);
			s_instant_showing = 1;
			RA_LOG("OSD: Showing instant notification (%dms)", s_instant_duration_ms);
		} else {
			RA_LOG("OSD: Instant notification discarded (urgent showing)");
		}
	}
}

// ---------------------------------------------------------------------------
// ROM MD5 calculation
// ---------------------------------------------------------------------------

static int ra_get_console_id(void); // forward declaration
static int ra_core_supported(void); // forward declaration

// Compute the RetroAchievements MD5 for a ROM file.
// For NES: skips the 16-byte iNES header (and optional 512-byte trainer)
// so the hash matches what RetroAchievements expects.
static int ra_calculate_rom_md5(const char *path, char *md5_hex_out)
{
	fileTYPE f = {};
	if (!FileOpen(&f, path, 1)) {
		RA_LOG("ERROR: Cannot open ROM file: %s", path);
		return 0;
	}

	uint32_t file_size = f.size;
	RA_LOG("Hashing ROM: %s (%u bytes)", path, file_size);

	// Read entire file
	uint8_t *rom_data = (uint8_t *)malloc(file_size);
	if (!rom_data) {
		RA_LOG("ERROR: malloc failed for ROM buffer (%u bytes)", file_size);
		FileClose(&f);
		return 0;
	}

	int rd = FileReadAdv(&f, rom_data, file_size);
	FileClose(&f);

	if (rd <= 0 || (uint32_t)rd != file_size) {
		RA_LOG("ERROR: Failed to read ROM (got %d of %u bytes)", rd, file_size);
		free(rom_data);
		return 0;
	}

	const uint8_t *hash_data = rom_data;
	uint32_t hash_size = file_size;

	// NES: skip iNES header ("NES\x1a") + optional 512-byte trainer
	if (file_size > 16 &&
		rom_data[0] == 0x4E && rom_data[1] == 0x45 &&  // 'N' 'E'
		rom_data[2] == 0x53 && rom_data[3] == 0x1A) {  // 'S' 0x1a
		uint32_t skip = 16;
		if (rom_data[6] & 0x04) skip += 512; // trainer present
		RA_LOG("iNES header detected, skipping %u bytes (trainer=%d)",
			skip, (rom_data[6] & 0x04) ? 1 : 0);
		hash_data = rom_data + skip;
		hash_size = file_size - skip;
	}

	// SNES: skip optional 512-byte SMC/SWC copier header
	if ((file_size % 1024) == 512 && file_size > 512) {
		RA_LOG("SNES SMC header detected (file_size %% 1024 == 512), skipping 512 bytes");
		hash_data = rom_data + 512;
		hash_size = file_size - 512;
	}

	struct MD5Context ctx;
	MD5Init(&ctx);
	MD5Update(&ctx, hash_data, hash_size);
	unsigned char digest[16];
	MD5Final(digest, &ctx);
	for (int i = 0; i < 16; i++)
		sprintf(md5_hex_out + i * 2, "%02x", digest[i]);
	md5_hex_out[32] = '\0';

	free(rom_data);
	RA_LOG("ROM MD5: %s", md5_hex_out);
	return 1;
}

// ---------------------------------------------------------------------------
// Credentials loading
// ---------------------------------------------------------------------------

// Config file format (/media/fat/retroachievements.cfg):
//   username=YourRAUsername
//   password=YourRAPassword
//   # Lines starting with # are comments

static int ra_load_credentials(void)
{
	g_ra_user[0] = '\0';
	g_ra_password[0] = '\0';

	FILE *f = fopen(RA_CFG_PATH, "r");
	if (!f) {
		RA_LOG("Credentials file not found: %s", RA_CFG_PATH);
		RA_LOG("To enable RetroAchievements, create the file with:");
		RA_LOG("  username=YourRAUsername");
		RA_LOG("  password=YourRAPassword");
		return 0;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		// Strip newline
		char *nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		nl = strchr(line, '\r');
		if (nl) *nl = '\0';

		// Skip comments and empty lines
		if (line[0] == '#' || line[0] == '\0') continue;

		char *eq = strchr(line, '=');
		if (!eq) continue;

		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;

		// Trim leading spaces from value
		while (*val == ' ' || *val == '\t') val++;

		if (!strcasecmp(key, "username")) {
			snprintf(g_ra_user, sizeof(g_ra_user), "%s", val);
		} else if (!strcasecmp(key, "password")) {
			snprintf(g_ra_password, sizeof(g_ra_password), "%s", val);
		} else if (!strcasecmp(key, "show_challenge_show_popup")) {
			g_show_challenge_show_popup = atoi(val);
		} else if (!strcasecmp(key, "show_challenge_hide_popup")) {
			g_show_challenge_hide_popup = atoi(val);
		} else if (!strcasecmp(key, "show_progress_popups")) {
			g_show_progress_popups = atoi(val);
		} else if (!strcasecmp(key, "show_progress_name")) {
			g_show_progress_name = atoi(val);
		} else if (!strcasecmp(key, "hardcore")) {
			g_hardcore = atoi(val);
		}
	}
	fclose(f);

	if (!g_ra_user[0] || !g_ra_password[0]) {
		RA_LOG("Credentials incomplete (need both username and password)");
		return 0;
	}

	RA_LOG("Credentials loaded: user=%s password=***(%zu chars)", g_ra_user, strlen(g_ra_password));
	RA_LOG("Config: show_challenge_show=%d show_challenge_hide=%d show_progress=%d show_progress_name=%d hardcore=%d",
		g_show_challenge_show_popup, g_show_challenge_hide_popup,
		g_show_progress_popups, g_show_progress_name, g_hardcore);
	return 1;
}

// ---------------------------------------------------------------------------
// SNES Option C VALCACHE diagnostic dump (uses RA_LOG)
// ---------------------------------------------------------------------------
static void snes_optionc_dump_valcache(const char *label)
{
	if (!g_ra_map) return;
	const uint8_t *base = (const uint8_t *)g_ra_map;
	int addr_count = ra_snes_addrlist_count();
	const uint32_t *addrs = ra_snes_addrlist_addrs();

	// Response header
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	RA_LOG("DUMP[%s] VALCACHE hdr: resp_id=%u resp_frame=%u",
		label, resp->response_id, resp->response_frame);

	// Raw VALCACHE bytes (first 32)
	const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;
	int dump_len = addr_count < 32 ? addr_count : 32;
	if (dump_len <= 0) dump_len = 32;
	char hex[200];
	int pos = 0;
	int non_zero = 0;
	for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
		pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
		if (vals[i]) non_zero++;
	}
	RA_LOG("DUMP[%s] VALCACHE raw[0..%d]: %s", label, dump_len - 1, hex);

	// Count non-zero in full address range
	int nz_all = 0;
	for (int i = 0; i < addr_count; i++)
		if (vals[i]) nz_all++;
	RA_LOG("DUMP[%s] non-zero: %d/%d", label, nz_all, addr_count);

	// Address+value pairs (first 10)
	int show = addr_count < 10 ? addr_count : 10;
	for (int i = 0; i < show; i++) {
		RA_LOG("DUMP[%s]   [%d] addr=0x%05X val=0x%02X", label, i, addrs[i], vals[i]);
	}
	// Also show a few known SMW addresses if present
	const uint32_t smw_addrs[] = {0x00019, 0x00DBF, 0x00F06, 0x00F31, 0x01490};
	const char *smw_names[] = {"playerSts", "dragonCoin", "lives", "coinCnt", "score"};
	for (int j = 0; j < 5; j++) {
		for (int i = 0; i < addr_count; i++) {
			if (addrs[i] == smw_addrs[j]) {
				RA_LOG("DUMP[%s]   SMW:%s(0x%05X)=0x%02X", label, smw_names[j], smw_addrs[j], vals[i]);
				break;
			}
		}
	}

	// ADDRLIST header readback
	const ra_addr_req_hdr_t *ahdr = (const ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	RA_LOG("DUMP[%s] ADDRLIST hdr readback: addr_count=%u request_id=%u",
		label, ahdr->addr_count, ahdr->request_id);

	// ADDRLIST address readback (first 5 from DDRAM)
	const uint32_t *ddram_addrs = (const uint32_t *)(base + RA_SNES_ADDRLIST_OFFSET + 8);
	int rshow = addr_count < 5 ? addr_count : 5;
	for (int i = 0; i < rshow; i++) {
		RA_LOG("DUMP[%s]   DDRAM_ADDR[%d]=0x%08X (local=0x%05X)", label, i, ddram_addrs[i], addrs[i]);
	}

	// FPGA debug word 1 at offset 0x10 (DDRAM_BASE + 2)
	// Format: {ver(8), dispatch_cnt(8), first_dout(16), timeout_cnt(16), ok_cnt(16)}
	const uint8_t *dbg8 = (const uint8_t *)(base + 0x10);
	uint16_t ok_cnt      = dbg8[0] | (dbg8[1] << 8);
	uint16_t timeout_cnt = dbg8[2] | (dbg8[3] << 8);
	uint16_t first_dout  = dbg8[4] | (dbg8[5] << 8);
	uint8_t  dispatch_cnt = dbg8[6];
	uint8_t  fpga_ver     = dbg8[7];

	// FPGA debug word 2 at offset 0x18 (DDRAM_BASE + 3)
	// Format: {first_addr(16), wram_cnt(16), bsram_cnt(16), max_timeout(16)}
	const uint8_t *dbg8b = (const uint8_t *)(base + 0x18);
	uint16_t max_timeout  = dbg8b[0] | (dbg8b[1] << 8);
	uint16_t bsram_cnt    = dbg8b[2] | (dbg8b[3] << 8);
	uint16_t wram_cnt     = dbg8b[4] | (dbg8b[5] << 8);
	uint16_t first_addr   = dbg8b[6] | (dbg8b[7] << 8);

	RA_LOG("DUMP[%s] FPGA_DBG: ver=0x%02X ok=%u timeout=%u first_dout=0x%04X dispatch=%u",
		label, fpga_ver, ok_cnt, timeout_cnt, first_dout, dispatch_cnt);
	RA_LOG("DUMP[%s] FPGA_DBG2: wram=%u bsram=%u first_addr=0x%04X max_timeout=%u",
		label, wram_cnt, bsram_cnt, first_addr, max_timeout);
	if (fpga_ver != 0x0A) {
		RA_LOG("DUMP[%s] *** WARNING: FPGA version mismatch! Expected 0x0A, got 0x%02X ***",
			label, fpga_ver);
		RA_LOG("DUMP[%s] *** The FPGA bitstream may be outdated. Recompile and redeploy .rbf ***",
			label);
	}

	// Bypass pattern verification: FPGA sets val = addr[7:0] ^ 0xA5
	if (addr_count > 0 && addrs) {
		int bypass_match = 0, bypass_mismatch = 0;
		for (int i = 0; i < addr_count; i++) {
			uint8_t expected = (uint8_t)(addrs[i] & 0xFF) ^ 0xA5;
			if (vals[i] == expected)
				bypass_match++;
			else
				bypass_mismatch++;
		}
		RA_LOG("DUMP[%s] BYPASS_CHECK: match=%d mismatch=%d (expected addr[7:0]^0xA5)",
			label, bypass_match, bypass_mismatch);
		if (bypass_mismatch > 0 && bypass_mismatch <= 5) {
			for (int i = 0; i < addr_count && bypass_mismatch > 0; i++) {
				uint8_t expected = (uint8_t)(addrs[i] & 0xFF) ^ 0xA5;
				if (vals[i] != expected) {
					RA_LOG("DUMP[%s]   MISMATCH[%d] addr=0x%05X got=0x%02X expected=0x%02X",
						label, i, addrs[i], vals[i], expected);
					bypass_mismatch--;
				}
			}
		}
	}
}

static void psx_optionc_dump_valcache(const char *label)
{
	if (!g_ra_map) return;
	const uint8_t *base = (const uint8_t *)g_ra_map;
	int addr_count = ra_snes_addrlist_count();
	const uint32_t *addrs = ra_snes_addrlist_addrs();

	// Response header
	const ra_val_resp_hdr_t *resp = (const ra_val_resp_hdr_t *)(base + RA_SNES_VALCACHE_OFFSET);
	RA_LOG("DUMP[%s] VALCACHE hdr: resp_id=%u resp_frame=%u",
		label, resp->response_id, resp->response_frame);

	// Raw VALCACHE bytes (first 32)
	const uint8_t *vals = base + RA_SNES_VALCACHE_OFFSET + 8;
	int dump_len = addr_count < 32 ? addr_count : 32;
	if (dump_len <= 0) dump_len = 32;
	char hex[200];
	int pos = 0;
	for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
		pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
	}
	RA_LOG("DUMP[%s] VALCACHE raw[0..%d]: %s", label, dump_len - 1, hex);

	// Count non-zero in full address range
	int nz_all = 0;
	for (int i = 0; i < addr_count; i++)
		if (vals[i]) nz_all++;
	RA_LOG("DUMP[%s] non-zero: %d/%d", label, nz_all, addr_count);

	// Address+value pairs (first 16)
	int show = addr_count < 16 ? addr_count : 16;
	for (int i = 0; i < show; i++) {
		RA_LOG("DUMP[%s]   [%d] addr=0x%06X val=0x%02X", label, i, addrs[i], vals[i]);
	}

	// Check THPS achievement-relevant addresses
	const uint32_t thps_addrs[] = {0x0a5be0, 0x0d17c8, 0x0d1414, 0x0d1410, 0x0a42f4, 0x0d16a4};
	const char *thps_names[] = {"gameState", "ptrBase", "check1", "check2", "check3", "levelState"};
	for (int j = 0; j < 6; j++) {
		for (int i = 0; i < addr_count; i++) {
			if (addrs[i] == thps_addrs[j]) {
				RA_LOG("DUMP[%s]   THPS:%s(0x%06X)=0x%02X", label, thps_names[j], thps_addrs[j], vals[i]);
				break;
			}
		}
	}

	// ADDRLIST header readback
	const ra_addr_req_hdr_t *ahdr = (const ra_addr_req_hdr_t *)(base + RA_SNES_ADDRLIST_OFFSET);
	RA_LOG("DUMP[%s] ADDRLIST hdr readback: addr_count=%u request_id=%u",
		label, ahdr->addr_count, ahdr->request_id);

	// FPGA debug words (same layout as SNES mirror)
	const uint8_t *dbg8 = (const uint8_t *)(base + 0x10);
	uint16_t ok_cnt      = dbg8[0] | (dbg8[1] << 8);
	uint16_t timeout_cnt = dbg8[2] | (dbg8[3] << 8);
	uint16_t first_dout  = dbg8[4] | (dbg8[5] << 8);
	uint8_t  dispatch_cnt = dbg8[6];
	uint8_t  fpga_ver     = dbg8[7];

	// Debug word 2: {first_addr(16), saveram_cnt(8), iwram_cnt(8), ewram_cnt(16), max_timeout(16)}
	const uint8_t *dbg8b = (const uint8_t *)(base + 0x18);
	uint16_t max_timeout   = dbg8b[0] | (dbg8b[1] << 8);
	uint16_t ewram_cnt     = dbg8b[2] | (dbg8b[3] << 8);
	uint8_t  iwram_cnt     = dbg8b[4];
	uint8_t  saveram_cnt   = dbg8b[5];
	uint16_t first_addr    = dbg8b[6] | (dbg8b[7] << 8);

	RA_LOG("DUMP[%s] FPGA_DBG: ver=0x%02X ok=%u timeout=%u first_dout=0x%04X dispatch=%u max_timeout=%u first_addr=0x%04X iwram=%u ewram=%u saveram=%u",
		label, fpga_ver, ok_cnt, timeout_cnt, first_dout, dispatch_cnt, max_timeout, first_addr, iwram_cnt, ewram_cnt, saveram_cnt);
	if (fpga_ver != 0x02) {
		RA_LOG("DUMP[%s] *** WARNING: FPGA version mismatch! Expected 0x02, got 0x%02X ***",
			label, fpga_ver);
	}
}

// ---------------------------------------------------------------------------
// rcheevos callbacks (compiled only if library is available)
// ---------------------------------------------------------------------------

#ifdef HAS_RCHEEVOS

static uint32_t ra_read_memory(uint32_t address, uint8_t *buffer,
	uint32_t num_bytes, rc_client_t *client)
{
	(void)client;
	if (!g_ra_map || !ra_ramread_active(g_ra_map)) {
		memset(buffer, 0, num_bytes);
		// For supported consoles, return num_bytes so rcheevos keeps
		// addresses valid during the mirror startup window (FPGA may
		// not have written the magic header yet after a core reset).
		return ra_core_supported() ? num_bytes : 0;
	}

	int console = ra_get_console_id();
	switch (console) {
		case 3:  // RC_CONSOLE_SUPER_NINTENDO
			if (g_snes_optionc) {
				// Option C: selective address reading
				if (g_snes_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_snes_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			} else {
				// VBlank-gated: read from full WRAM mirror
				return ra_ramread_snes_read(g_ra_map, address, buffer, num_bytes);
			}
		case 7:  // RC_CONSOLE_NINTENDO (NES)
			return ra_ramread_nes_read(g_ra_map, address, buffer, num_bytes);
		case 1:  // RC_CONSOLE_MEGA_DRIVE (Genesis)
			if (g_md_optionc) {
				if (g_md_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_md_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 12: // RC_CONSOLE_PLAYSTATION (PSX)
			if (g_psx_optionc) {
				if (g_psx_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_psx_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 2: // RC_CONSOLE_NINTENDO_64 (N64)
			// N64 rcheevos addresses are big-endian; RDRAM in DDR3 is
			// little-endian within 32-bit words.  XOR 3 on the low 2 bits
			// converts between the two byte orders.
			if (g_n64_optionc) {
				if (g_n64_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(((address + i) ^ 3));
				}
				if (g_n64_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, ((address + i) ^ 3));
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 11: // RC_CONSOLE_MASTER_SYSTEM
		case 15: // RC_CONSOLE_GAME_GEAR
			if (g_sms_optionc) {
				if (g_sms_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_sms_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 5:  // RC_CONSOLE_GAMEBOY_ADVANCE
			if (g_gba_optionc) {
				if (g_gba_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_gba_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 4:  // RC_CONSOLE_GAMEBOY
		case 6:  // RC_CONSOLE_GAMEBOY_COLOR
			if (g_gb_optionc) {
				if (g_gb_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_gb_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		case 27: // RC_CONSOLE_ARCADE (NeoGeo)
			if (g_neogeo_optionc) {
				if (g_neogeo_collecting) {
					for (uint32_t i = 0; i < num_bytes; i++)
						ra_snes_addrlist_add(address + i);
				}
				if (g_neogeo_cache_ready) {
					for (uint32_t i = 0; i < num_bytes; i++)
						buffer[i] = ra_snes_addrlist_read_cached(g_ra_map, address + i);
					return num_bytes;
				}
				memset(buffer, 0, num_bytes);
				return num_bytes;
			}
			memset(buffer, 0, num_bytes);
			return num_bytes;
		default:
			memset(buffer, 0, num_bytes);
			return num_bytes;
	}
}

static void ra_server_call(const rc_api_request_t *request,
	rc_client_server_callback_t callback, void *callback_data,
	rc_client_t *client)
{
	(void)client;

	// Log the request (mask token for security)
	if (request->post_data) {
		const char *token_pos = strstr(request->post_data, "&t=");
		if (token_pos) {
			int prefix_len = (int)(token_pos - request->post_data);
			RA_LOG("HTTP: POST %s [%.*s&t=***]", request->url, prefix_len, request->post_data);
		} else {
			RA_LOG("HTTP: POST %s [%.80s%s]", request->url,
				request->post_data, strlen(request->post_data) > 80 ? "..." : "");
		}
	} else {
		RA_LOG("HTTP: GET %s", request->url);
	}

	// Bridge struct: passed through ra_http as opaque userdata
	struct ra_http_bridge {
		rc_client_server_callback_t rc_callback;
		void *rc_callback_data;
	};

	ra_http_bridge *bridge = (ra_http_bridge *)malloc(sizeof(ra_http_bridge));
	if (!bridge) {
		RA_LOG("ERROR: malloc failed for HTTP bridge");
		rc_api_server_response_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		resp.body = "malloc failed";
		resp.body_length = strlen(resp.body);
		callback(&resp, callback_data);
		return;
	}
	bridge->rc_callback = callback;
	bridge->rc_callback_data = callback_data;

	// The ra_http callback adapts our ra_http_resp into rc_api_server_response_t
	auto http_done = [](const void *resp_ptr, void *userdata) {
		struct ra_http_resp_view {
			int http_status;
			char *body;
			size_t body_len;
		};
		const ra_http_resp_view *hr = (const ra_http_resp_view *)resp_ptr;
		ra_http_bridge *br = (ra_http_bridge *)userdata;

		rc_api_server_response_t rc_resp;
		memset(&rc_resp, 0, sizeof(rc_resp));
		rc_resp.http_status_code = hr->http_status;
		rc_resp.body = hr->body ? hr->body : "";
		rc_resp.body_length = hr->body_len;

		// Log response body with token masked
		{
			char body_preview[220];
			snprintf(body_preview, sizeof(body_preview), "%.200s", rc_resp.body);
			// Mask "Token":"<value>" in response JSON
			char *tp = strstr(body_preview, "\"Token\":\"");
			if (tp) {
				char *val_start = tp + 9; // skip "Token":"  (9 chars)
				char *val_end = strchr(val_start, '"');
				if (val_end) {
					memmove(val_start + 3, val_end, strlen(val_end) + 1);
					memcpy(val_start, "***", 3);
				}
			}
			RA_LOG("HTTP response: status=%d body_len=%zu body=%s",
				hr->http_status, hr->body_len, body_preview);
		}

		if (hr->http_status == 0) {
			// curl failed entirely — mark as retryable
			rc_resp.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		}

		br->rc_callback(&rc_resp, br->rc_callback_data);
		free(br);
	};

	ra_http_request(request->url, request->post_data, NULL,
		http_done, bridge);
}

static void ra_event_handler(const rc_client_event_t *event, rc_client_t *client)
{
	(void)client;

	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		{
			RA_LOG("*** ACHIEVEMENT TRIGGERED: [%u] %s — %s ***",
				event->achievement->id, event->achievement->title,
				event->achievement->description);
			const int title_max = 28;
			const int desc_max  = 60;
			char title_buf[32];
			char desc_buf[64];
			snprintf(title_buf, title_max + 1, "%s", event->achievement->title);
			if (strlen(event->achievement->title) > (size_t)title_max)
				strcat(title_buf, "...");
			snprintf(desc_buf, desc_max + 1, "%s", event->achievement->description);
			if (strlen(event->achievement->description) > (size_t)desc_max)
				strcat(desc_buf, "...");
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				">> ACHIEVEMENT <<\n\n%s\n%s",
				title_buf, desc_buf);
			ra_notify_urgent(buf, 4000);
			ra_play_achievement_sound();
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		{
			RA_LOG("CHALLENGE SHOW: [%u] %s",
				event->achievement->id, event->achievement->title);
			if (g_show_challenge_show_popup) {
				const int title_max = 28;
				char title_buf[32];
				snprintf(title_buf, title_max + 1, "%s", event->achievement->title);
				if (strlen(event->achievement->title) > (size_t)title_max)
					strcat(title_buf, "...");
				char buf[NOTIF_TEXT_MAX];
				snprintf(buf, sizeof(buf), "CHALLENGE ACTIVE\n\n%s", title_buf);
				ra_notify(buf, 3000);
			}
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		{
			RA_LOG("CHALLENGE HIDE: [%u] %s",
				event->achievement->id, event->achievement->title);
			if (g_show_challenge_hide_popup) {
				const int title_max = 28;
				char title_buf[32];
				snprintf(title_buf, title_max + 1, "%s", event->achievement->title);
				if (strlen(event->achievement->title) > (size_t)title_max)
					strcat(title_buf, "...");
				char buf[NOTIF_TEXT_MAX];
				snprintf(buf, sizeof(buf), "CHALLENGE MISSED\n\n%s", title_buf);
				ra_notify(buf, 3000);
			}
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		{
			RA_LOG("PROGRESS: %s — %s", event->achievement->title, event->achievement->measured_progress);
			if (g_show_progress_popups) {
				char buf[NOTIF_TEXT_MAX];
				if (g_show_progress_name) {
					const int title_max = 28;
					char title_buf[32]; // 28 chars + "..." + null
					snprintf(title_buf, title_max + 1, "%s", event->achievement->title);
					if (strlen(event->achievement->title) > (size_t)title_max)
						strcat(title_buf, "...");
					snprintf(buf, sizeof(buf), "%s\nProgress: %s",
						title_buf, event->achievement->measured_progress);
				} else {
					snprintf(buf, sizeof(buf), "Progress: %s",
						event->achievement->measured_progress);
				}
				ra_notify_progress(buf);
			}
		}
		break;

	case RC_CLIENT_EVENT_GAME_COMPLETED:
		RA_LOG("*** GAME COMPLETED! ***");
		ra_notify_urgent("** GAME COMPLETED! **\n\nCongratulations!", 5000);
		ra_play_achievement_sound();
		break;

	case RC_CLIENT_EVENT_SERVER_ERROR:
		{
			RA_LOG("SERVER ERROR: %s", event->server_error->error_message);
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				"RA Server Error\n\n%.60s",
				event->server_error->error_message);
			ra_notify(buf, 3000);
		}
		break;

	default:
		RA_LOG("EVENT: type=%d", event->type);
		break;
	}
}

static void ra_log_callback(const char *message, const rc_client_t *client)
{
	(void)client;
	RA_LOG("rcheevos: %s", message);
}

// Forward declaration (used in ra_login_callback)
static void ra_load_game_callback(int result, const char *error_message,
	rc_client_t *client, void *userdata);

static void ra_login_callback(int result, const char *error_message,
	rc_client_t *client, void *userdata)
{
	(void)client;
	(void)userdata;

	g_login_pending = 0;

	if (result == RC_OK) {
		const rc_client_user_t *user = rc_client_get_user_info(client);
		RA_LOG("LOGIN OK: %s (score: %u)", user->display_name, user->score);
		g_logged_in = 1;

		{
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				"RetroAchievements\n\n%s\nScore: %u",
				user->display_name, user->score);
			ra_notify(buf, 2500);
		}

		// If a game MD5 is already available, load it now
		if (g_rom_md5[0] && !g_game_loaded && !g_game_load_pending) {
			RA_LOG("Game MD5 available, loading game: %s", g_rom_md5);
			g_game_load_pending = 1;
			rc_client_begin_load_game(g_client, g_rom_md5,
				ra_load_game_callback, NULL);
		}
	} else {
		RA_LOG("LOGIN FAILED: result=%d error=%s", result,
			error_message ? error_message : "(none)");
	}
}

static void ra_load_game_callback(int result, const char *error_message,
	rc_client_t *client, void *userdata)
{
	(void)client;
	(void)userdata;

	g_game_load_pending = 0;

	if (result == RC_OK) {
		const rc_client_game_t *game = rc_client_get_game_info(client);
		RA_LOG("=== GAME IDENTIFIED ===");
		RA_LOG("  ID: %u", game->id);
		RA_LOG("  Title: %s", game->title);
		RA_LOG("  ROM: %s", g_rom_path);
		RA_LOG("  MD5: %s", g_rom_md5);
		g_game_loaded = 1;

		{
			char buf[NOTIF_TEXT_MAX];
			// Count achievements via the list API
			rc_client_achievement_list_t *list =
				rc_client_create_achievement_list(client,
					RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
					RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
			uint32_t total = 0;
			if (list) {
				for (uint32_t b = 0; b < list->num_buckets; b++)
					total += list->buckets[b].num_achievements;
				rc_client_destroy_achievement_list(list);
			}
			if (total > 0) {
				snprintf(buf, sizeof(buf),
					"%s\n\n%u achievements",
					game->title, total);
			} else {
				snprintf(buf, sizeof(buf), "%s", game->title);
			}
			ra_notify(buf, 3000);
		}
	} else {
		RA_LOG("GAME LOAD FAILED: result=%d error=%s", result,
			error_message ? error_message : "(none)");
		if (result == RC_NO_GAME_LOADED) {
			RA_LOG("This ROM is not in the RetroAchievements database.");
		}
	}
}

#endif // HAS_RCHEEVOS

// ---------------------------------------------------------------------------
// Core identification
// ---------------------------------------------------------------------------

// Returns the rcheevos console ID for the current core, or 0 if unsupported.
static int ra_get_console_id(void)
{
	const char *name = user_io_get_core_name(1); // orig name
	if (!name || !name[0]) return 0;

	if (!strcasecmp(name, "NES"))     return 7;  // RC_CONSOLE_NINTENDO
	if (!strcasecmp(name, "SNES"))    return 3;  // RC_CONSOLE_SUPER_NINTENDO
	if (!strcasecmp(name, "Genesis")
		|| !strcasecmp(name, "MegaDrive")) return 1; // RC_CONSOLE_MEGA_DRIVE
	if (!strcasecmp(name, "GBA"))     return 5;  // RC_CONSOLE_GAMEBOY_ADVANCE
	if (!strcasecmp(name, "PSX"))     return 12; // RC_CONSOLE_PLAYSTATION
	if (!strcasecmp(name, "N64"))     return 2;  // RC_CONSOLE_NINTENDO_64
	if (!strcasecmp(name, "TGFX16")) return 8;  // RC_CONSOLE_PC_ENGINE
	if (!strcasecmp(name, "GAMEBOY")) return 4;  // RC_CONSOLE_GAMEBOY
	if (!strcasecmp(name, "SMS"))     return 11; // RC_CONSOLE_MASTER_SYSTEM
	if (!strcasecmp(name, "NEOGEO"))  return 27; // RC_CONSOLE_ARCADE

	RA_LOG("Core '%s' not yet mapped to RA console ID", name);
	return 0;
}

// Returns 1 if the current core is supported for RA
static int ra_core_supported(void)
{
	return ra_get_console_id() != 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void ra_hash_message(const char *msg)
{
	RA_LOG("HASH: %s", msg);
}

void achievements_init(void)
{
	ra_log_open();

	// Install crash handlers to capture backtraces
	signal(SIGSEGV, ra_crash_handler);
	signal(SIGBUS,  ra_crash_handler);
	signal(SIGABRT, ra_crash_handler);
	signal(SIGFPE,  ra_crash_handler);

	RA_LOG("=== RetroAchievements for MiSTer ===");
	RA_LOG("Build: OptionC v21-n64xor3arm (2026-04-14)");
	RA_LOG("Phase 3 — Full pipeline: HTTP + login + achievements");

	const char *core = user_io_get_core_name(1);
	int console_id = ra_get_console_id();
	RA_LOG("Core: '%s' -> console_id=%d supported=%d", core ? core : "(null)",
		console_id, ra_core_supported());

	if (!ra_core_supported()) {
		RA_LOG("Core not supported for RetroAchievements. Inactive.");
		return;
	}

#ifdef HAS_RCHEEVOS
	// Initialize rcheevos hash infrastructure (needed for disc-based consoles)
	rc_hash_init_default_cdreader();
	rc_hash_init_custom_filereader(NULL); // use default stdio-based reader
	rc_hash_init_error_message_callback(ra_hash_message);
	rc_hash_init_verbose_message_callback(ra_hash_message);
#endif

	// N64 savestates occupy 0x3C000000-0x3FFFFFFF (4 slots × 16MB),
	// colliding with the default RA base at 0x3D000000 (= SS slot 1).
	// Move N64 RA mirror to the unused gap at 0x38000000.
	if (console_id == 2) {  // RC_CONSOLE_NINTENDO_64
		ra_ramread_set_base(0x38000000);
		RA_LOG("N64: Using RA DDRAM base 0x38000000 (avoiding SS slot collision)");
	}

	// Map DDRAM mirror region
	g_ra_map = ra_ramread_map();
	if (!g_ra_map) {
		RA_LOG("ERROR: Failed to mmap DDRAM mirror at 0x%08X", ra_ramread_get_base());
		return;
	}
	RA_LOG("DDRAM mirror mapped at 0x%08X (%u bytes)", ra_ramread_get_base(), RA_DDRAM_MAP_SIZE);

	// Initial mirror status check
	if (ra_ramread_active(g_ra_map)) {
		RA_LOG("Mirror already active (magic OK). Dumping state:");
		ra_ramread_debug_dump(g_ra_map);
	} else {
		RA_LOG("Mirror not yet active (FPGA may not have started writing yet).");
	}

	// Start HTTP worker thread
	ra_http_init();

	// Load credentials
	int has_creds = ra_load_credentials();

#ifdef HAS_RCHEEVOS
	// Create rc_client
	g_client = rc_client_create(ra_read_memory, ra_server_call);
	if (!g_client) {
		RA_LOG("ERROR: rc_client_create() failed");
		return;
	}

	rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, ra_log_callback);
	rc_client_set_event_handler(g_client, ra_event_handler);
	rc_client_set_hardcore_enabled(g_client, g_hardcore);
	RA_LOG("Hardcore mode: %s", g_hardcore ? "ENABLED" : "disabled");

	// Configure User-Agent: "MiSTer/1.0 rcheevos/x.y.z" (updated per-core in achievements_load_game)
	{
		rc_client_get_user_agent_clause(g_client, g_ua_clause, sizeof(g_ua_clause));
		char ua[128];
		snprintf(ua, sizeof(ua), "MiSTer/1.0 %s", g_ua_clause);
		ra_http_set_user_agent(ua);
		RA_LOG("User-Agent: %s", ua);
	}

	RA_LOG("rc_client created successfully");

	// Begin login if credentials available
	if (has_creds) {
		RA_LOG("Starting login for user '%s'...", g_ra_user);
		g_login_pending = 1;
		rc_client_begin_login_with_password(g_client, g_ra_user, g_ra_password,
			ra_login_callback, NULL);
	} else {
		RA_LOG("No credentials — running in monitor-only mode.");
		RA_LOG("Create %s to enable RetroAchievements.", RA_CFG_PATH);
	}
#else
	(void)has_creds;
	RA_LOG("Built without rcheevos library (HAS_RCHEEVOS not defined).");
	RA_LOG("Running in diagnostics-only mode: DDRAM mirror + ROM hash.");
#endif
}

void achievements_load_game(const char *rom_path, uint32_t crc32)
{
	if (!ra_core_supported()) return;

	// Update User-Agent with core-specific prefix (e.g. "NES_MiSTer/1.0 rcheevos/11.6")
	if (g_ua_clause[0]) {
		const char *core_name = user_io_get_core_name(1);
		char ua[128];
		if (core_name && core_name[0])
			snprintf(ua, sizeof(ua), "%s_MiSTer/1.0 %s", core_name, g_ua_clause);
		else
			snprintf(ua, sizeof(ua), "MiSTer/1.0 %s", g_ua_clause);
		ra_http_set_user_agent(ua);
		RA_LOG("User-Agent updated: %s", ua);
	}

	RA_LOG("--- Game Load ---");
	RA_LOG("ROM path: %s", rom_path);
	RA_LOG("CRC32: %08X", crc32);

	// Store ROM path
	snprintf(g_rom_path, sizeof(g_rom_path), "%s", rom_path);

	// Calculate hash
	g_rom_md5[0] = '\0';
	if (rom_path && rom_path[0]) {
		int console_id = ra_get_console_id();
#ifdef HAS_RCHEEVOS
		// For disc-based consoles (PSX), byte-order-variable ROMs (N64),
		// or filename-based hashing (Arcade/NeoGeo), use rcheevos hash
		if (console_id == 12 || console_id == 2 || console_id == 27) {
			// Build absolute path — rom_path is relative to getRootDir()
			char abs_path[1024];
			if (rom_path[0] == '/') {
				snprintf(abs_path, sizeof(abs_path), "%s", rom_path);
			} else {
				snprintf(abs_path, sizeof(abs_path), "%s/%s", getRootDir(), rom_path);
			}
			const char *cname = (console_id == 12) ? "PSX" : (console_id == 2) ? "N64" : "NeoGeo";
			RA_LOG("Using rcheevos hash for %s: %s", cname, abs_path);
			if (rc_hash_generate_from_file(g_rom_md5, console_id, abs_path)) {
				RA_LOG("%s hash: %s", cname, g_rom_md5);
			} else {
				RA_LOG("ERROR: rc_hash_generate_from_file failed for %s", cname);
				g_rom_md5[0] = '\0';
			}
		} else
#endif
		{
			ra_calculate_rom_md5(rom_path, g_rom_md5);
		}
	}

	// Reset frame tracking
	g_last_frame = 0;
	g_first_frame = 0;
	g_mirror_validated = 0;
	g_frames_processed = 0;
	g_frames_skipped = 0;
	g_game_loaded = 0;
	g_load_time = time(NULL);
	g_snes_optionc = 0;
	g_snes_collecting = 0;
	g_snes_cache_ready = 0;
	g_snes_last_resp_frame = 0;
	g_snes_game_frames = 0;
	g_snes_poll_logged = 0;
	g_psx_optionc = 0;
	g_psx_collecting = 0;
	g_psx_cache_ready = 0;
	g_psx_needs_recollect = 0;
	g_psx_last_resp_frame = 0;
	g_psx_game_frames = 0;
	g_psx_poll_logged = 0;
	g_md_optionc = 0;
	g_md_collecting = 0;
	g_md_cache_ready = 0;
	g_md_last_resp_frame = 0;
	g_md_game_frames = 0;
	g_md_poll_logged = 0;
	g_n64_optionc = 0;
	g_n64_collecting = 0;
	g_n64_cache_ready = 0;
	g_n64_needs_recollect = 0;
	g_n64_last_resp_frame = 0;
	g_n64_game_frames = 0;
	g_n64_poll_logged = 0;
	g_gb_optionc = 0;
	g_gb_collecting = 0;
	g_gb_cache_ready = 0;
	g_gb_last_resp_frame = 0;
	g_gb_game_frames = 0;
	g_gb_poll_logged = 0;
	g_gba_optionc = 0;
	g_gba_collecting = 0;
	g_gba_cache_ready = 0;
	g_gba_last_resp_frame = 0;
	g_gba_game_frames = 0;
	g_gba_poll_logged = 0;
	g_neogeo_optionc = 0;
	g_neogeo_collecting = 0;
	g_neogeo_cache_ready = 0;
	g_neogeo_last_resp_frame = 0;
	g_neogeo_game_frames = 0;
	g_neogeo_poll_logged = 0;
	ra_snes_addrlist_init();

	// Check mirror state
	if (g_ra_map && ra_ramread_active(g_ra_map)) {
		RA_LOG("Mirror active at game load time. Dumping:");
		ra_ramread_debug_dump(g_ra_map);
	}

#ifdef HAS_RCHEEVOS
	if (g_client && g_rom_md5[0]) {
		if (g_logged_in && !g_game_load_pending) {
			// Already logged in — load game immediately
			RA_LOG("Logged in, loading game by MD5: %s", g_rom_md5);
			g_game_load_pending = 1;
			rc_client_begin_load_game(g_client, g_rom_md5,
				ra_load_game_callback, NULL);
		} else if (g_login_pending) {
			// Login still in progress — game will be loaded in login callback
			RA_LOG("Login in progress, game will load when login completes.");
		} else {
			RA_LOG("Not logged in — game identified but achievements unavailable.");
			RA_LOG("MD5: %s (can verify at retroachievements.org)", g_rom_md5);
		}
	}
#endif

	RA_LOG("--- Game Load Complete, monitoring frames ---");

	// Hardcore mode: force FPGA status bits to disable cheats & save states
	if (g_hardcore) {
		int cid = ra_get_console_id();
		switch (cid) {
		case 7:  // NES
			user_io_status_set("[70]", 1);
			user_io_status_set("[20]", 1);
			break;
		case 3:  // SNES
			user_io_status_set("[58]", 1);
			user_io_status_set("[24]", 1);
			break;
		case 1:  // MegaDrive / Genesis
			user_io_status_set("[64]", 1);
			user_io_status_set("[24]", 1);
			break;
		case 12: // PSX
			user_io_status_set("[93]", 1);
			user_io_status_set("[6]", 1);
			break;
		case 11: // SMS / Game Gear — cheats bit [24], no save states to disable
			user_io_status_set("[24]", 1);
			break;
		default:
			RA_LOG("Hardcore: no FPGA bit mapping for console %d", cid);
			break;
		}
		RA_LOG("Hardcore: forced FPGA status bits (cheats off, savestates off)");
	}
}

void achievements_poll(void)
{
	static uint32_t poll_calls = 0;
	poll_calls++;

	// Heartbeat: log every ~10 seconds to confirm poll is alive
	{
		static struct timespec hb_last = {0, 0};
		struct timespec hb_now;
		clock_gettime(CLOCK_MONOTONIC, &hb_now);
		if (hb_last.tv_sec == 0) hb_last = hb_now;
		double hb_elapsed = (hb_now.tv_sec - hb_last.tv_sec)
			+ (hb_now.tv_nsec - hb_last.tv_nsec) / 1e9;
		if (hb_elapsed >= 10.0) {
			hb_last = hb_now;
			RA_LOG("HEARTBEAT: poll=%u map=%p validated=%d game_loaded=%d",
				poll_calls, g_ra_map, g_mirror_validated, g_game_loaded);
		}
	}

	// Always pump HTTP responses (even if mirror not validated yet,
	// because login/game-load responses need to be processed)
	ra_http_poll();

	// Show queued OSD notifications (achievement popups, etc.)
	ra_osd_poll();

	if (!g_ra_map) return;

	// Check if mirror has become active
	if (!g_mirror_validated) {
		if (ra_ramread_active(g_ra_map)) {
			g_mirror_validated = 1;
			RA_LOG("=== DDRAM Mirror Activated! ===");
			ra_ramread_debug_dump(g_ra_map);

			// Detect FPGA protocol: region_count==0 means Option C, non-zero means VBlank-gated
			const ra_header_t *hdr = (const ra_header_t *)g_ra_map;
			if (ra_get_console_id() == 3) {
				if (hdr->region_count == 0) {
					g_snes_optionc = 1;
					RA_LOG("SNES FPGA protocol: Option C (selective address reading)");
				} else {
					g_snes_optionc = 0;
					RA_LOG("SNES FPGA protocol: VBlank-gated full mirror (region_count=%d)", hdr->region_count);
				}
			}
			if (ra_get_console_id() == 12) {
				g_psx_optionc = 1;
				RA_LOG("PSX FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 1) {
				g_md_optionc = 1;
				RA_LOG("MegaDrive FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 2) {
				g_n64_optionc = 1;
				RA_LOG("N64 FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 4) {
				g_gb_optionc = 1;
				RA_LOG("Gameboy FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 5) {
				g_gba_optionc = 1;
				RA_LOG("GBA FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 11) {
				g_sms_optionc = 1;
				RA_LOG("SMS FPGA protocol: Option C (selective address reading)");
			}
			if (ra_get_console_id() == 27) {
				g_neogeo_optionc = 1;
				RA_LOG("NeoGeo FPGA protocol: Option C (selective address reading)");
			}
		} else {
			// Periodic debug while waiting for FPGA mirror
			static uint32_t wait_count = 0;
			if ((++wait_count % 18000) == 1) {
				const uint8_t *p = (const uint8_t *)g_ra_map;
				RA_LOG("Waiting for mirror... raw header: "
					"%02X %02X %02X %02X  %02X %02X %02X %02X  "
					"%02X %02X %02X %02X  %02X %02X %02X %02X",
					p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
					p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
			}
		}
		return;
	}

	// Read frame counter
	uint32_t frame = ra_ramread_frame(g_ra_map);
	int busy = ra_ramread_busy(g_ra_map);

	// Periodic diagnostics every ~5s (assuming poll is called frequently)
	if ((poll_calls % 18000) == 1) {
		int console = ra_get_console_id();
		int optionc = (console == 4) ? g_gb_optionc : (console == 5) ? g_gba_optionc : (console == 11) ? g_sms_optionc : (console == 2) ? g_n64_optionc : (console == 12) ? g_psx_optionc : (console == 1) ? g_md_optionc : (console == 27) ? g_neogeo_optionc : g_snes_optionc;
		RA_LOG("DIAG: poll_calls=%u frame=%u last_frame=%u busy=%d processed=%u skipped=%u game_loaded=%d console=%d optionc=%d",
			poll_calls, frame, g_last_frame, busy, g_frames_processed, g_frames_skipped, g_game_loaded,
			console, optionc);

		if (console == 3 && g_snes_optionc) {
			// SNES Option C: show address cache status
			RA_LOG("SNES OptionC: addrs=%d cache=%s resp_frame=%u",
				ra_snes_addrlist_count(),
				g_snes_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map));
		} else if (console == 3) {
			// SNES VBlank-gated: show WRAM data
			const uint8_t *base = (const uint8_t *)g_ra_map;
			const uint8_t *wram = base + RA_SNES_WRAM_OFFSET;
			char hex[200];
			int pos = 0;
			for (int i = 0; i < 32 && pos < (int)sizeof(hex) - 4; i++)
				pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", wram[i]);
			RA_LOG("WRAM[0..31]: %s", hex);
			const ra_header_t *hdr = (const ra_header_t *)base;
			RA_LOG("BSRAM size=%u", hdr->reserved2);
		} else if (console == 12 && g_psx_optionc) {
			// PSX Option C: show address cache status
			RA_LOG("PSX OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_psx_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_psx_game_frames);
		} else if (console == 2 && g_n64_optionc) {
			// N64 Option C: show address cache status
			RA_LOG("N64 OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_n64_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_n64_game_frames);
			if (g_ra_map) {
				const uint8_t *base = (const uint8_t *)g_ra_map;
				// Header: magic + flags
				const uint32_t *hdr32 = (const uint32_t *)base;
				uint32_t magic = hdr32[0];
				uint8_t flags = base[5];
				uint32_t frame = hdr32[2]; // frame_counter at offset 0x08
				// Debug word 1 at offset 0x10:
				// {ver(8), state_snap(8), vblank_cnt(16), timeout_cnt(16), ok_cnt(16)}
				const uint64_t *dbg1 = (const uint64_t *)(base + 0x10);
				uint64_t d1 = *dbg1;
				uint8_t  fpga_ver    = (d1 >> 56) & 0xFF;
				uint8_t  state_snap  = (d1 >> 48) & 0xFF;
				uint16_t vbl_hb      = (d1 >> 32) & 0xFFFF;
				uint16_t timeout_cnt = (d1 >> 16) & 0xFFFF;
				uint16_t ok_cnt      = d1 & 0xFFFF;
				// Debug word 2 at offset 0x18:
				// {first_addr(16), warmup_left(8), 0(8), dispatch_cnt(16), max_timeout(16)}
				const uint64_t *dbg2 = (const uint64_t *)(base + 0x18);
				uint64_t d2 = *dbg2;
				uint16_t first_addr  = (d2 >> 48) & 0xFFFF;
				uint8_t  warmup_left = (d2 >> 40) & 0x0F;
				uint16_t disp_cnt    = (d2 >> 16) & 0xFFFF;
				uint16_t max_timeout = d2 & 0xFFFF;
				RA_LOG("N64 FPGA: magic=%08X flags=%02X frame=%u ver=0x%02X state=%u vbl_hb=%u ok=%u disp=%u timeout=%u/%u warmup=%u first=0x%04X",
					magic, flags, frame, fpga_ver, state_snap, vbl_hb,
					ok_cnt, disp_cnt, timeout_cnt, max_timeout, warmup_left, first_addr);
				if (fpga_ver != 0x03) {
					RA_LOG("N64 FPGA: *** VERSION MISMATCH! Expected 0x03, got 0x%02X — old bitstream? ***", fpga_ver);
				}
				// VALCACHE response header
				const uint32_t *resp32 = (const uint32_t *)(base + 0x48000);
				RA_LOG("N64 VALCACHE: resp_id=%u resp_frame=%u (expect req_id=%u)",
					resp32[0], resp32[1], ra_snes_addrlist_request_id());
			}
		} else if (console == 1 && g_md_optionc) {
			// MegaDrive Option C: show address cache status
			RA_LOG("MD OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_md_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_md_game_frames);
			if (g_md_cache_ready && g_ra_map) {
				const uint8_t *vals = (const uint8_t *)g_ra_map + 0x48000 + 8;
				int cnt = ra_snes_addrlist_count();
				int dump_len = cnt < 45 ? cnt : 45;
				int non_zero = 0;
				char hex[256];
				int pos = 0;
				for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
					if (vals[i]) non_zero++;
				}
				RA_LOG("VALCACHE[0..%d]: %s (%d non-zero)", dump_len - 1, hex, non_zero);
				// v0x04 debug layout:
				// Word 1 (0x10): {ver(8), dispatch(8), ok(16), oob(16), 0(16)}
				// Words 2-5 (0x18-0x30): 8 debug records at 32 bits each, 2 per word
				//   Each rec: {bram_addr[14:0](15), cur_addr_bit0(1), bram_dout(16)}
				// Word 6 (0x38): sweep test {0x5B00(16), sweep_dout(16), bram_dout_now(16), 0(16)}
				const uint8_t *base = (const uint8_t *)g_ra_map;
				const uint64_t *dbg1 = (const uint64_t *)(base + 0x10);
				uint64_t d1 = *dbg1;
				uint8_t  fpga_ver     = (d1 >> 56) & 0xFF;
				uint8_t  dispatch_cnt = (d1 >> 48) & 0xFF;
				uint16_t ok_cnt       = (d1 >> 32) & 0xFFFF;
				uint16_t oob_cnt      = (d1 >> 16) & 0xFFFF;
				RA_LOG("FPGA_DBG: ver=0x%02X disp=%u ok=%u oob=%u",
					fpga_ver, dispatch_cnt, ok_cnt, oob_cnt);

				// Read 8 per-address debug records (first 8 addresses)
				// Addresses in sorted order: D003 D008 D009 D01B D023 D5C1 F601 F602
				for (int w = 0; w < 4; w++) {
					const uint64_t *dw = (const uint64_t *)(base + 0x18 + w * 8);
					uint64_t word = *dw;
					uint32_t rec_lo = word & 0xFFFFFFFF;
					uint32_t rec_hi = (word >> 32) & 0xFFFFFFFF;
					int idx0 = w * 2, idx1 = w * 2 + 1;
					uint16_t baddr0 = (rec_lo >> 17) & 0x7FFF;
					uint8_t  bit0_0 = (rec_lo >> 16) & 1;
					uint16_t bdout0 = rec_lo & 0xFFFF;
					uint16_t baddr1 = (rec_hi >> 17) & 0x7FFF;
					uint8_t  bit0_1 = (rec_hi >> 16) & 1;
					uint16_t bdout1 = rec_hi & 0xFFFF;
					RA_LOG("  REC[%d]: waddr=0x%04X a0=%u dout=0x%04X | REC[%d]: waddr=0x%04X a0=%u dout=0x%04X",
						idx0, baddr0, bit0_0, bdout0, idx1, baddr1, bit0_1, bdout1);
				}

				// Sweep test result: {sweep1_F601(16), sweep2_FF84(16), addr1(16), addr2(16)}
				const uint64_t *dsweep = (const uint64_t *)(base + 0x38);
				uint64_t sw = *dsweep;
				uint16_t sw_val1 = (sw >> 48) & 0xFFFF;  // bram_dout at 0x5B00 (F601)
				uint16_t sw_val2 = (sw >> 32) & 0xFFFF;  // bram_dout at 0x5F02 (FF84)
				uint16_t sw_a1   = (sw >> 16) & 0xFFFF;   // should be 0x5B00
				uint16_t sw_a2   = sw & 0xFFFF;            // should be 0x5F02
				RA_LOG("SWEEP1(F601): bram=0x%04X dout=0x%04X | SWEEP2(FF84): bram=0x%04X dout=0x%04X",
					sw_a1, sw_val1, sw_a2, sw_val2);

				// Dump first 20 addresses from the sorted address list
				const uint32_t *addrs = ra_snes_addrlist_addrs();
				int acnt = ra_snes_addrlist_count();
				int adump = acnt < 20 ? acnt : 20;
				char ahex[256];
				int apos = 0;
				for (int i = 0; i < adump && apos < (int)sizeof(ahex) - 8; i++)
					apos += snprintf(ahex + apos, sizeof(ahex) - apos, "%04X ", addrs[i]);
				RA_LOG("ADDRLIST[0..%d]: %s (total=%d)", adump - 1, ahex, acnt);

				// Direct lookup of key Sonic 1 / MD addresses
				uint8_t gs   = ra_snes_addrlist_read_cached(g_ra_map, 0xF601);
				uint8_t act  = ra_snes_addrlist_read_cached(g_ra_map, 0xFE10);
				uint8_t zone = ra_snes_addrlist_read_cached(g_ra_map, 0xFE11);
				uint8_t lives= ra_snes_addrlist_read_cached(g_ra_map, 0xFE13);
				uint8_t rings_h = ra_snes_addrlist_read_cached(g_ra_map, 0xFE20);
				uint8_t rings_l = ra_snes_addrlist_read_cached(g_ra_map, 0xFE21);
				uint8_t tmin = ra_snes_addrlist_read_cached(g_ra_map, 0xFE22);
				uint8_t tsec = ra_snes_addrlist_read_cached(g_ra_map, 0xFE25);
				uint8_t demo = ra_snes_addrlist_read_cached(g_ra_map, 0xFFF0);
				uint8_t dbgm = ra_snes_addrlist_read_cached(g_ra_map, 0xFFFA);
				RA_LOG("KEY: F601(state)=%02X FE10(act)=%02X FE11(zone)=%02X FE13(lives)=%02X FE20-21(rings)=%02X%02X FE22(tmin)=%02X FE25(tsec)=%02X FFF0(demo)=%02X FFFA(dbg)=%02X",
					gs, act, zone, lives, rings_h, rings_l, tmin, tsec, demo, dbgm);
			}
		} else if (console == 4 && g_gb_optionc) {
			// GB Option C: show address cache status and value dump
			RA_LOG("GB OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_gb_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_gb_game_frames);
			if (g_gb_cache_ready && g_ra_map) {
				// Dump all cached values
				const uint8_t *vals = (const uint8_t *)g_ra_map + 0x48000 + 8;
				int cnt = ra_snes_addrlist_count();
				int dump_len = cnt < 32 ? cnt : 32;
				int non_zero = 0;
				char hex[256];
				int pos = 0;
				for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
					if (vals[i]) non_zero++;
				}
				RA_LOG("VALCACHE[0..%d]: %s (%d non-zero)", dump_len - 1, hex, non_zero);

				// Dump sorted address list
				const uint32_t *addrs = ra_snes_addrlist_addrs();
				char ahex[256];
				int apos = 0;
				for (int i = 0; i < dump_len && apos < (int)sizeof(ahex) - 8; i++)
					apos += snprintf(ahex + apos, sizeof(ahex) - apos, "%04X ", addrs[i]);
				RA_LOG("ADDRLIST[0..%d]: %s (total=%d)", dump_len - 1, ahex, cnt);

				// Key SML addresses from code notes
				uint8_t gstate = ra_snes_addrlist_read_cached(g_ra_map, 0xFFB3);
				uint8_t gmode  = ra_snes_addrlist_read_cached(g_ra_map, 0xFFD8);
				uint8_t screen = ra_snes_addrlist_read_cached(g_ra_map, 0xFFF4);
				uint8_t conts  = ra_snes_addrlist_read_cached(g_ra_map, 0xC0A6);
				uint8_t event  = ra_snes_addrlist_read_cached(g_ra_map, 0xDFE1);
				RA_LOG("SML KEY: FFB3(state)=%02X FFD8(mode)=%02X FFF4(screen)=%02X C0A6(conts)=%02X DFE1(event)=%02X",
					gstate, gmode, screen, conts, event);

				// FPGA debug words
				const uint8_t *base = (const uint8_t *)g_ra_map;
				const uint64_t *dbg1 = (const uint64_t *)(base + 0x10);
				uint64_t d1 = *dbg1;
				uint8_t  fpga_ver     = (d1 >> 56) & 0xFF;
				uint16_t timeout_cnt  = (d1 >> 16) & 0xFFFF;
				uint16_t ok_cnt       = d1 & 0xFFFF;
				const uint64_t *dbg2 = (const uint64_t *)(base + 0x18);
				uint64_t d2 = *dbg2;
				uint16_t wram_cnt   = (d2 >> 32) & 0xFFFF;
				uint16_t cram_cnt   = (d2 >> 16) & 0xFFFF;
				uint16_t hram_cnt   = d2 & 0xFFFF;
				RA_LOG("GB FPGA: ver=0x%02X ok=%u timeout=%u wram=%u cram=%u hram=%u",
					fpga_ver, ok_cnt, timeout_cnt, wram_cnt, cram_cnt, hram_cnt);
			}
		} else if (console == 11 && g_sms_optionc) {
			// SMS/GG Option C: show address cache status
			RA_LOG("SMS OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_sms_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_sms_game_frames);
			if (g_sms_cache_ready && g_ra_map) {
				const uint8_t *vals = (const uint8_t *)g_ra_map + 0x48000 + 8;
				int cnt = ra_snes_addrlist_count();
				int dump_len = cnt < 45 ? cnt : 45;
				int non_zero = 0;
				char hex[256];
				int pos = 0;
				for (int i = 0; i < dump_len && pos < (int)sizeof(hex) - 4; i++) {
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", vals[i]);
					if (vals[i]) non_zero++;
				}
				RA_LOG("VALCACHE[0..%d]: %s (%d non-zero)", dump_len - 1, hex, non_zero);
			}
		} else if (console == 5 && g_gba_optionc) {
			// GBA Option C: show address cache status
			RA_LOG("GBA OptionC: addrs=%d cache=%s resp_frame=%u game_frames=%u",
				ra_snes_addrlist_count(),
				g_gba_cache_ready ? "active" : "waiting",
				ra_snes_addrlist_response_frame(g_ra_map),
				g_gba_game_frames);
		} else {
			// NES: region-based layout
			const uint8_t *cpuram = ra_ramread_region_data(g_ra_map, 0);
			uint16_t cpuram_sz = ra_ramread_region_size(g_ra_map, 0);
			if (cpuram && cpuram_sz > 0) {
				int n = cpuram_sz < 32 ? cpuram_sz : 32;
				char hex[200];
				int pos = 0;
				for (int i = 0; i < n && pos < (int)sizeof(hex) - 4; i++)
					pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", cpuram[i]);
				RA_LOG("RAM[0..%d]: %s", n - 1, hex);
			} else {
				RA_LOG("RAM: region 0 not available (ptr=%p sz=%u)", cpuram, cpuram_sz);
			}
		}
	}

	(void)busy;

#ifdef HAS_RCHEEVOS
	// ================================================================
	// SNES: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 3 && g_snes_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_snes_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_snes_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_snes_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("SNES OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
				snes_optionc_dump_valcache("bootstrap");
			} else {
				RA_LOG("SNES OptionC: No addresses collected — achievements may have no memory refs");
			}
		} else if (!g_snes_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_snes_cache_ready = 1;
				g_snes_last_resp_frame = 0;
				g_snes_game_frames = 0;
				g_snes_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_snes_cache_time);
				RA_LOG("SNES OptionC: Cache active! FPGA response matched request.");
				snes_optionc_dump_valcache("cache-active");
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_snes_last_resp_frame) {
				g_snes_last_resp_frame = resp_frame;
				g_frames_processed++;
				g_snes_game_frames++;

				// Dump first 5 frames after cache became active
				if (g_snes_game_frames <= 5) {
					RA_LOG("SNES OptionC: GameFrame %u (resp_frame=%u)",
						g_snes_game_frames, resp_frame);
					snes_optionc_dump_valcache("early-frame");
				}

				// Periodically re-collect to catch address changes (every ~5 min)
				int re_collect = (g_snes_game_frames % 18000 == 0) && (g_snes_game_frames > 0);
				if (re_collect) {
					g_snes_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_snes_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("SNES OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		// Periodic SNES debug — log once per 300-frame milestone
		uint32_t milestone = g_snes_game_frames / 300;
		if (milestone > 0 && milestone != g_snes_poll_logged) {
			g_snes_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_snes_cache_time.tv_sec)
				+ (now.tv_nsec - g_snes_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_snes_game_frames > 0) ?
				(elapsed * 1000.0 / g_snes_game_frames) : 0.0;
			RA_LOG("POLL(SNES): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_snes_last_resp_frame, g_snes_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
			// Also dump VALCACHE every 1800 game frames (~30s)
			if ((g_snes_game_frames % 1800) < 300) {
				snes_optionc_dump_valcache("periodic");
			}
		}
		return; // SNES handled — skip default frame tracking
	}

	// ================================================================
	// PSX: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 12 && g_psx_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_psx_cache_ready) {
			// Phase 1: Bootstrap — collect addresses with zero values
			g_psx_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_psx_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				g_psx_needs_recollect = 1; // schedule pointer-resolution pass
				RA_LOG("PSX OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
				psx_optionc_dump_valcache("bootstrap");
			} else {
				RA_LOG("PSX OptionC: No addresses collected");
			}
		} else if (!g_psx_cache_ready) {
			// Phase 2/4: Wait for FPGA cache
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_psx_cache_ready = 1;
				g_psx_last_resp_frame = 0;
				g_psx_game_frames = 0;
				g_psx_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_psx_cache_time);
				if (g_psx_needs_recollect) {
					RA_LOG("PSX OptionC: Cache active (pre-recollect). Will resolve pointers.");
					psx_optionc_dump_valcache("pre-recollect");
				} else {
					RA_LOG("PSX OptionC: Cache active! FPGA response matched request.");
					psx_optionc_dump_valcache("cache-active");
				}
			}
		} else if (g_psx_needs_recollect) {
			// Phase 3: Pointer-resolution re-collection
			// Now that cache has real values, AddAddress conditions will
			// compute correct derived addresses from pointer base values.
			g_psx_needs_recollect = 0;
			g_psx_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_psx_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("PSX OptionC: Pointer-resolve re-collection done, %d addrs (was different!)",
					ra_snes_addrlist_count());
				g_psx_cache_ready = 0; // wait for new FPGA data with correct addresses
				psx_optionc_dump_valcache("ptr-resolve");
			} else {
				RA_LOG("PSX OptionC: Pointer-resolve complete, no address changes");
			}
		} else {
			// Phase 5: Normal frame processing
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_psx_last_resp_frame) {
				g_psx_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_psx_game_frames++;

				if (g_psx_game_frames <= 5) {
					RA_LOG("PSX OptionC: GameFrame %u (resp_frame=%u)",
						g_psx_game_frames, resp_frame);
					psx_optionc_dump_valcache("early-frame");
				}

				// Re-collect every 600 frames (~10s) to track pointer changes
				int re_collect = (g_psx_game_frames % 600 == 0) && (g_psx_game_frames > 0);
				if (re_collect) {
					g_psx_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_psx_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("PSX OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
						g_psx_cache_ready = 0; // wait for new FPGA data
					}
				}
			}
		}

		uint32_t milestone = g_psx_game_frames / 300;
		if (milestone > 0 && milestone != g_psx_poll_logged) {
			g_psx_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_psx_cache_time.tv_sec)
				+ (now.tv_nsec - g_psx_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_psx_game_frames > 0) ?
				(elapsed * 1000.0 / g_psx_game_frames) : 0.0;
			RA_LOG("POLL(PSX): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_psx_last_resp_frame, g_psx_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
			if ((g_psx_game_frames % 1800) < 300) {
				psx_optionc_dump_valcache("periodic");
			}
		}
		return; // PSX handled
	}

	// ================================================================
	// N64: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 2 && g_n64_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_n64_cache_ready) {
			// Phase 1: Bootstrap — collect addresses with zero values
			g_n64_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_n64_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				g_n64_needs_recollect = 1; // schedule pointer-resolution pass
				RA_LOG("N64 OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("N64 OptionC: No addresses collected");
			}
		} else if (!g_n64_cache_ready) {
			// Phase 2/4: Wait for FPGA cache
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_n64_cache_ready = 1;
				g_n64_last_resp_frame = 0;
				g_n64_game_frames = 0;
				g_n64_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_n64_cache_time);
				if (g_n64_needs_recollect) {
					RA_LOG("N64 OptionC: Cache active (pre-recollect). Will resolve pointers.");
				} else {
					RA_LOG("N64 OptionC: Cache active! FPGA response matched request.");
				}
			}
		} else if (g_n64_needs_recollect) {
			// Phase 3: Pointer-resolution re-collection
			g_n64_needs_recollect = 0;
			g_n64_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_n64_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("N64 OptionC: Pointer-resolve re-collection done, %d addrs (was different!)",
					ra_snes_addrlist_count());
				g_n64_cache_ready = 0; // wait for new FPGA data with correct addresses
			} else {
				RA_LOG("N64 OptionC: Pointer-resolve complete, no address changes");
			}
		} else {
			// Phase 5: Normal frame processing
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_n64_last_resp_frame) {
				g_n64_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_n64_game_frames++;

				if (g_n64_game_frames <= 5) {
					RA_LOG("N64 OptionC: GameFrame %u (resp_frame=%u)",
						g_n64_game_frames, resp_frame);
				}

				// Log every 100 frames to track progress (temporary)
				if ((g_n64_game_frames % 100) == 0) {
					RA_LOG("N64 OptionC: frame tick %u (resp=%u)",
						g_n64_game_frames, resp_frame);
				}

				// Value diagnostic at frame 30 and 300: compare FPGA cached values
				// with direct ARM reads of RDRAM to verify byte ordering.
				if (g_n64_game_frames == 30 || g_n64_game_frames == 300) {
					void *rdram_direct = shmem_map(0x30000000, 0x800000);
					if (rdram_direct) {
						const uint8_t *rdram = (const uint8_t *)rdram_direct;
						const uint8_t *vals = (const uint8_t *)g_ra_map + RA_SNES_VALCACHE_OFFSET + 8;
						int addr_count = ra_snes_addrlist_count();
						const uint32_t *addrs = ra_snes_addrlist_addrs();
						int show = addr_count < 30 ? addr_count : 30;
						RA_LOG("=== N64 RDRAM Value Diagnostic (frame %u, %d addrs) ===",
							g_n64_game_frames, addr_count);
						int match_direct = 0, match_xor3 = 0, match_none = 0;
						for (int i = 0; i < show; i++) {
							uint32_t a = addrs[i];
							uint8_t cached = vals[i];
							uint8_t direct = (a < 0x800000) ? rdram[a] : 0;
							uint8_t xor3   = (a < 0x800000) ? rdram[a ^ 3] : 0;
							const char *tag;
							if (cached == direct && cached == xor3) tag = "BOTH";
							else if (cached == direct) { tag = "DIRECT"; match_direct++; }
							else if (cached == xor3) { tag = "XOR3"; match_xor3++; }
							else { tag = "DIFF"; match_none++; }
							RA_LOG("  [%3d] addr=0x%06X cached=0x%02X direct=0x%02X xor3=0x%02X %s",
								i, a, cached, direct, xor3, tag);
						}
						RA_LOG("  SUMMARY: direct=%d xor3=%d diff=%d (of %d shown)",
							match_direct, match_xor3, match_none, show);

						// SM64-specific known addresses (from code notes)
						const struct { uint32_t addr; const char *name; } sm64[] = {
							{0x0033b24a, "MapID"},
							{0x0033b21d, "HP"},
							{0x0033b21e, "Lives"},
							{0x0033b21a, "Coins"},
							{0x0033b218, "Stars"},
							{0x0033b17c, "Animation"},
							{0x0032ddfa, "MapID2"},
							{0x0032ddf6, "CurFile"},
						};
						RA_LOG("  --- SM64 Known Addresses ---");
						for (int j = 0; j < 8; j++) {
							uint32_t a = sm64[j].addr;
							if (a < 0x800000) {
								uint8_t d = rdram[a];
								uint8_t x = rdram[a ^ 3];
								// Check if this address is in the monitored list
								uint8_t c = 0;
								int found = 0;
								for (int k = 0; k < addr_count; k++) {
									if (addrs[k] == a) { c = vals[k]; found = 1; break; }
								}
								RA_LOG("  SM64 %s(0x%06X): direct=0x%02X xor3=0x%02X cached=%s0x%02X",
									sm64[j].name, a, d, x, found ? "" : "N/A:", c);
							}
						}
						shmem_unmap(rdram_direct, 0x800000);
					} else {
						RA_LOG("N64 VALDIAG: failed to mmap RDRAM at 0x30000000");
					}
				}

				// Re-collect every 600 frames (~10s) to track pointer changes
				int re_collect = (g_n64_game_frames % 600 == 0) && (g_n64_game_frames > 0);
				if (re_collect) {
					RA_LOG("N64 OptionC: Re-collect starting at game_frame=%u",
						g_n64_game_frames);
					g_n64_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_n64_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("N64 OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
						g_n64_cache_ready = 0; // wait for new FPGA data
					} else {
						RA_LOG("N64 OptionC: Re-collect done, no address changes");
					}
				}
			}
		}

		uint32_t milestone = g_n64_game_frames / 300;
		if (milestone > 0 && milestone != g_n64_poll_logged) {
			g_n64_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_n64_cache_time.tv_sec)
				+ (now.tv_nsec - g_n64_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_n64_game_frames > 0) ?
				(elapsed * 1000.0 / g_n64_game_frames) : 0.0;
			RA_LOG("POLL(N64): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_n64_last_resp_frame, g_n64_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return; // N64 handled
	}

	// ================================================================
	// MegaDrive/Genesis: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 1 && g_md_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_md_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_md_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_md_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("MD OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("MD OptionC: No addresses collected");
			}
		} else if (!g_md_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_md_cache_ready = 1;
				g_md_last_resp_frame = 0;
				g_md_game_frames = 0;
				g_md_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_md_cache_time);
				RA_LOG("MD OptionC: Cache active! FPGA response matched request.");
				// Dump address list once on activation
				const uint32_t *a0 = ra_snes_addrlist_addrs();
				int ac = ra_snes_addrlist_count();
				int ad = ac < 20 ? ac : 20;
				char ah[256]; int ap = 0;
				for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
					ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
				RA_LOG("ADDRLIST[0..%d]: %s (total=%d)", ad - 1, ah, ac);
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_md_last_resp_frame) {
				g_md_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_md_game_frames++;

				if (g_md_game_frames <= 5) {
					const uint8_t *ev = (const uint8_t *)g_ra_map + 0x48000 + 8;
					int ec = ra_snes_addrlist_count();
					int ed = ec < 45 ? ec : 45;
					int enz = 0;
					char eh[256]; int ep = 0;
					for (int i = 0; i < ed && ep < (int)sizeof(eh) - 4; i++) {
						ep += snprintf(eh + ep, sizeof(eh) - ep, "%02X ", ev[i]);
						if (ev[i]) enz++;
					}
					RA_LOG("MD early[%u]: %s (%d nz)", g_md_game_frames, eh, enz);
					uint8_t egs = ra_snes_addrlist_read_cached(g_ra_map, 0xF601);
					uint8_t eact = ra_snes_addrlist_read_cached(g_ra_map, 0xFE10);
					uint8_t ezone = ra_snes_addrlist_read_cached(g_ra_map, 0xFE11);
					uint8_t elives = ra_snes_addrlist_read_cached(g_ra_map, 0xFE13);
					uint8_t edemo = ra_snes_addrlist_read_cached(g_ra_map, 0xFFF0);
					RA_LOG("MD early KEY: state=%02X act=%02X zone=%02X lives=%02X demo=%02X",
						egs, eact, ezone, elives, edemo);
				}

				// Re-collect every ~5 min to catch address changes
				int re_collect = (g_md_game_frames % 18000 == 0) && (g_md_game_frames > 0);
				if (re_collect) {
					g_md_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_md_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("MD OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		uint32_t milestone = g_md_game_frames / 300;
		if (milestone > 0 && milestone != g_md_poll_logged) {
			g_md_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_md_cache_time.tv_sec)
				+ (now.tv_nsec - g_md_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_md_game_frames > 0) ?
				(elapsed * 1000.0 / g_md_game_frames) : 0.0;
			RA_LOG("POLL(MD): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_md_last_resp_frame, g_md_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
			// Key address snapshot at each milestone
			if (g_md_cache_ready && g_ra_map) {
				uint8_t gs   = ra_snes_addrlist_read_cached(g_ra_map, 0xF601);
				uint8_t act  = ra_snes_addrlist_read_cached(g_ra_map, 0xFE10);
				uint8_t zone = ra_snes_addrlist_read_cached(g_ra_map, 0xFE11);
				uint8_t lives= ra_snes_addrlist_read_cached(g_ra_map, 0xFE13);
				uint8_t demo = ra_snes_addrlist_read_cached(g_ra_map, 0xFFF0);
				uint8_t ffe0 = ra_snes_addrlist_read_cached(g_ra_map, 0xFFE0);
				uint8_t ffe3 = ra_snes_addrlist_read_cached(g_ra_map, 0xFFE3);
				uint8_t fff9 = ra_snes_addrlist_read_cached(g_ra_map, 0xFFF9);
				RA_LOG("KEY: state=%02X act=%02X zone=%02X lives=%02X demo=%02X FFE0=%02X FFE3=%02X FFF9=%02X",
					gs, act, zone, lives, demo, ffe0, ffe3, fff9);
			}
		}
		return; // MegaDrive handled
	}

	// ================================================================
	// Gameboy/Gameboy Color: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 4 && g_gb_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_gb_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_gb_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_gb_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("GB OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("GB OptionC: No addresses collected — achievements may have no memory refs");
			}
		} else if (!g_gb_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_gb_cache_ready = 1;
				g_gb_last_resp_frame = 0;
				g_gb_game_frames = 0;
				g_gb_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_gb_cache_time);
				RA_LOG("GB OptionC: Cache active! FPGA response matched request.");
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_gb_last_resp_frame) {
				g_gb_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;  // keep DIAG display in sync
				g_frames_processed++;
				g_gb_game_frames++;

				// Periodically re-collect to catch address changes (every ~5 min)
				int re_collect = (g_gb_game_frames % 18000 == 0) && (g_gb_game_frames > 0);
				if (re_collect) {
					g_gb_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				// Value-change watchers: log every transition of key addresses
				{
					static uint8_t prev_d190 = 0, prev_ff99 = 0, prev_fffa = 0;
					static uint8_t prev_ffb3 = 0, prev_ffb4 = 0, prev_ffda = 0;
					static uint16_t prev_score = 0;
					static int watch_init = 0;

					uint8_t cur_d190 = ra_snes_addrlist_read_cached(g_ra_map, 0xD190);
					uint8_t cur_ff99 = ra_snes_addrlist_read_cached(g_ra_map, 0xFF99);
					uint8_t cur_fffa = ra_snes_addrlist_read_cached(g_ra_map, 0xFFFA);
					uint8_t cur_ffb3 = ra_snes_addrlist_read_cached(g_ra_map, 0xFFB3);
					uint8_t cur_ffb4 = ra_snes_addrlist_read_cached(g_ra_map, 0xFFB4);
					uint8_t cur_ffda = ra_snes_addrlist_read_cached(g_ra_map, 0xFFDA);
					uint16_t cur_score = ra_snes_addrlist_read_cached(g_ra_map, 0xC0A0)
					                   | (ra_snes_addrlist_read_cached(g_ra_map, 0xC0A1) << 8);

					if (!watch_init) {
						watch_init = 1;
						prev_d190 = cur_d190; prev_ff99 = cur_ff99;
						prev_fffa = cur_fffa; prev_ffb3 = cur_ffb3;
						prev_ffb4 = cur_ffb4; prev_ffda = cur_ffda;
						prev_score = cur_score;
					} else {
						if (cur_d190 != prev_d190)
							RA_LOG("WATCH: D190 %02X->%02X gf=%u (FF99=%02X score=%04X D192=%02X D193=%02X)",
								prev_d190, cur_d190, g_gb_game_frames, cur_ff99, cur_score,
								ra_snes_addrlist_read_cached(g_ra_map, 0xD192),
								ra_snes_addrlist_read_cached(g_ra_map, 0xD193));
						if (cur_ff99 != prev_ff99)
							RA_LOG("WATCH: FF99 %02X->%02X gf=%u (D190=%02X)",
								prev_ff99, cur_ff99, g_gb_game_frames, cur_d190);
						if (cur_fffa != prev_fffa)
							RA_LOG("WATCH: FFFA %02X->%02X gf=%u (coin count)",
								prev_fffa, cur_fffa, g_gb_game_frames);
						if (cur_ffb3 != prev_ffb3)
							RA_LOG("WATCH: FFB3 %02X->%02X gf=%u",
								prev_ffb3, cur_ffb3, g_gb_game_frames);
						if (cur_ffb4 != prev_ffb4)
							RA_LOG("WATCH: FFB4 %02X->%02X gf=%u",
								prev_ffb4, cur_ffb4, g_gb_game_frames);
						if (cur_ffda != prev_ffda)
							RA_LOG("WATCH: FFDA %02X->%02X gf=%u",
								prev_ffda, cur_ffda, g_gb_game_frames);
						if (cur_score != prev_score)
							RA_LOG("WATCH: score %04X->%04X gf=%u (D190=%02X FF99=%02X)",
								prev_score, cur_score, g_gb_game_frames, cur_d190, cur_ff99);
						prev_d190 = cur_d190; prev_ff99 = cur_ff99;
						prev_fffa = cur_fffa; prev_ffb3 = cur_ffb3;
						prev_ffb4 = cur_ffb4; prev_ffda = cur_ffda;
						prev_score = cur_score;
					}
				}

				if (re_collect) {
					g_gb_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("GB OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		// Periodic GB debug — log once per 300-frame milestone
		uint32_t milestone = g_gb_game_frames / 300;
		if (milestone > 0 && milestone != g_gb_poll_logged) {
			g_gb_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_gb_cache_time.tv_sec)
				+ (now.tv_nsec - g_gb_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_gb_game_frames > 0) ?
				(elapsed * 1000.0 / g_gb_game_frames) : 0.0;
			RA_LOG("POLL(GB): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_gb_last_resp_frame, g_gb_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return; // Gameboy handled
	}

	// ================================================================
	// SMS / Game Gear: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 11 && g_sms_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_sms_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_sms_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_sms_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("SMS OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("SMS OptionC: No addresses collected");
			}
		} else if (!g_sms_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_sms_cache_ready = 1;
				g_sms_last_resp_frame = 0;
				g_sms_game_frames = 0;
				g_sms_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_sms_cache_time);
				RA_LOG("SMS OptionC: Cache active! FPGA response matched request.");
				const uint32_t *a0 = ra_snes_addrlist_addrs();
				int ac = ra_snes_addrlist_count();
				int ad = ac < 20 ? ac : 20;
				char ah[256]; int ap = 0;
				for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
					ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
				RA_LOG("ADDRLIST[0..%d]: %s (total=%d)", ad - 1, ah, ac);
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_sms_last_resp_frame) {
				g_sms_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_sms_game_frames++;

				// Re-collect every ~5 min to catch address changes
				int re_collect = (g_sms_game_frames % 18000 == 0) && (g_sms_game_frames > 0);
				if (re_collect) {
					g_sms_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_sms_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("SMS OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		uint32_t milestone = g_sms_game_frames / 300;
		if (milestone > 0 && milestone != g_sms_poll_logged) {
			g_sms_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_sms_cache_time.tv_sec)
				+ (now.tv_nsec - g_sms_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_sms_game_frames > 0) ?
				(elapsed * 1000.0 / g_sms_game_frames) : 0.0;
			RA_LOG("POLL(SMS): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_sms_last_resp_frame, g_sms_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return; // SMS handled
	}

	// ================================================================
	// NeoGeo (Arcade): Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 27 && g_neogeo_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_neogeo_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_neogeo_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_neogeo_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("NeoGeo OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("NeoGeo OptionC: No addresses collected");
			}
		} else if (!g_neogeo_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_neogeo_cache_ready = 1;
				g_neogeo_last_resp_frame = 0;
				g_neogeo_game_frames = 0;
				g_neogeo_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_neogeo_cache_time);
				RA_LOG("NeoGeo OptionC: Cache active! FPGA response matched request.");
				const uint32_t *a0 = ra_snes_addrlist_addrs();
				int ac = ra_snes_addrlist_count();
				int ad = ac < 20 ? ac : 20;
				char ah[256]; int ap = 0;
				for (int i = 0; i < ad && ap < (int)sizeof(ah) - 8; i++)
					ap += snprintf(ah + ap, sizeof(ah) - ap, "%04X ", a0[i]);
				RA_LOG("ADDRLIST[0..%d]: %s (total=%d)", ad - 1, ah, ac);
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_neogeo_last_resp_frame) {
				g_neogeo_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_neogeo_game_frames++;

				// Re-collect every ~5 min to catch address changes
				int re_collect = (g_neogeo_game_frames % 18000 == 0) && (g_neogeo_game_frames > 0);
				if (re_collect) {
					g_neogeo_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_neogeo_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("NeoGeo OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		uint32_t milestone = g_neogeo_game_frames / 300;
		if (milestone > 0 && milestone != g_neogeo_poll_logged) {
			g_neogeo_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_neogeo_cache_time.tv_sec)
				+ (now.tv_nsec - g_neogeo_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_neogeo_game_frames > 0) ?
				(elapsed * 1000.0 / g_neogeo_game_frames) : 0.0;
			RA_LOG("POLL(NeoGeo): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_neogeo_last_resp_frame, g_neogeo_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return; // NeoGeo handled
	}

	// ================================================================
	// GBA: Option C — selective address reading
	// ================================================================
	if (g_client && g_game_loaded && ra_get_console_id() == 5 && g_gba_optionc) {
		if (ra_snes_addrlist_count() == 0 && !g_gba_cache_ready) {
			// Bootstrap: run one do_frame with zeros to discover needed addresses
			g_gba_collecting = 1;
			ra_snes_addrlist_begin_collect();
			rc_client_do_frame(g_client);
			g_gba_collecting = 0;
			int changed = ra_snes_addrlist_end_collect(g_ra_map);
			if (changed) {
				RA_LOG("GBA OptionC: Bootstrap collection done, %d addrs written to DDRAM",
					ra_snes_addrlist_count());
			} else {
				RA_LOG("GBA OptionC: No addresses collected — achievements may have no memory refs");
			}
		} else if (!g_gba_cache_ready) {
			// Wait for FPGA to respond with cached values
			if (ra_snes_addrlist_is_ready(g_ra_map)) {
				g_gba_cache_ready = 1;
				g_gba_last_resp_frame = 0;
				g_gba_game_frames = 0;
				g_gba_poll_logged = 0;
				clock_gettime(CLOCK_MONOTONIC, &g_gba_cache_time);
				RA_LOG("GBA OptionC: Cache active! FPGA response matched request.");
			}
		} else {
			// Normal frame processing from cache
			uint32_t resp_frame = ra_snes_addrlist_response_frame(g_ra_map);
			if (resp_frame > g_gba_last_resp_frame) {
				g_gba_last_resp_frame = resp_frame;
				g_last_frame = resp_frame;
				g_frames_processed++;
				g_gba_game_frames++;

				// Re-collect every ~5 min to catch address changes
				int re_collect = (g_gba_game_frames % 18000 == 0) && (g_gba_game_frames > 0);
				if (re_collect) {
					g_gba_collecting = 1;
					ra_snes_addrlist_begin_collect();
				}

				rc_client_do_frame(g_client);

				if (re_collect) {
					g_gba_collecting = 0;
					if (ra_snes_addrlist_end_collect(g_ra_map)) {
						RA_LOG("GBA OptionC: Address list refreshed, %d addrs",
							ra_snes_addrlist_count());
					}
				}
			}
		}

		uint32_t milestone = g_gba_game_frames / 300;
		if (milestone > 0 && milestone != g_gba_poll_logged) {
			g_gba_poll_logged = milestone;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double elapsed = (now.tv_sec - g_gba_cache_time.tv_sec)
				+ (now.tv_nsec - g_gba_cache_time.tv_nsec) / 1e9;
			double ms_per_cycle = (g_gba_game_frames > 0) ?
				(elapsed * 1000.0 / g_gba_game_frames) : 0.0;
			RA_LOG("POLL(GBA): resp_frame=%u game_frames=%u elapsed=%.1fs ms/cycle=%.1f addrs=%d",
				g_gba_last_resp_frame, g_gba_game_frames, elapsed, ms_per_cycle,
				ra_snes_addrlist_count());
		}
		return; // GBA handled
	}
#endif

	// ================================================================
	// Default frame tracking (NES and other cores)
	// ================================================================

	if (frame == g_last_frame) {
		// Frame counter not advancing — still run rc_client_do_frame at a
		// throttled rate (~60 Hz) so achievements can be processed even if
		// the mirror frame counter is not implemented by the core.
		static uint32_t idle_counter = 0;
		idle_counter++;
		if (idle_counter >= 300) { // roughly every 300 polls
			idle_counter = 0;
			g_frames_processed++;
#ifdef HAS_RCHEEVOS
			if (g_client && g_game_loaded) {
				rc_client_do_frame(g_client);
			}
#endif
		}
		return;
	}

	// First frame detection
	if (g_first_frame == 0) {
		g_first_frame = frame;
		RA_LOG("First frame received: %u", frame);
		ra_ramread_debug_dump(g_ra_map);
	}

	// Frame delta check (detect missed frames)
	uint32_t delta = frame - g_last_frame;
	if (delta > 1 && g_last_frame > 0) {
		RA_LOG("WARNING: Missed %u frames (last=%u, now=%u)", delta - 1, g_last_frame, frame);
	}

	g_last_frame = frame;
	g_frames_processed++;

#ifdef HAS_RCHEEVOS
	// Process achievements if game is loaded
	if (g_client && g_game_loaded) {
		rc_client_do_frame(g_client);
	}
#endif

	// Periodic debug output every ~5 seconds (300 frames at 60fps)
	if ((g_frames_processed % 300) == 1) {
		time_t now = time(NULL);
		int uptime = (int)(now - g_load_time);
		RA_LOG("POLL: frame=%u processed=%u skipped=%u uptime=%ds",
			frame, g_frames_processed, g_frames_skipped, uptime);

		// Quick RAM summary: print first 16 bytes of each region
		for (int r = 0; r < RA_MAX_REGIONS; r++) {
			const uint8_t *data = ra_ramread_region_data(g_ra_map, r);
			uint16_t size = ra_ramread_region_size(g_ra_map, r);
			if (!data || size == 0) break;

			int n = size < 16 ? size : 16;
			char hex[16 * 3 + 1] = {};
			for (int i = 0; i < n; i++) sprintf(hex + i * 3, "%02X ", data[i]);
			RA_LOG("  Region %d [%u bytes]: %s...", r, size, hex);
		}
	}

	// Detailed dump every ~60 seconds
	if ((g_frames_processed % 3600) == 1 && g_frames_processed > 1) {
		RA_LOG("=== Periodic Full Dump (every ~60s) ===");
		ra_ramread_debug_dump(g_ra_map);
	}
}

void achievements_unload_game(void)
{
	if (!ra_core_supported()) return;

	RA_LOG("--- Game Unload ---");
	RA_LOG("Stats: %u frames processed, %u skipped", g_frames_processed, g_frames_skipped);

#ifdef HAS_RCHEEVOS
	if (g_client) {
		rc_client_unload_game(g_client);
	}
#endif

	g_game_loaded = 0;
	g_game_load_pending = 0;
	g_last_frame = 0;
	g_mirror_validated = 0;
	g_snes_optionc = 0;
	g_rom_md5[0] = '\0';
	g_rom_path[0] = '\0';
	g_snes_collecting = 0;
	g_snes_cache_ready = 0;
	g_snes_last_resp_frame = 0;
	g_snes_game_frames = 0;
	g_snes_poll_logged = 0;
	g_sms_optionc = 0;
	g_sms_collecting = 0;
	g_sms_cache_ready = 0;
	g_sms_last_resp_frame = 0;
	g_sms_game_frames = 0;
	g_sms_poll_logged = 0;
	g_gb_optionc = 0;
	g_gb_collecting = 0;
	g_gb_cache_ready = 0;
	g_gb_last_resp_frame = 0;
	g_gb_game_frames = 0;
	g_gb_poll_logged = 0;
	g_gba_optionc = 0;
	g_gba_collecting = 0;
	g_gba_cache_ready = 0;
	g_gba_last_resp_frame = 0;
	g_gba_game_frames = 0;
	g_gba_poll_logged = 0;
	g_neogeo_optionc = 0;
	g_neogeo_collecting = 0;
	g_neogeo_cache_ready = 0;
	g_neogeo_last_resp_frame = 0;
	g_neogeo_game_frames = 0;
	g_neogeo_poll_logged = 0;
	ra_snes_addrlist_init();

	// Clear pending notifications
	s_urgent_head = s_urgent_tail = 0;
	s_urgent_showing  = 0;
	s_instant_pending = 0;
	s_instant_showing = 0;
}

void achievements_deinit(void)
{
	RA_LOG("=== Shutdown ===");

	achievements_unload_game();

#ifdef HAS_RCHEEVOS
	if (g_client) {
		rc_client_destroy(g_client);
		g_client = NULL;
		RA_LOG("rc_client destroyed");
	}
#endif

	ra_http_deinit();

	if (g_ra_map) {
		ra_ramread_unmap(g_ra_map);
		g_ra_map = NULL;
		RA_LOG("DDRAM mirror unmapped");
	}

	ra_log_close();
}

int achievements_active(void)
{
	return g_mirror_validated && g_ra_map != NULL;
}

int achievements_hardcore_active(void)
{
	return g_hardcore;
}

void achievements_info(void)
{
	if (!ra_core_supported()) {
		Info("RetroAchievements\n\nCore not supported", 2000, 0, 0, 1);
		return;
	}

	char buf[NOTIF_TEXT_MAX];
	int off = 0;
	int remain = sizeof(buf);

	#define NOTIF_APPEND(fmt, ...) do { \
		int n = snprintf(buf + off, remain, fmt, ##__VA_ARGS__); \
		if (n > 0 && n < remain) { off += n; remain -= n; } \
	} while(0)

	NOTIF_APPEND("RetroAchievements\n\n");

#ifdef HAS_RCHEEVOS
	if (!g_client) {
		NOTIF_APPEND("Not initialized");
	} else if (g_login_pending) {
		NOTIF_APPEND("Logging in...");
	} else if (!g_logged_in) {
		NOTIF_APPEND("Not logged in\nCheck %s", RA_CFG_PATH);
	} else {
		const rc_client_user_t *user = rc_client_get_user_info(g_client);
		if (user) {
			NOTIF_APPEND("%s", user->display_name);
		}

		if (g_game_loaded) {
			const rc_client_game_t *game = rc_client_get_game_info(g_client);
			if (game) {
				NOTIF_APPEND("\n%s", game->title);
			}

			// Count unlocked/total achievements
			rc_client_achievement_list_t *list =
				rc_client_create_achievement_list(g_client,
					RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
					RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
			if (list) {
				uint32_t total = 0, unlocked = 0;
				for (uint32_t b = 0; b < list->num_buckets; b++) {
					for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
						total++;
						if (list->buckets[b].achievements[a]->state ==
							RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED)
							unlocked++;
					}
				}
				rc_client_destroy_achievement_list(list);
				NOTIF_APPEND("\n%u/%u unlocked", unlocked, total);
			}
		} else if (g_game_load_pending) {
			NOTIF_APPEND("\nLoading game...");
		} else {
			NOTIF_APPEND("\nNo game loaded");
		}
	}
#else
	NOTIF_APPEND("Diagnostics only\n(no rcheevos lib)");
#endif

	if (g_mirror_validated) {
		NOTIF_APPEND("\nMirror: OK f%u", g_last_frame);
	} else if (g_ra_map) {
		NOTIF_APPEND("\nMirror: waiting");
	}

	#undef NOTIF_APPEND

	Info(buf, 4000, 0, 0, 1);
}
