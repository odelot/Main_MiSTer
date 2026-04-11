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

#include "achievements.h"
#include "ra_ramread.h"
#include "ra_http.h"
#include "user_io.h"
#include "file_io.h"
#include "menu.h"
#include "hardware.h"
#include "lib/md5/md5.h"

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_api_request.h"
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
// State
// ---------------------------------------------------------------------------

static void *g_ra_map = NULL;        // DDRAM mirror mmap pointer
static uint32_t g_last_frame = 0;    // Last processed frame counter
static uint32_t g_first_frame = 0;   // First valid frame seen (for uptime tracking)
static int g_game_loaded = 0;        // Game is loaded and identified
static int g_mirror_validated = 0;   // DDRAM mirror has been validated at least once
static char g_rom_md5[33] = {};      // MD5 hex string of current ROM
static char g_rom_path[1024] = {};   // Path to current ROM

#ifdef HAS_RCHEEVOS
static rc_client_t *g_client = NULL;
#endif

// Credentials
static char g_ra_user[128] = {};
static char g_ra_token[128] = {};
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
// OSD Notification Queue
// ---------------------------------------------------------------------------

#define NOTIF_QUEUE_CAP 8
#define NOTIF_TEXT_MAX  200

struct ra_notif {
	char text[NOTIF_TEXT_MAX];
	int duration_ms;
};

static ra_notif s_notif_queue[NOTIF_QUEUE_CAP];
static int s_notif_head = 0;
static int s_notif_tail = 0;
static int s_notif_showing = 0;
static unsigned long s_notif_timer = 0;

static void ra_notify(const char *text, int duration_ms = 3000)
{
	int count = s_notif_head - s_notif_tail;
	if (count >= NOTIF_QUEUE_CAP) {
		// Queue full — drop oldest
		s_notif_tail++;
		RA_LOG("OSD: Notification queue full, dropping oldest");
	}
	ra_notif *n = &s_notif_queue[s_notif_head % NOTIF_QUEUE_CAP];
	snprintf(n->text, NOTIF_TEXT_MAX, "%s", text);
	n->duration_ms = duration_ms;
	s_notif_head++;
}

// Show queued notifications via Info() popup (called from achievements_poll)
static void ra_osd_poll(void)
{
	// If currently showing, wait for timer to expire
	if (s_notif_showing) {
		if (CheckTimer(s_notif_timer)) {
			s_notif_showing = 0;
		} else {
			return;
		}
	}

	// Any notifications queued?
	if (s_notif_head == s_notif_tail) return;

	// Don't show if menu is active (user is navigating)
	if (menu_present()) return;

	// Dequeue and display
	ra_notif *n = &s_notif_queue[s_notif_tail % NOTIF_QUEUE_CAP];
	s_notif_tail++;

	// Info(text, timeout, width, height, frame)
	// frame=1 draws a border. We give Info a slightly longer timeout than our
	// own timer so the popup stays visible until we advance to the next one.
	Info(n->text, n->duration_ms + 500, 0, 0, 1);

	s_notif_timer = GetTimer(n->duration_ms);
	s_notif_showing = 1;

	RA_LOG("OSD: Showing notification (%dms)", n->duration_ms);
}

// ---------------------------------------------------------------------------
// ROM MD5 calculation
// ---------------------------------------------------------------------------

static int ra_get_console_id(void); // forward declaration

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
//   token=YourAPIToken
//   # Lines starting with # are comments
//
// Token is obtained from retroachievements.org → Settings → API Keys.

static int ra_load_credentials(void)
{
	g_ra_user[0] = '\0';
	g_ra_token[0] = '\0';

	FILE *f = fopen(RA_CFG_PATH, "r");
	if (!f) {
		RA_LOG("Credentials file not found: %s", RA_CFG_PATH);
		RA_LOG("To enable RetroAchievements, create the file with:");
		RA_LOG("  username=YourRAUsername");
		RA_LOG("  token=YourAPIToken");
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
		} else if (!strcasecmp(key, "token")) {
			snprintf(g_ra_token, sizeof(g_ra_token), "%s", val);
		}
	}
	fclose(f);

	if (!g_ra_user[0] || !g_ra_token[0]) {
		RA_LOG("Credentials incomplete (need both username and token)");
		return 0;
	}

	RA_LOG("Credentials loaded: user=%s token=***(%zu chars)", g_ra_user, strlen(g_ra_token));
	return 1;
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
		return num_bytes;
	}
	return ra_ramread_nes_read(g_ra_map, address, buffer, num_bytes);
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
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				">> ACHIEVEMENT <<\n\n%s\n%s",
				event->achievement->title,
				event->achievement->description);
			ra_notify(buf, 4000);
			ra_play_achievement_sound();
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		{
			RA_LOG("CHALLENGE SHOW: [%u] %s",
				event->achievement->id, event->achievement->title);
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				"CHALLENGE ACTIVE\n\n%s",
				event->achievement->title);
			ra_notify(buf, 3000);
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		RA_LOG("CHALLENGE HIDE: [%u] %s",
			event->achievement->id, event->achievement->title);
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		{
			RA_LOG("PROGRESS: %s", event->achievement->measured_progress);
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf),
				"Progress: %s",
				event->achievement->measured_progress);
			ra_notify(buf, 2000);
		}
		break;

	case RC_CLIENT_EVENT_GAME_COMPLETED:
		RA_LOG("*** GAME COMPLETED! ***");
		ra_notify("** GAME COMPLETED! **\n\nCongratulations!", 5000);
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

void achievements_init(void)
{
	ra_log_open();
	RA_LOG("=== RetroAchievements for MiSTer ===");
	RA_LOG("Phase 3 — Full pipeline: HTTP + login + achievements");

	const char *core = user_io_get_core_name(1);
	int console_id = ra_get_console_id();
	RA_LOG("Core: '%s' -> console_id=%d supported=%d", core ? core : "(null)",
		console_id, ra_core_supported());

	if (!ra_core_supported()) {
		RA_LOG("Core not supported for RetroAchievements. Inactive.");
		return;
	}

	// Map DDRAM mirror region
	g_ra_map = ra_ramread_map();
	if (!g_ra_map) {
		RA_LOG("ERROR: Failed to mmap DDRAM mirror at 0x%08X", RA_DDRAM_PHYS_BASE);
		return;
	}
	RA_LOG("DDRAM mirror mapped at 0x%08X (%u bytes)", RA_DDRAM_PHYS_BASE, RA_DDRAM_MAP_SIZE);

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
	rc_client_set_hardcore_enabled(g_client, 0); // Softcore for now (no anti-tamper)

	// Configure User-Agent: "MiSTer/1.0 rcheevos/x.y.z"
	{
		char ua_clause[64] = "";
		rc_client_get_user_agent_clause(g_client, ua_clause, sizeof(ua_clause));
		char ua[128];
		snprintf(ua, sizeof(ua), "MiSTer/1.0 %s", ua_clause);
		ra_http_set_user_agent(ua);
		RA_LOG("User-Agent: %s", ua);
	}

	RA_LOG("rc_client created successfully");

	// Begin login if credentials available
	if (has_creds) {
		RA_LOG("Starting login for user '%s'...", g_ra_user);
		g_login_pending = 1;
		rc_client_begin_login_with_token(g_client, g_ra_user, g_ra_token,
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

	RA_LOG("--- Game Load ---");
	RA_LOG("ROM path: %s", rom_path);
	RA_LOG("CRC32: %08X", crc32);

	// Store ROM path
	snprintf(g_rom_path, sizeof(g_rom_path), "%s", rom_path);

	// Calculate MD5
	g_rom_md5[0] = '\0';
	if (rom_path && rom_path[0]) {
		ra_calculate_rom_md5(rom_path, g_rom_md5);
	}

	// Reset frame tracking
	g_last_frame = 0;
	g_first_frame = 0;
	g_mirror_validated = 0;
	g_frames_processed = 0;
	g_frames_skipped = 0;
	g_game_loaded = 0;
	g_load_time = time(NULL);

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
}

void achievements_poll(void)
{
	static uint32_t poll_calls = 0;
	poll_calls++;

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
		}
		return;
	}

	// Read frame counter
	uint32_t frame = ra_ramread_frame(g_ra_map);
	int busy = ra_ramread_busy(g_ra_map);

	// Periodic diagnostics every ~5s (assuming poll is called frequently)
	if ((poll_calls % 18000) == 1) {
		RA_LOG("DIAG: poll_calls=%u frame=%u last_frame=%u busy=%d processed=%u skipped=%u game_loaded=%d",
			poll_calls, frame, g_last_frame, busy, g_frames_processed, g_frames_skipped, g_game_loaded);

		// Dump first 32 bytes of CPU-RAM region to verify data is arriving
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

	// NOTE: Busy flag is currently ignored because some cores keep it permanently
	// set. We process frames regardless. If data corruption is observed, we can
	// revisit this and add a short-lived busy check (e.g., skip only if busy was
	// set for less than N polls).
	(void)busy;

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
	g_rom_md5[0] = '\0';
	g_rom_path[0] = '\0';

	// Clear pending notifications
	s_notif_head = s_notif_tail = 0;
	s_notif_showing = 0;
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
