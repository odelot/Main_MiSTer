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
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>

#include "achievements.h"
#include "achievements_console.h"
#include "ra_ramread.h"
#include "ra_http.h"
#include "user_io.h"
#include "cfg.h"
#include "file_io.h"
#include "menu.h"
#include "osd.h"
#include "hardware.h"
#include "lib/md5/md5.h"
#include "ra_cdreader_chd.h"

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
static int g_ra_debug = 0; // forward decl — defined/loaded in ra_load_credentials
static int g_async_log = 1; // retroachievements.cfg: async_log (1 = writer thread)
static int g_trigger_dump = 1; // retroachievements.cfg: trigger_dump (needs debug=1)

#define RA_LOG(fmt, ...) ra_log_impl("RA: " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Async log writer
//
// Synchronous logging costs a vprintf to stdout (often a slow serial console)
// plus a vfprintf + fflush per line, on the calling thread. Bulk diagnostics
// pay that per address: the PROGRESS valcache dump emits up to ~1000 lines
// inside rc_client_do_frame(), which measured ~30ms — enough to miss two
// emulated frames. The unlock path (curl, aplay, OSD) is already off the main
// loop; this puts logging there too.
//
// Producers (main loop + HTTP worker) only format into a stack buffer and
// memcpy it into a ring slot; the writer thread does all I/O and flushes once
// per drained batch instead of once per line. A full ring drops lines and
// counts them rather than blocking a producer — never stall the main loop for
// a debug feature.
// ---------------------------------------------------------------------------

// Slot width must clear the longest line actually emitted, otherwise the
// trailing '\n' is what gets cut and the next line is appended to the
// truncated one, corrupting the log format. Measured worst case is the
// HTTP_WORKER curl command (~360 chars) and the Config summary (~280).
// Bytes are untouched pages until used, so the ceiling is cheap.
#define RA_LOGQ_SLOTS 4096
#define RA_LOGQ_LINE  512

static char     s_logq[RA_LOGQ_SLOTS][RA_LOGQ_LINE];
static unsigned s_logq_head = 0;   // next write slot (producers)
static unsigned s_logq_tail = 0;   // next read slot (writer)
static unsigned s_logq_dropped = 0;
static pthread_mutex_t s_logq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_logq_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t s_logq_thread;
static int s_logq_running = 0;
static int s_logq_quit = 0;

// Write one line to both sinks. Caller owns flushing.
static void ra_log_sink(const char *line)
{
	printf("\033[1;35m%s\033[0m", line);
	if (g_logfile) fputs(line, g_logfile);
}

static void *ra_log_thread(void *arg)
{
	(void)arg;
	for (;;) {
		char line[RA_LOGQ_LINE];
		unsigned dropped = 0;
		int have = 0;

		pthread_mutex_lock(&s_logq_mutex);
		while (s_logq_head == s_logq_tail && !s_logq_quit)
			pthread_cond_wait(&s_logq_cond, &s_logq_mutex);
		if (s_logq_head != s_logq_tail) {
			const char *slot = s_logq[s_logq_tail % RA_LOGQ_SLOTS];
			size_t len = strnlen(slot, RA_LOGQ_LINE - 1);
			memcpy(line, slot, len);
			line[len] = '\0';
			s_logq_tail++;
			have = 1;
		} else if (s_logq_quit) {
			pthread_mutex_unlock(&s_logq_mutex);
			break;
		}
		// Report drops from the writer side so producers stay allocation- and
		// I/O-free even when the ring overflows.
		if (s_logq_dropped && s_logq_head == s_logq_tail) {
			dropped = s_logq_dropped;
			s_logq_dropped = 0;
		}
		pthread_mutex_unlock(&s_logq_mutex);

		if (have) ra_log_sink(line);
		if (dropped) {
			char note[96];
			snprintf(note, sizeof(note),
				"RA: LOG: %u line(s) dropped (async queue full)\n", dropped);
			ra_log_sink(note);
		}
		// Flush only when the batch is exhausted: a burst of N lines costs one
		// fflush instead of N.
		pthread_mutex_lock(&s_logq_mutex);
		int drained = (s_logq_head == s_logq_tail);
		pthread_mutex_unlock(&s_logq_mutex);
		if (drained && g_logfile) fflush(g_logfile);
	}
	if (g_logfile) fflush(g_logfile);
	return NULL;
}

// Format + hand off. Falls back to a synchronous write when the writer is not
// running (toggle off, before startup, or after shutdown) so no line is lost.
static void ra_log_emit(const char *fmt, va_list args)
{
	char line[RA_LOGQ_LINE];
	int n = vsnprintf(line, sizeof(line), fmt, args);
	if (n < 0) return;

	// Over-long line: vsnprintf dropped the tail including the newline. Mark
	// the cut and restore the terminator so the next line still starts fresh.
	size_t len = (size_t)n;
	if (n >= (int)sizeof(line)) {
		memcpy(line + sizeof(line) - 5, "...\n", 5);
		len = sizeof(line) - 1;
	}

	if (!s_logq_running) {
		ra_log_sink(line);
		if (g_logfile) fflush(g_logfile);
		return;
	}

	pthread_mutex_lock(&s_logq_mutex);
	if (s_logq_head - s_logq_tail >= RA_LOGQ_SLOTS) {
		s_logq_dropped++;               // ring full: drop, never block
	} else {
		// Copy only the bytes in use (+NUL), not the whole slot: a bulk dump
		// of ~800 short lines would otherwise memcpy 400KB per frame just to
		// move ~30KB of text. Slots stay NUL-terminated, so leftover bytes
		// from a previous longer line are never read.
		memcpy(s_logq[s_logq_head % RA_LOGQ_SLOTS], line, len + 1);
		s_logq_head++;
		pthread_cond_signal(&s_logq_cond);
	}
	pthread_mutex_unlock(&s_logq_mutex);
}

void ra_log_write(const char *fmt, ...)
{
	if (!g_ra_debug) return;
	va_list args;
	va_start(args, fmt);
	ra_log_emit(fmt, args);
	va_end(args);
}

static void ra_log_impl(const char *fmt, ...)
{
	if (!g_ra_debug) return;
	va_list args;
	va_start(args, fmt);
	ra_log_emit(fmt, args);
	va_end(args);
}

// Block until the queue is empty. For shutdown and the crash path, where the
// log must be on disk before we stop running.
static void ra_log_drain(void)
{
	if (!s_logq_running) return;
	for (int i = 0; i < 2000; i++) {  // bounded: ~2s worst case, never hangs
		pthread_mutex_lock(&s_logq_mutex);
		int empty = (s_logq_head == s_logq_tail);
		pthread_mutex_unlock(&s_logq_mutex);
		if (empty) return;
		usleep(1000);
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

// Start/stop the writer to match async_log. Separate from ra_log_open because
// the config (and with it the toggle) is parsed later during init; until this
// runs, ra_log_emit falls back to synchronous writes.
static void ra_log_apply_config(void)
{
	if (g_async_log && !s_logq_running) {
		s_logq_quit = 0;
		if (pthread_create(&s_logq_thread, NULL, ra_log_thread, NULL) == 0)
			s_logq_running = 1;
	} else if (!g_async_log && s_logq_running) {
		ra_log_drain();
		pthread_mutex_lock(&s_logq_mutex);
		s_logq_quit = 1;
		pthread_cond_signal(&s_logq_cond);
		pthread_mutex_unlock(&s_logq_mutex);
		pthread_join(s_logq_thread, NULL);
		s_logq_running = 0;
	}
}

static void ra_log_close(void)
{
	if (s_logq_running) {
		ra_log_drain();
		pthread_mutex_lock(&s_logq_mutex);
		s_logq_quit = 1;
		pthread_cond_signal(&s_logq_cond);
		pthread_mutex_unlock(&s_logq_mutex);
		pthread_join(s_logq_thread, NULL);
		s_logq_running = 0;   // later logs go synchronous again
	}
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
		// Flush whatever the async writer still holds: the lines right before
		// a crash are the ones worth having. Drained WITHOUT the mutex on
		// purpose — the crash may have happened while a producer held it, and
		// a deadlock here would cost us the backtrace too. The process is
		// dying, so a torn read is an acceptable trade for not hanging.
		if (s_logq_running) {
			unsigned tail = s_logq_tail, head = s_logq_head;
			if (head - tail > RA_LOGQ_SLOTS) tail = head - RA_LOGQ_SLOTS;
			for (unsigned i = tail; i != head; i++)
				fputs(s_logq[i % RA_LOGQ_SLOTS], g_logfile);
		}
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

// Active console handler (set in achievements_init, dispatches all per-console logic)
static const console_handler_t *g_active_handler = NULL;

#ifdef HAS_RCHEEVOS
static rc_client_t *g_client = NULL;
#endif

// Credentials
static char g_ra_user[128] = {};
static char g_ra_password[128] = {};
static int g_has_credentials = 0;
static int g_logged_in = 0;
static int g_login_pending = 0;
static int g_game_load_pending = 0;
static int g_login_deferred = 0;      // login deferred until FPGA mirror validated
static int g_game_load_deferred = 0;  // game load deferred until
static int      g_mirror_confirming = 0;       // magic seen, waiting for frame counter to advance
static uint32_t g_mirror_initial_frame = 0;    // frame when magic was first seen (stale detection) FPGA mirror validated

// Debug counters
static uint32_t g_frames_processed = 0;
static uint32_t g_frames_skipped = 0;  // frames where busy flag was set
static time_t g_load_time = 0;

void ra_frame_processed(uint32_t frame)
{
	g_last_frame = frame;
	g_frames_processed++;
}

// Config file path
#define RA_CFG_PATH  "/media/fat/retroachievements.cfg"
#define RA_SFX_PATH  "/media/fat/achievement.wav"

// Popup display settings (from retroachievements.cfg)
static int g_show_challenge_show_popup = 1; // 1 = show popup on challenge SHOW event
static int g_show_challenge_hide_popup = 1; // 1 = show popup on challenge HIDE event
static int g_show_progress_popups      = 1; // 1 = show progress indicator popups
static int g_show_progress_name        = 1; // 1 = include achievement name in progress popup
static int g_leaderboards_enabled      = 1; // [deprecated] fallback only when both new leaderboard popup flags are absent
static int g_show_leaderboards_updates = 1; // 1 = show STARTED/FAILED/TRACKER SHOW/TRACKER UPDATE popups
static int g_show_leaderboards_submission = 1; // 1 = show SUBMITTED/SCOREBOARD popups
static int g_hardcore                  = 0; // 1 = hardcore mode (disables cheats & save states)
static int g_force_hardcore            = 0; // 1 = force hardcore mode even if core doesn't support it
static int g_stall_recovery            = 0; // 1 = enable SelAddr stall recovery (disabled by default)
static int g_rtquery_enabled           = 1; // 1 = enable realtime queries for AddAddress resolution
static int g_gba_reset_ram             = 1; // 1 = clear IWRAM+EWRAM on game load (retroachievements.cfg: gba_reset_ram)
static int g_recollect_interval        = 600; // frames between address re-collections (PSX default 600, SNES 18000)
static int g_smart_cache               = -1; // -1 = default per console, 1 = smart cache: rtquery on cache miss, no periodic recollect
static int g_n64_snapshot              = 0;  // 1 = snapshot RDRAM at VBlank for consistent reads
static int g_multiline_desc            = 0;  // 1 = wrap long text to extra lines instead of truncating with "..."
static int g_pending_reset_request     = 0;  // RC_CLIENT_EVENT_RESET received: reset the core on the next poll tick
static int g_smart_cleanup             = 1;  // 1 = dynamic-only smart-cache prune (SNES/NES/MD): drops AddAddress targets ~1/min; statics never pruned
static int g_justifier_test            = 0;  // 1 = MegaDrive only: cap rtquery busy-wait (~1ms + fail-fast) to A/B test lightgun input latency
static int g_desc_ticker               = 0;  // 1 = scroll the selected achievement's description on a ticker row in the list view (retroachievements.cfg: list_desc_ticker)
static int g_list_hotkey               = 0;  // 1 = in-game gamepad shortcut (Menu + Y) opens the achievement list (retroachievements.cfg: list_hotkey)
static int g_popup_pos                 = INFO_ALIGN_LEFT; // popup corner: left (default) / center / right (retroachievements.cfg: popup_position)

// Debug watch list (retroachievements.cfg: watch=19807d,19795a — RA addresses
// in hex). Handlers log every value change of these addresses per frame.
#define RA_WATCH_MAX 16
static uint32_t g_watch_addrs[RA_WATCH_MAX];
static int g_watch_count = 0;
static char g_ua_clause[64]            = ""; // rcheevos user-agent clause (e.g. "rcheevos/11.6")
static char g_fpga_core_version[8]     = "0.1"; // version reported by FPGA in DDRAM header

// ---------------------------------------------------------------------------
// Per-achievement event state (rate-limiting CHALLENGE, dedup PROGRESS)
// ---------------------------------------------------------------------------

#define RA_ACH_STATE_MAX 128
struct ra_ach_state_t {
	uint32_t id;
	time_t   challenge_last_popup; // monotonic: last time SHOW popup was shown
	char     progress_last[32];    // last progress string displayed
};
static ra_ach_state_t g_ach_state[RA_ACH_STATE_MAX];
static int g_ach_state_count = 0;

#define CHALLENGE_POPUP_COOLDOWN_SEC  10  // suppress CHALLENGE SHOW popup if one was shown < 10s ago
#define PROGRESS_SAME_VAL_COOLDOWN_SEC 5  // suppress PROGRESS popup if same value shown < 5s ago

static ra_ach_state_t *ra_ach_state_get(uint32_t id)
{
	for (int i = 0; i < g_ach_state_count; i++)
		if (g_ach_state[i].id == id) return &g_ach_state[i];
	if (g_ach_state_count < RA_ACH_STATE_MAX) {
		ra_ach_state_t *s = &g_ach_state[g_ach_state_count++];
		s->id                  = id;
		s->challenge_last_popup = 0;
		s->progress_last[0]    = '\0';
		return s;
	}
	return NULL;
}

// Returns 1 if CHALLENGE SHOW popup should be suppressed (fired too recently)
static int ra_challenge_popup_suppressed(uint32_t id)
{
	ra_ach_state_t *s = ra_ach_state_get(id);
	if (!s) return 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	time_t elapsed = now.tv_sec - s->challenge_last_popup;
	if (elapsed < CHALLENGE_POPUP_COOLDOWN_SEC) return 1;
	s->challenge_last_popup = now.tv_sec;
	return 0;
}

// Returns 1 if PROGRESS popup should be suppressed (same value shown recently)
static int ra_progress_popup_suppressed(uint32_t id, const char *progress)
{
	ra_ach_state_t *s = ra_ach_state_get(id);
	if (!s) return 0;
	if (strcmp(s->progress_last, progress) == 0) {
		// Same value as last time — suppress unless it's been a while
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		// (future: could add time-based override here)
		return 1;
	}
	// New value — update and allow
	snprintf(s->progress_last, sizeof(s->progress_last), "%s", progress);
	return 0;
}

// ---------------------------------------------------------------------------
// Achievement Sound
// ---------------------------------------------------------------------------

static void *ra_play_thread(void *arg)
{
	(void)arg;
	// Only play if the file exists — silent no-op otherwise
	if (access(RA_SFX_PATH, R_OK) != 0) return NULL;

	// fork/exec instead of system(): on cores built with MISTER_DISABLE_ALSA
	// (e.g. N64, PSX) the FPGA never drains the ALSA ring buffer, so aplay
	// blocks forever and every unlock would leak a process + thread. Kill it
	// after a timeout instead.
	pid_t pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
		execlp("aplay", "aplay", "-q", RA_SFX_PATH, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) return NULL;

	// The jingle lasts ~1s; allow 5s before declaring aplay stuck.
	for (int i = 0; i < 50; i++) {
		if (waitpid(pid, NULL, WNOHANG) == pid) return NULL;
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
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
	int play_sound;
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
static void ra_notify_urgent(const char *text, int duration_ms = 4000, int play_sound = 0)
{
	int count = s_urgent_head - s_urgent_tail;
	if (count >= NOTIF_QUEUE_CAP) {
		s_urgent_tail++;
		RA_LOG("OSD: Urgent queue full, dropping oldest");
	}
	ra_notif *n = &s_urgent_queue[s_urgent_head % NOTIF_QUEUE_CAP];
	snprintf(n->text, NOTIF_TEXT_MAX, "%s", text);
	n->duration_ms = duration_ms;
	n->play_sound  = play_sound;
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
		InfoAligned(n->text, n->duration_ms + 500, g_popup_pos, 1);
		if (n->play_sound) ra_play_achievement_sound();
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
			InfoAligned(s_instant_text, s_instant_duration_ms + 500, g_popup_pos, 1);
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
	
	// FDS: skip fwNES FDS header ("FDS\x1a")
	if (file_size > 16 &&
		rom_data[0] == 0x46 && rom_data[1] == 0x44 &&  // 'F' 'D'
		rom_data[2] == 0x53 && rom_data[3] == 0x1A) {  // 'S' 0x1a
		RA_LOG("FDS header detected, skipping 16 bytes");
		hash_data = rom_data + 16;
		hash_size = file_size - 16;
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

// Popup corner (popup_position). Accepts the names left / center / right —
// also "centre" and the numbers 0 / 1 / 2 — case-insensitively. Any other
// value falls back to the default (left) and is reported in the log.
static int ra_parse_popup_pos(const char *val)
{
	char v[16];
	snprintf(v, sizeof(v), "%s", val);
	size_t n = strlen(v);
	while (n && (v[n-1] == ' ' || v[n-1] == '\t')) v[--n] = '\0';

	if (!strcasecmp(v, "left")   || !strcmp(v, "0")) return INFO_ALIGN_LEFT;
	if (!strcasecmp(v, "center") || !strcasecmp(v, "centre") || !strcmp(v, "1")) return INFO_ALIGN_CENTER;
	if (!strcasecmp(v, "right")  || !strcmp(v, "2")) return INFO_ALIGN_RIGHT;

	RA_LOG("Config: popup_position=\"%s\" not supported, falling back to \"left\"", v);
	return INFO_ALIGN_LEFT;
}

static const char *ra_popup_pos_name(int pos)
{
	return (pos == INFO_ALIGN_CENTER) ? "center" : (pos == INFO_ALIGN_RIGHT) ? "right" : "left";
}

static int ra_load_credentials(void)
{
	g_ra_user[0] = '\0';
	g_ra_password[0] = '\0';
	int legacy_leaderboards_defined = 0;
	int show_leaderboards_updates_defined = 0;
	int show_leaderboards_submission_defined = 0;

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
		} else if (!strcasecmp(key, "show_leaderboards_updates") ||
					   !strcasecmp(key, "show-leaderboards-updates")) {
			g_show_leaderboards_updates = atoi(val);
			show_leaderboards_updates_defined = 1;
		} else if (!strcasecmp(key, "show_leaderboards_submission") ||
					   !strcasecmp(key, "show-leaderboards-submission")) {
			g_show_leaderboards_submission = atoi(val);
			show_leaderboards_submission_defined = 1;
		} else if (!strcasecmp(key, "leaderboards-enabled") ||
					!strcasecmp(key, "leaderboards_enabled")) {
				g_leaderboards_enabled = atoi(val);
				legacy_leaderboards_defined = 1;
		} else if (!strcasecmp(key, "hardcore")) {
			g_hardcore = atoi(val);
		} else if (!strcasecmp(key, "force_hardcore")) {
			g_force_hardcore = atoi(val);
		} else if (!strcasecmp(key, "stall_recovery")) {
			g_stall_recovery = atoi(val);
		} else if (!strcasecmp(key, "rtquery_enabled") ||
					!strcasecmp(key, "rtquery")) {
			g_rtquery_enabled = atoi(val);
		} else if (!strcasecmp(key, "recollect_interval")) {
			g_recollect_interval = atoi(val);
			if (g_recollect_interval < 60) g_recollect_interval = 60; // minimum 1 second
		} else if (!strcasecmp(key, "smart_cache")) {
			g_smart_cache = atoi(val);
		} else if (!strcasecmp(key, "debug")) {
			g_ra_debug = atoi(val);
		} else if (!strcasecmp(key, "async_log")) {
			g_async_log = atoi(val);
		} else if (!strcasecmp(key, "trigger_dump")) {
			g_trigger_dump = atoi(val);
		} else if (!strcasecmp(key, "n64_snapshot")) {
			g_n64_snapshot = atoi(val);
		} else if (!strcasecmp(key, "gba_reset_ram")) {
			g_gba_reset_ram = atoi(val);
		} else if (!strcasecmp(key, "multiline_desc")) {
			g_multiline_desc = atoi(val);
		} else if (!strcasecmp(key, "list_desc_ticker")) {
			g_desc_ticker = atoi(val);
		} else if (!strcasecmp(key, "list_hotkey")) {
			g_list_hotkey = atoi(val);
		} else if (!strcasecmp(key, "popup_position")) {
			g_popup_pos = ra_parse_popup_pos(val);
		} else if (!strcasecmp(key, "smart_cleanup")) {
			g_smart_cleanup = atoi(val);
		} else if (!strcasecmp(key, "justifier_test")) {
			g_justifier_test = atoi(val);
		} else if (!strcasecmp(key, "watch")) {
			g_watch_count = 0;
			const char *p = val;
			while (*p && g_watch_count < RA_WATCH_MAX) {
				char *end;
				unsigned long a = strtoul(p, &end, 16);
				if (end == p) break;
				g_watch_addrs[g_watch_count++] = (uint32_t)a;
				p = end;
				while (*p == ',' || *p == ' ') p++;
			}
		}
	}
	fclose(f);

	/* Backward compatibility: transfer deprecated value only when both new keys are absent. */
	if (!show_leaderboards_updates_defined && !show_leaderboards_submission_defined && legacy_leaderboards_defined) {
		g_show_leaderboards_updates = g_leaderboards_enabled;
		g_show_leaderboards_submission = g_leaderboards_enabled;
	}

	if (!g_ra_user[0] || !g_ra_password[0]) {
		RA_LOG("Credentials incomplete (need both username and password)");
		return 0;
	}

	RA_LOG("Credentials loaded: user=%s password=***(%zu chars)", g_ra_user, strlen(g_ra_password));
	RA_LOG("Config: show_challenge_show=%d show_challenge_hide=%d show_progress=%d show_progress_name=%d show_leaderboards_updates=%d show_leaderboards_submission=%d leaderboards_enabled(deprecated)=%d hardcore=%d force_hardcore=%d stall_recovery=%d rtquery=%d recollect=%d smart_cache=%d smart_cleanup=%d justifier_test=%d n64_snapshot=%d gba_reset_ram=%d multiline_desc=%d list_desc_ticker=%d list_hotkey=%d popup_position=%s debug=%d async_log=%d",
                g_show_challenge_show_popup, g_show_challenge_hide_popup,
                g_show_progress_popups, g_show_progress_name,
		g_show_leaderboards_updates, g_show_leaderboards_submission, g_leaderboards_enabled,
                g_hardcore, g_force_hardcore, g_stall_recovery, g_rtquery_enabled, g_recollect_interval, g_smart_cache, g_smart_cleanup, g_justifier_test, g_n64_snapshot, g_gba_reset_ram, g_multiline_desc, g_desc_ticker, g_list_hotkey, ra_popup_pos_name(g_popup_pos), g_ra_debug, g_async_log);
	return 1;
}


// ---------------------------------------------------------------------------
// Text formatting helper: truncate with "..." or wrap to multiple lines
// ---------------------------------------------------------------------------

// Word-wrap `text` into `out` as a '\n'-separated string of at most
// `max_lines` lines, each up to `wrap_width` characters. Breaks on spaces
// when possible; a word longer than the line hard-breaks. If the text does
// not fit, the last line ends with "...". Unlike ra_format_text this always
// wraps, regardless of the multiline_desc option (used by the detail view).
static void ra_wrap_text(const char *text, char *out, size_t out_size, int wrap_width, int max_lines)
{
	if (!text || !out || out_size == 0) return;
	size_t len = strlen(text);
	if (len <= (size_t)wrap_width) {
		snprintf(out, out_size, "%s", text);
		return;
	}
	{
		size_t pos = 0, written = 0;
		for (int line = 0; line < max_lines && pos < len && written + 1 < out_size; line++) {
			if (line > 0) { out[written++] = '\n'; out[written] = '\0'; }
			size_t take = len - pos;
			if (take > (size_t)wrap_width) {
				take = (size_t)wrap_width;
				// Word wrap: if the cut lands inside a word, break at the last
				// space instead so the whole word moves to the next line.
				// (A single word longer than the line still hard-breaks.)
				if (text[pos + take] != ' ') {
					size_t s = take;
					while (s > 0 && text[pos + s - 1] != ' ') s--;
					if (s > 0) take = s;
				}
			}
			size_t avail = out_size - written - 1;
			if (take > avail) take = avail;
			memcpy(out + written, text + pos, take);
			// Drop trailing spaces from the line end
			while (take > 0 && out[written + take - 1] == ' ') take--;
			written += take;
			out[written] = '\0';
			pos += take;
			// Skip spaces at the start of the next line
			while (pos < len && text[pos] == ' ') pos++;
		}
		if (pos < len && written + 3 < out_size) {
			// The "..." must also fit in wrap_width: shrink the last line to
			// wrap_width-3, preferring a word boundary.
			size_t line_start = written;
			while (line_start > 0 && out[line_start - 1] != '\n') line_start--;
			size_t line_len = written - line_start;
			if (line_len + 3 > (size_t)wrap_width) {
				size_t keep = (size_t)wrap_width - 3;
				size_t s = keep;
				while (s > 0 && out[line_start + s - 1] != ' ') s--;
				if (s > 0) keep = s;
				while (keep > 0 && out[line_start + keep - 1] == ' ') keep--;
				written = line_start + keep;
				out[written] = '\0';
			}
			strcat(out, "...");
		}
	}
}

// Format an achievement title/description for a popup line. With multiline_desc
// off, long text is truncated to trunc_width with a trailing "..."; with it on,
// the text is word-wrapped over up to max_lines lines of wrap_width columns.
static void ra_format_text(const char *text, char *out, size_t out_size, int trunc_width, int wrap_width, int max_lines)
{
	if (!text || !out || out_size == 0) return;
	size_t len = strlen(text);
	if (!g_multiline_desc) {
		if (len <= (size_t)trunc_width) {
			snprintf(out, out_size, "%s", text);
			return;
		}
		size_t copy = (size_t)trunc_width < out_size - 1 ? (size_t)trunc_width : out_size - 1;
		memcpy(out, text, copy);
		out[copy] = '\0';
		if (copy + 3 < out_size) strcat(out, "...");
	} else {
		ra_wrap_text(text, out, out_size, wrap_width, max_lines);
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
		return ra_core_supported() ? num_bytes : 0;
	}

	// Dispatch to active console handler
	if (g_active_handler && g_active_handler->read_memory) {
		uint32_t r = g_active_handler->read_memory(g_ra_map, address, buffer, num_bytes);
		if (r > 0) return r;
	}

	// Fallback: VBlank-gated region reads for NES and SNES
	int console = ra_get_console_id();
	if (console == 7)  // NES
		return ra_ramread_nes_read(g_ra_map, address, buffer, num_bytes);
	if (console == 3)  // SNES VBlank-gated (handler returned 0 = not seladdr)
		return ra_ramread_snes_read(g_ra_map, address, buffer, num_bytes);

	memset(buffer, 0, num_bytes);
	return num_bytes;
}

static void ra_server_call(const rc_api_request_t *request,
	rc_client_server_callback_t callback, void *callback_data,
	rc_client_t *client)
{
	(void)client;

	// Log the request (mask token and password for security)
	if (request->post_data) {
		const char *token_pos = strstr(request->post_data, "&t=");
		const char *pass_pos  = strstr(request->post_data, "&p=");
		const char *mask_pos  = token_pos ? token_pos : pass_pos;
		const char *mask_key  = token_pos ? "&t=" : "&p=";
		if (mask_pos) {
			int prefix_len = (int)(mask_pos - request->post_data);
			RA_LOG("HTTP: POST %s [%.*s%s***]", request->url, prefix_len, request->post_data, mask_key);
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

	ra_http_request(request->url, request->post_data, request->content_type,
		http_done, bridge);
}

static void ra_event_handler(const rc_client_event_t *event, rc_client_t *client)
{
	(void)client;
	RA_LOG("Event: type=%d", event->type);
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		{
			if (event->achievement->id == 101000001) {
				ra_notify_urgent("HARDCORE mode DISABLED\nCORE still not supported", 3500);
			} else {
				RA_LOG("*** ACHIEVEMENT TRIGGERED: [%u] %s — %s ***",
					event->achievement->id, event->achievement->title,
					event->achievement->description);
					gba_dump_trigger(event->achievement->id);
					seladdr_trigdump_report(event->achievement->id,
						event->achievement->title);
				char title_buf[96];
				ra_format_text(event->achievement->title, title_buf, sizeof(title_buf), 28, 28, 2);
				// In multiline mode, prefix desc with "\-> " so it reads as a
				// sub-line of the title. The prefix is added BEFORE wrapping so
				// its 4 chars count toward the first line's width (adding it
				// after wrapping pushed the first line past the OSD width).
				char desc_display[200];
				if (g_multiline_desc) {
					char desc_prefixed[224];
					snprintf(desc_prefixed, sizeof(desc_prefixed), "\\-> %s",
						event->achievement->description);
					ra_format_text(desc_prefixed, desc_display, sizeof(desc_display), 28, 28, 3);
				} else {
					ra_format_text(event->achievement->description, desc_display, sizeof(desc_display), 28, 28, 3);
				}
				char buf[NOTIF_TEXT_MAX];
				snprintf(buf, sizeof(buf),
					">> ACHIEVEMENT <<\n\n%s\n%s",
					title_buf, desc_display);
								ra_notify_urgent(buf, 4000, 1);
			}
		}
		break;

	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		{
			RA_LOG("CHALLENGE SHOW: [%u] %s",
				event->achievement->id, event->achievement->title);
			if (g_show_challenge_show_popup &&
			    !ra_challenge_popup_suppressed(event->achievement->id)) {
				char title_buf[96];
				ra_format_text(event->achievement->title, title_buf, sizeof(title_buf), 28, 28, 2);
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
				char title_buf[96];
				ra_format_text(event->achievement->title, title_buf, sizeof(title_buf), 28, 28, 2);
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
			// Dump all cached values on progress events
			if (g_ra_map) {
				int cnt = ra_snes_addrlist_count();
				const uint32_t *addrs = ra_snes_addrlist_addrs();
				for (int i = 0; i < cnt; i++) {
					uint8_t v = ra_snes_addrlist_read_cached(g_ra_map, addrs[i]);
					RA_LOG("  COND[%d] addr=0x%05X val=0x%02X", i, addrs[i], v);
				}
			}
			if (g_show_progress_popups &&
			    !ra_progress_popup_suppressed(event->achievement->id, event->achievement->measured_progress)) {
				char buf[NOTIF_TEXT_MAX];
				if (g_show_progress_name) {
					char title_buf[96];
					ra_format_text(event->achievement->title, title_buf, sizeof(title_buf), 28, 28, 2);
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

        case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
                {
                        if (!g_show_leaderboards_updates || !event->leaderboard)
                                break;

                        RA_LOG("LEADERBOARD STARTED: [%u] %s",
                                event->leaderboard->id, event->leaderboard->title);

                        const int title_max = 28;
                        char title_buf[32];
                        snprintf(title_buf, title_max + 1, "%s", event->leaderboard->title);
                        if (strlen(event->leaderboard->title) > (size_t)title_max)
                                strcat(title_buf, "...");

                        char buf[NOTIF_TEXT_MAX];
                        snprintf(buf, sizeof(buf), "LEADERBOARD START\n\n%s", title_buf);
                        ra_notify(buf, 2500);
                }
                break;

        case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
                {
                        if (!g_show_leaderboards_updates || !event->leaderboard)
                                break;

                        RA_LOG("LEADERBOARD FAILED: [%u] %s",
                                event->leaderboard->id, event->leaderboard->title);

                        const int title_max = 28;
                        char title_buf[32];
                        snprintf(title_buf, title_max + 1, "%s", event->leaderboard->title);
                        if (strlen(event->leaderboard->title) > (size_t)title_max)
                                strcat(title_buf, "...");

                        char buf[NOTIF_TEXT_MAX];
                        snprintf(buf, sizeof(buf), "LEADERBOARD FAILED\n\n%s", title_buf);
                        ra_notify(buf, 2500);
                }
                break;

        case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		{
			if (!g_show_leaderboards_submission || !event->leaderboard)
                                break;

                        RA_LOG("LEADERBOARD SUBMITTED: [%u] %s",
                                event->leaderboard->id, event->leaderboard->title);

                        const int title_max = 28;
                        char title_buf[32];
                        char value_buf[RC_CLIENT_LEADERBOARD_DISPLAY_SIZE] = "-";
                        snprintf(title_buf, title_max + 1, "%s", event->leaderboard->title);
                        if (strlen(event->leaderboard->title) > (size_t)title_max)
                                strcat(title_buf, "...");

                        if (event->leaderboard->tracker_value && event->leaderboard->tracker_value[0])
                                snprintf(value_buf, sizeof(value_buf), "%s", event->leaderboard->tracker_value);

                        char buf[NOTIF_TEXT_MAX];
                        snprintf(buf, sizeof(buf), "LEADERBOARD SUBMITTED\n\n%s\nScore: %s",
                                title_buf, value_buf);
                        ra_notify_urgent(buf, 3500);
                }
                break;

        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
                {
                        if (!g_show_leaderboards_updates || !event->leaderboard_tracker)
                                break;

                        RA_LOG("LEADERBOARD TRACKER: id=%u value=%s",
                                event->leaderboard_tracker->id,
                                event->leaderboard_tracker->display);

                        char buf[NOTIF_TEXT_MAX];
                        snprintf(buf, sizeof(buf), "LB #%u\n%s",
                                event->leaderboard_tracker->id,
                                event->leaderboard_tracker->display);
                        ra_notify_progress(buf);
                }
                break;

        case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
                {
                        if (!g_show_leaderboards_updates || !event->leaderboard_tracker)
                                break;

                        RA_LOG("LEADERBOARD TRACKER HIDE: id=%u",
                                event->leaderboard_tracker->id);
                }
                break;

        case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
                {
                        if (!g_show_leaderboards_submission || !event->leaderboard_scoreboard)
                                break;

                        RA_LOG("LEADERBOARD SCOREBOARD: id=%u submitted=%s best=%s rank=%u entries=%u",
                                event->leaderboard_scoreboard->leaderboard_id,
                                event->leaderboard_scoreboard->submitted_score,
                                event->leaderboard_scoreboard->best_score,
                                event->leaderboard_scoreboard->new_rank,
                                event->leaderboard_scoreboard->num_entries);

                        char buf[NOTIF_TEXT_MAX];
                        snprintf(buf, sizeof(buf),
                                "LEADERBOARD RESULT\n\nRank: #%u/%u\nSubmitted: %s\nBest: %s",
                                event->leaderboard_scoreboard->new_rank,
                                event->leaderboard_scoreboard->num_entries,
                                event->leaderboard_scoreboard->submitted_score,
                                event->leaderboard_scoreboard->best_score);
                        ra_notify_urgent(buf, 4000);
                }
                break;

        case RC_CLIENT_EVENT_GAME_COMPLETED:
		RA_LOG("*** GAME COMPLETED! ***");
		ra_notify_urgent("** GAME COMPLETED! **\n\nCongratulations!", 5000, 1);
		break;

	case RC_CLIENT_EVENT_RESET:
		// Raised when hardcore is enabled with a game loaded: the runtime
		// stays DISABLED until rc_client_reset is called after the system
		// resets. Defer to the next poll tick — resetting from inside the
		// event handler would re-enter rc_client.
		RA_LOG("EVENT_RESET: runtime requests a system reset (hardcore enabled)");
		g_pending_reset_request = 1;
		break;

	case RC_CLIENT_EVENT_SUBSET_COMPLETED:
		{
			// Per rc_client integration guide: "Completed" in softcore,
			// "Mastered" in hardcore.
			const char *verb = rc_client_get_hardcore_enabled(client) ?
				"MASTERED" : "COMPLETED";
			const char *title = (event->subset && event->subset->title) ?
				event->subset->title : "(unknown subset)";
			RA_LOG("*** SUBSET %s: %s ***", verb, title);
			char title_buf[96];
			ra_format_text(title, title_buf, sizeof(title_buf), 28, 28, 2);
			char buf[NOTIF_TEXT_MAX];
			snprintf(buf, sizeof(buf), "** SUBSET %s! **\n\n%s", verb, title_buf);
			ra_notify_urgent(buf, 5000, 1);
		}
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
		RA_LOG("LOGIN OK: %s (hardcore: %u, softcore: %u)", user->display_name, user->score, user->score_softcore);
		g_logged_in = 1;
		// Login popup is shown at game load time, not here

                // Login now happens on demand during game load. Only identify once mirror is active.
                if (g_rom_md5[0] && !g_game_loaded && !g_game_load_pending) {
                        if (g_mirror_validated) {
                                RA_LOG("Game MD5 available, loading game: %s", g_rom_md5);
                                // Consume the deferred flag: leaving it set made the next
                                // mirror re-validation (e.g. after an OSD reset) trigger a
                                // spurious full game reload.
                                g_game_load_deferred = 0;
                                g_game_load_pending = 1;
                                rc_client_begin_load_game(g_client, g_rom_md5,
                                        ra_load_game_callback, NULL);
                        } else {
                                RA_LOG("Login OK but mirror not validated yet, deferring game identify.");
                                g_game_load_deferred = 1;
                        }
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

		// Multiset (rcheevos 12+): the server decides which subsets are
		// attached to this game/user; rc_client loads them automatically.
		// Log them so it's visible which sets are active.
		{
			rc_client_subset_list_t *subsets = rc_client_create_subset_list(client);
			if (subsets) {
				RA_LOG("  Subsets: %u", subsets->num_subsets);
				for (uint32_t s = 0; s < subsets->num_subsets; s++) {
					const rc_client_subset_t *ss = subsets->subsets[s];
					RA_LOG("    [%u] %s (%u achievements, %u leaderboards)",
						ss->id, ss->title, ss->num_achievements,
						ss->num_leaderboards);
				}
				rc_client_destroy_subset_list(subsets);
			}
		}

		{
			// Single combined popup: game title + achievement count + logged-in user
			char buf[NOTIF_TEXT_MAX];
			// Count achievements per set. LOCK_STATE buckets are per-subset
			// in rcheevos 12 (bucket->subset_id), so multiset games can show
			// the main set's count separately instead of one summed number.
			enum { MAX_SETS_SHOWN = 8 };
			uint32_t per_set[MAX_SETS_SHOWN] = {0};
			uint32_t nsets = 1;
			uint32_t total = 0;
			rc_client_achievement_list_t *list =
				rc_client_create_achievement_list(client,
					RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
					RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
			rc_client_subset_list_t *subsets = rc_client_create_subset_list(client);
			if (subsets && subsets->num_subsets > 1)
				nsets = subsets->num_subsets;
			if (list) {
				for (uint32_t b = 0; b < list->num_buckets; b++) {
					total += list->buckets[b].num_achievements;
					if (nsets > 1) {
						for (uint32_t s = 0; s < nsets && s < MAX_SETS_SHOWN; s++) {
							if (list->buckets[b].subset_id == subsets->subsets[s]->id) {
								per_set[s] += list->buckets[b].num_achievements;
								break;
							}
						}
					}
				}
				rc_client_destroy_achievement_list(list);
			}
			if (subsets)
				rc_client_destroy_subset_list(subsets);
			const rc_client_user_t *user = rc_client_get_user_info(client);
			int hardcore_enabled = rc_client_get_hardcore_enabled(client);
			if (user) {
				snprintf(buf, sizeof(buf),
					"%s\n(HC:%u SC:%u)",
					user->display_name, user->score, user->score_softcore);
				ra_notify_urgent(buf, 2000);
			}
			if (total > 0 && nsets > 1) {
				// e.g. "93 achievements\n+2 sets (84, 10)"
				char sets_buf[48];
				int pos = 0;
				for (uint32_t s = 1; s < nsets && s < MAX_SETS_SHOWN; s++) {
					pos += snprintf(sets_buf + pos, sizeof(sets_buf) - pos,
						"%s%u", (s > 1) ? ", " : "", per_set[s]);
					if (pos >= (int)sizeof(sets_buf) - 1) break;
				}
				snprintf(buf, sizeof(buf),
					"%s\n%u achievements\n+%u sets (%s)",
					game->title, per_set[0], nsets - 1, sets_buf);
			} else if (total > 0) {
				snprintf(buf, sizeof(buf),
					"%s\n%u achievements",
					game->title, total);
			} else {
				snprintf(buf, sizeof(buf),
					"%s\n No achievements",
					game->title);
			}
			ra_notify_urgent(buf, 2000);
			
			hardcore_enabled = rc_client_get_hardcore_enabled(client);
			if (hardcore_enabled) {
				RA_LOG("HARDCORE mode ENABLED!");		
				ra_notify_urgent("HARDCORE mode ENABLED!", 2000);
			}
		}
	} else {
		RA_LOG("GAME LOAD FAILED: result=%d error=%s", result,
			error_message ? error_message : "(none)");
		if (result == RC_NO_GAME_LOADED) {
			RA_LOG("This ROM is not in the RetroAchievements database.");
			// Generic warning for every console: the loaded dump's hash is
			// not in the RA database (bad dump, ROM hack or unsupported
			// region/revision) — achievements will not track.
			ra_notify_urgent("RetroAchievements\n\n"
				"Game not recognized:\n"
				"this dump is not in the\n"
				"RA database", 4000);
		}
	}
}

#endif // HAS_RCHEEVOS

// ---------------------------------------------------------------------------
// Core identification
// ---------------------------------------------------------------------------

// Returns the rcheevos console ID for the current handler, or 0 if unsupported.
static int ra_get_console_id(void)
{
	return g_active_handler ? g_active_handler->console_id : 0;
}

// Returns 1 if the current core is supported for RA
static int ra_core_supported(void)
{
	return g_active_handler != NULL;
}

#ifdef HAS_RCHEEVOS
static int ra_has_internet_connectivity(void)
{
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        int connected = 0;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo("retroachievements.org", "80", &hints, &res) != 0 || !res) {
                RA_LOG("Internet check failed: DNS resolution for retroachievements.org");
                return 0;
        }

        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
                int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (fd < 0)
                        continue;

                int flags = fcntl(fd, F_GETFL, 0);
                if (flags >= 0)
                        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

                int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
                if (rc == 0) {
                        connected = 1;
                        close(fd);
                        break;
                }

                if (rc < 0 && errno == EINPROGRESS) {
                        fd_set wfds;
                        FD_ZERO(&wfds);
                        FD_SET(fd, &wfds);
                        struct timeval tv;
                        tv.tv_sec = 0;
                        tv.tv_usec = 500000;

                        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
                        if (sel > 0 && FD_ISSET(fd, &wfds)) {
                                int so_error = 0;
                                socklen_t slen = sizeof(so_error);
                                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen) == 0 && so_error == 0)
                                        connected = 1;
                        }
                }

                close(fd);
                if (connected)
                        break;
        }

        freeaddrinfo(res);
        return connected;
}

static void ra_show_no_internet_popup(void)
{
        ra_notify("RetroAchievements\n\nNo internet detected!\nTry again once you connected", 3000);
}
#endif

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
	RA_LOG("Build: SelAddr v29-b1 (2026-04-19)");
	RA_LOG("Phase 5 — Handler dispatch: all per-console logic in separate files");

	const char *core = user_io_get_core_name(1);
	g_active_handler = core ? get_console_handler_by_name(core) : NULL;
	RA_LOG("Core: '%s' -> handler=%s console_id=%d", core ? core : "(null)",
		g_active_handler ? g_active_handler->name : "none",
		ra_get_console_id());

	if (!g_active_handler) {
		RA_LOG("Core not supported for RetroAchievements. Inactive.");
		return;
	}

	// Call handler init — N64 sets DDRAM base here, must happen before ra_ramread_map()
	g_active_handler->init();

	// Apply the correct hardcore FPGA bits at core init. Must be symmetric: when
	// hardcore is inactive we clear the bits, otherwise stale restrictions (e.g. a
	// status bit restored from the core config) keep restore-state blocked.
	if (g_active_handler->set_hardcore) {
		int hc = achievements_hardcore_active();
		g_active_handler->set_hardcore(hc);
		RA_LOG("Hardcore: FPGA bits %s at core init for %s", hc ? "applied" : "cleared", g_active_handler->name);
	}

#ifdef HAS_RCHEEVOS
	// Initialize rcheevos hash infrastructure (needed for disc-based consoles)
	ra_cdreader_chd_register(); // unified reader: CHD + cue/gdi fallback
	rc_hash_init_custom_filereader(NULL); // use default stdio-based reader
	rc_hash_init_error_message_callback(ra_hash_message);
	rc_hash_init_verbose_message_callback(ra_hash_message);
#endif

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

	// Config is parsed now, so honour async_log (until here logging was
	// synchronous — and with debug defaulting to 0, silent).
	ra_log_apply_config();

#ifdef HAS_RCHEEVOS
	// Create rc_client
	g_client = rc_client_create(ra_read_memory, ra_server_call);
	if (!g_client) {
		RA_LOG("ERROR: rc_client_create() failed");
		return;
	}

	rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, ra_log_callback);
	rc_client_set_event_handler(g_client, ra_event_handler);
	int hardcore_active = achievements_hardcore_active();
	rc_client_set_hardcore_enabled(g_client, hardcore_active ? 1 : 0);
	RA_LOG("Hardcore mode: %s", hardcore_active ? "ENABLED" : "disabled");

	// Configure User-Agent: "MiSTer/1.0 rcheevos/x.y.z" (updated per-core in achievements_load_game)
	{
		rc_client_get_user_agent_clause(g_client, g_ua_clause, sizeof(g_ua_clause));
		char ua[128];
		snprintf(ua, sizeof(ua), "MiSTer/1.0 %s", g_ua_clause);
		ra_http_set_user_agent(ua);
		RA_LOG("User-Agent: %s", ua);
	}

	RA_LOG("rc_client created successfully");

	// Login is attempted on-demand in achievements_load_game, right before identify.
        g_has_credentials = has_creds;
        if (g_has_credentials) {
                RA_LOG("Credentials loaded. Login will happen on demand before game identify.");
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

static void ra_update_user_agent(void)
{
        if (g_ua_clause[0]) {
                const char *core_name = user_io_get_core_name(1);
                char ua[128];
                if (core_name && core_name[0])
                        snprintf(ua, sizeof(ua), "%s_MiSTer/%s %s", core_name, g_fpga_core_version, g_ua_clause);
                else
                        snprintf(ua, sizeof(ua), "MiSTer/%s %s", g_fpga_core_version, g_ua_clause);
                ra_http_set_user_agent(ua);
                RA_LOG("User-Agent updated: %s", ua);
        }
}

void achievements_load_game(const char *rom_path, uint32_t crc32)
{
        if (!g_active_handler) return;

        // Virtual Boy: the OSD offers auxiliary loads without the config-store
        // flag (VBT brightness tables, TAS movies) that also land here. Only a
        // .vb file is a game — ignore everything else so an aux load cannot
        // rehash and drop the active session.
        if (g_active_handler->console_id == 28 && rom_path && rom_path[0]) {
                size_t len = strlen(rom_path);
                if (len < 3 || strcasecmp(rom_path + len - 3, ".vb") != 0) {
                        RA_LOG("VirtualBoy: non-ROM load '%s' ignored (session kept)", rom_path);
                        return;
                }
        }

        ra_update_user_agent();

        RA_LOG("--- Game Load ---");
        RA_LOG("ROM path: %s", rom_path);
        RA_LOG("CRC32: %08X", crc32);

#ifdef HAS_RCHEEVOS
        // Switching game without a core restart: drop the previous rc_client
        // session (also aborts an in-flight async load) before loading the new
        // one, so unlock state/deltas from the old game can't leak into it.
        if (g_client && (g_game_loaded || g_game_load_pending)) {
                RA_LOG("Previous game session active -- unloading before new load");
                rc_client_unload_game(g_client);
                g_game_loaded = 0;
                g_game_load_pending = 0;
        }
#endif

        // Store ROM path
        snprintf(g_rom_path, sizeof(g_rom_path), "%s", rom_path);

        // Switch to FDS handler if we are NES and the ROM is an FDS file
        if (g_active_handler && (g_active_handler->console_id == 7 || g_active_handler->console_id == 81)) {
                size_t len = strlen(rom_path);
                if (len >= 4 && strcasecmp(rom_path + len - 4, ".fds") == 0) {
                        extern const console_handler_t g_console_fds;
                        g_active_handler = &g_console_fds;
                        RA_LOG("FDS ROM detected, switching handler to Famicom Disk System (ID 81)");
                } else {
                        extern const console_handler_t g_console_nes;
                        g_active_handler = &g_console_nes;
                }
        }

        // Shared ATARI7800 core runs both consoles: .a78 ROMs use the 7800
        // handler (4KB RAM mirror, ID 51), everything else the 2600 one (RIOT).
        if (g_active_handler && (g_active_handler->console_id == 25 || g_active_handler->console_id == 51)) {
                size_t len = strlen(rom_path);
                if (len >= 4 && strcasecmp(rom_path + len - 4, ".a78") == 0) {
                        g_active_handler = &g_console_atari7800;
                        RA_LOG("A78 ROM detected, switching handler to Atari 7800 (ID 51)");
                } else {
                        g_active_handler = &g_console_atari2600;
                }
        }

        // Calculate hash — try handler first, fall back to generic MD5
        g_rom_md5[0] = '\0';
        if (rom_path && rom_path[0]) {
                if (!g_active_handler->calculate_hash(rom_path, g_rom_md5)) {
                        ra_calculate_rom_md5(rom_path, g_rom_md5);
                }
        }

        // Reset frame tracking and console state
        g_last_frame = 0;
        g_first_frame = 0;
        g_mirror_validated = 0;
        g_mirror_confirming = 0;
        g_mirror_initial_frame = 0;
        g_game_load_deferred = 0;
        g_frames_processed = 0;
        g_frames_skipped = 0;
        g_game_loaded = 0;
        g_load_time = time(NULL);
        g_ach_state_count = 0;
        g_active_handler->reset();
        ra_snes_addrlist_init();

        // RetroAchievements safety: zero the DDRAM mirror data area so any leftover
        // bytes from the previous game cannot be fed to rcheevos before the FPGA
        // refreshes the mirror. The header (magic/frame/region descriptors) is
        // preserved so mirror validation logic continues to work; only the data
        // payload (offset 0x100 onwards) and the realtime-query mailbox are cleared.
        if (g_ra_map) {
                uint8_t *base = (uint8_t *)g_ra_map;
                memset(base + 0x100, 0, RA_DDRAM_MAP_SIZE - 0x100);
                RA_LOG("DDRAM mirror data area zeroed at game load");
        }

        // Check mirror state
        if (g_ra_map && ra_ramread_active(g_ra_map)) {
                RA_LOG("Mirror active at game load time. Dumping:");
                ra_ramread_debug_dump(g_ra_map);
        }

#ifdef HAS_RCHEEVOS
        if (g_client && g_rom_md5[0]) {
                if (!g_logged_in && !g_login_pending && g_has_credentials) {
                        if (!ra_has_internet_connectivity()) {
                                RA_LOG("No internet connectivity, skipping RA login for now.");
                                ra_show_no_internet_popup();
                                g_login_deferred = 1;
                        } else {
                                RA_LOG("Starting RA login for '%s' before game identify.", g_ra_user);
                                g_login_deferred = 0;
                                g_login_pending = 1;
                                g_game_load_deferred = 1;
                                rc_client_begin_login_with_password(g_client, g_ra_user, g_ra_password,
                                        ra_login_callback, NULL);
                        }
                } else if (g_logged_in && !g_game_load_pending) {
                        if (g_ra_map && ra_ramread_active(g_ra_map)) {
                                // Already logged in and FPGA mirror active — load immediately
                                RA_LOG("Logged in and FPGA mirror active, loading game by MD5: %s", g_rom_md5);
                                g_game_load_pending = 1;
                                rc_client_begin_load_game(g_client, g_rom_md5,
                                        ra_load_game_callback, NULL);
                        } else {
                                // Mirror not yet active — defer until FPGA validated
                                RA_LOG("Logged in but FPGA mirror not active — game load deferred.");
                                g_game_load_deferred = 1;
                        }
                } else if (g_login_pending || g_login_deferred) {
                        // Login still in progress or deferred — game loads after login
                        RA_LOG("Login pending/deferred, game will load when login completes.");
                } else {
                        RA_LOG("Not logged in — game identified but achievements unavailable.");
                        RA_LOG("MD5: %s (can verify at retroachievements.org)", g_rom_md5);
                }
        }
#endif

        RA_LOG("--- Game Load Complete, monitoring frames ---");

        // Hardcore mode: let handler set console-specific FPGA bits (symmetric:
        // clear them when hardcore is inactive so restore-state is re-enabled).
        if (g_active_handler->set_hardcore) {
                int hc = achievements_hardcore_active();
                g_active_handler->set_hardcore(hc);
                RA_LOG("Hardcore: FPGA bits %s for %s", hc ? "applied" : "cleared", g_active_handler->name);
        }
}

// ---------------------------------------------------------------------------
// Save I/O guard
//
// While the core streams its save RAM to/from the ARM (autosave triggered by
// opening the OSD, manual save/load backup), cores that share the save port
// with the RA read path serve the SD transfer address instead of the requested
// one — the SNES routes both through BSRAM Port B (bk_state mux), so every
// BSRAM/BWRAM read returns a byte from wherever the transfer pointer is.
// Evaluating rcheevos against those values fires delta conditions spuriously
// (confirmed: SMRPG unlocks on OSD-open with autosave enabled).
//
// The guard suspends achievement evaluation from the first notification until
// RA_SAVE_IO_GRACE_MS after the last one. The game keeps running; memrefs keep
// their pre-save values, so deltas resume cleanly against real data. The OSD
// open itself also arms the guard (see OsdEnable) to close the sub-millisecond
// race between bk_state rising and the first serviced sector.
// ---------------------------------------------------------------------------
#define RA_SAVE_IO_GRACE_MS 700

static struct timespec g_save_io_last = {0, 0};
static int g_save_io_paused = 0; // evaluation currently suspended (for logs)

void achievements_notify_save_io(void)
{
	clock_gettime(CLOCK_MONOTONIC, &g_save_io_last);
}

static int ra_save_io_active(void)
{
	if (!g_save_io_last.tv_sec) return 0;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double ms = (now.tv_sec - g_save_io_last.tv_sec) * 1000.0
	          + (now.tv_nsec - g_save_io_last.tv_nsec) / 1e6;
	return ms < RA_SAVE_IO_GRACE_MS;
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

	// RC_CLIENT_EVENT_RESET: the runtime asked for a system reset (hardcore
	// enabled with a game loaded) and stays disabled until rc_client_reset
	// runs. Pulse the conventional reset bit (status[0] on every RA-supported
	// console core) and go through the same path as an OSD reset.
	if (g_pending_reset_request) {
		g_pending_reset_request = 0;
		RA_LOG("Applying runtime reset request: pulsing core reset");
		ra_notify_urgent("HARDCORE enabled\n\nResetting game...", 2500);
		user_io_status_set("[0]", 1);
		user_io_status_set("[0]", 0);
		achievements_notify_core_reset(); // calls rc_client_reset -> re-enables runtime
	}

	if (!g_ra_map) return;

	// Check if mirror has become active
	if (!g_mirror_validated) {
		if (ra_ramread_active(g_ra_map)) {
			uint32_t cur_frame = ra_ramread_frame(g_ra_map);
			if (!g_mirror_confirming) {
				// First time we see the magic — record frame, wait for it to advance
				g_mirror_confirming = 1;
				g_mirror_initial_frame = cur_frame;
				RA_LOG("DDRAM magic detected (frame=%u), waiting for frame to advance...", cur_frame);
			} else if (cur_frame != g_mirror_initial_frame) {
				// Frame advanced — FPGA is alive and adapted
				g_mirror_confirming = 0;
				g_mirror_validated = 1;
				RA_LOG("=== DDRAM Mirror Activated! (frame %u -> %u) ===", g_mirror_initial_frame, cur_frame);
				ra_ramread_debug_dump(g_ra_map);

				// Detect FPGA protocol — returns 1 if adapted, 0 if not supported
				int fpga_ok = 1;
				if (g_active_handler && g_active_handler->detect_protocol)
					fpga_ok = g_active_handler->detect_protocol(g_ra_map);

				if (!fpga_ok) {
					RA_LOG("FPGA core not adapted for RA — suppressing login/load.");
					g_login_deferred = 0;
					g_game_load_deferred = 0;
					g_active_handler = NULL;
					return;
				}

#ifdef HAS_RCHEEVOS
				// Read FPGA core version from DDRAM header for User-Agent reporting
				{
					uint8_t vmaj = 0, vmin = 0;
					if (ra_ramread_get_core_version(g_ra_map, &vmaj, &vmin))
						snprintf(g_fpga_core_version, sizeof(g_fpga_core_version), "%u.%u", vmaj, vmin);
					else
						snprintf(g_fpga_core_version, sizeof(g_fpga_core_version), "0.1");
					RA_LOG("FPGA core version: %s", g_fpga_core_version);
					ra_update_user_agent();
				}

                                // Trigger deferred game load (mirror activated, already logged in)
				if (g_game_load_deferred && g_logged_in && g_rom_md5[0]
						&& !g_game_load_pending) {
					g_game_load_deferred = 0;
					RA_LOG("FPGA validated — loading deferred game by MD5: %s", g_rom_md5);
					// A (re)load is in flight: gate handler polls off until the
					// callback confirms, so rc_client_do_frame is not hammered
					// against a client whose game is mid-load.
					g_game_loaded = 0;
					g_game_load_pending = 1;
					rc_client_begin_load_game(g_client, g_rom_md5,
						ra_load_game_callback, NULL);
				}
				else if (g_game_loaded) {
					// Core was reset with the game still loaded (OSD reset):
					// achievements keep tracking — tell the user so the absence
					// of the load popups is not mistaken for RA being off.
					ra_notify("RetroAchievements\ntracking resumed", 2000);
				}
#endif
			}
			// else: frame still frozen — stale DDRAM from previous session, keep waiting
		} else {
			// Magic disappeared — reset confirming state
			g_mirror_confirming = 0;
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

	// Periodic diagnostics
	if ((poll_calls % 18000) == 1) {
		RA_LOG("DIAG: poll=%u frame=%u last=%u busy=%d processed=%u skipped=%u loaded=%d handler=%s addrs=%d",
			poll_calls, frame, g_last_frame, busy, g_frames_processed, g_frames_skipped,
			g_game_loaded, g_active_handler ? g_active_handler->name : "none",
			ra_snes_addrlist_count());
	}

	(void)busy;

	// Save I/O guard: suspend evaluation while save RAM streams between the
	// core and the ARM (see achievements_notify_save_io). Everything above
	// (HTTP pump, OSD popups, mirror validation) keeps running.
	if (ra_save_io_active()) {
		if (!g_save_io_paused && g_game_loaded) {
			g_save_io_paused = 1;
			RA_LOG("Save I/O detected — achievement evaluation paused");
		}
		return;
	}
	if (g_save_io_paused) {
		g_save_io_paused = 0;
		if (g_game_loaded)
			RA_LOG("Save I/O finished — achievement evaluation resumed");
	}

#ifdef HAS_RCHEEVOS
	// Dispatch to active console handler (handles all Selective Address consoles)
	if (g_active_handler && g_active_handler->poll) {
		if (g_active_handler->poll(g_ra_map, g_client, g_game_loaded))
			return;
	}

	// --- removed per-console poll blocks (now handled by handler->poll()) ---
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
	// Only log if delta is in a plausible range (≤1000 missed frames),
	// to avoid spam when the counter is garbage/oscillating.
	uint32_t delta = frame - g_last_frame;
	if (delta > 1 && g_last_frame > 0 && delta <= 1000) {
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
        if (!g_active_handler) return;

        RA_LOG("--- Game Unload ---");
        RA_LOG("Stats: %u frames processed, %u skipped", g_frames_processed, g_frames_skipped);

#ifdef HAS_RCHEEVOS
        if (g_client) {
                rc_client_unload_game(g_client);
        }
#endif

        g_active_handler->reset();
        ra_snes_addrlist_init();

        // Disable FPGA query mailbox polling — no game loaded, no need to poll.
        // FPGA will stop after next VBlank (reads RA_ARM_CONFIG_OFFSET once per cycle).
        if (g_ra_map) ra_rtquery_disable(g_ra_map);

        // RetroAchievements safety: clear the DDRAM mirror data area so the next
        // game does not see stale bytes from the unloaded game. The header is
        // preserved; only the payload (offset 0x100+) is wiped.
        if (g_ra_map) {
                uint8_t *base = (uint8_t *)g_ra_map;
                memset(base + 0x100, 0, RA_DDRAM_MAP_SIZE - 0x100);
                RA_LOG("DDRAM mirror data area zeroed at game unload");
        }

        g_game_loaded = 0;
        g_game_load_pending = 0;
        g_game_load_deferred = 0;
        g_last_frame = 0;
        g_mirror_validated = 0;
        g_mirror_confirming = 0;
        g_mirror_initial_frame = 0;
        g_login_deferred = 0;
        g_rom_md5[0] = 0;
        g_rom_path[0] = 0;

        // Clear pending notifications
        s_urgent_head = s_urgent_tail = 0;
        s_urgent_showing  = 0;
        s_instant_pending = 0;
        s_instant_showing = 0;
}

void achievements_notify_core_reset(void)
{
        if (!g_active_handler) return;

        RA_LOG("--- Core Reset ---");

#ifdef HAS_RCHEEVOS
        if (g_client && g_game_loaded) {
                rc_client_reset(g_client);
                RA_LOG("rc_client_reset notified");
        }
#endif

        // Keep the loaded game, but restart runtime/frame tracking state.
        g_last_frame = 0;
        g_first_frame = 0;
        g_frames_processed = 0;
        g_frames_skipped = 0;
        g_load_time = time(NULL);
        g_ach_state_count = 0;

        g_active_handler->reset();
        ra_snes_addrlist_init();

        // Drop stale queued notifications across reset boundaries.
        s_urgent_head = s_urgent_tail = 0;
        s_urgent_showing  = 0;
        s_instant_pending = 0;
        s_instant_showing = 0;

        // Force mirror to re-validate so we don't poll stale DDRAM while the FPGA is in reset
        g_mirror_validated = 0;
        g_mirror_confirming = 0;

        // Clear DDRAM payload so no old RAM is processed during the transition
        if (g_ra_map) {
                uint8_t *base = (uint8_t *)g_ra_map;
                memset(base + 0x100, 0, RA_DDRAM_MAP_SIZE - 0x100);
        }

}

void achievements_notify_state_loaded(void)
{
        if (!g_active_handler) return;

#ifdef HAS_RCHEEVOS
        if (!g_client || !g_game_loaded) return;

        // Deliberately NOT rc_client_reset(). A savestate load is not a reset:
        //   - rc_client_reset() clears game->waiting_for_reset, the gate that
        //     holds processing after hardcore is switched on until a genuine
        //     reset happens. Letting a state load satisfy it would open exactly
        //     the hole that gate exists to close.
        //   - rc_client_reset() also unloads the game when the media hash no
        //     longer matches, which has nothing to do with restoring a state.
        //
        // rc_client_deserialize_progress(NULL) is the path the library provides
        // for "this savestate carries no achievement data": it runs the subset
        // before/after hooks (needed for v12 multiset), hides the progress
        // tracker, and resets the runtime — so every trigger goes back to
        // WAITING and no delta spans the load boundary.
        int res = rc_client_deserialize_progress(g_client, NULL);
        RA_LOG("--- State Loaded --- runtime reset via deserialize_progress(NULL), res=%d", res);

        // The addresses being watched do not change across a state load, only
        // their values, so the SelAddr list stays valid and is left alone.
#endif
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
	// Reset auth state — client destroyed, so login is no longer valid
	g_logged_in = 0;
	g_login_pending = 0;
	g_login_deferred = 0;
#endif

	ra_http_deinit();

	if (g_ra_map) {
		// Clear DDRAM magic before unmapping so the next core starts clean
		uint32_t *magic_ptr = (uint32_t *)g_ra_map;
		*magic_ptr = 0;
		RA_LOG("DDRAM magic cleared");
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
	if (g_force_hardcore) return 1;
	return g_hardcore && g_active_handler && g_active_handler->hardcore_protected;
}

int achievements_stall_recovery_enabled(void)
{
	return g_stall_recovery;
}

int achievements_rtquery_enabled(void)
{
	return g_rtquery_enabled;
}

int achievements_gba_reset_ram(void)
{
	return g_gba_reset_ram;
}

int achievements_recollect_interval(void)
{
	return g_recollect_interval;
}

int achievements_smart_cache_enabled(void)
{
	if (g_smart_cache != -1) return g_smart_cache;

#ifdef HAS_RCHEEVOS
	int cid = ra_get_console_id();
	// All Selective Address cores now support the RTQuery mailbox (SMS/Game
	// Gear gained FPGA-side realtime-query support, ra_ram_mirror_sms v0x02).
	if (cid == RC_CONSOLE_PLAYSTATION || cid == RC_CONSOLE_NINTENDO || cid == RC_CONSOLE_MEGA_DRIVE ||
	    cid == RC_CONSOLE_SUPER_NINTENDO || cid == RC_CONSOLE_GAMEBOY_ADVANCE ||
	    cid == RC_CONSOLE_GAMEBOY || cid == RC_CONSOLE_GAMEBOY_COLOR ||
	    cid == RC_CONSOLE_SEGA_CD || cid == RC_CONSOLE_SEGA_32X ||
	    cid == RC_CONSOLE_MASTER_SYSTEM || cid == RC_CONSOLE_GAME_GEAR ||
	    cid == RC_CONSOLE_PC_ENGINE || cid == RC_CONSOLE_PC_ENGINE_CD ||
	    cid == RC_CONSOLE_ARCADE || cid == RC_CONSOLE_NEO_GEO_CD ||
	    cid == RC_CONSOLE_VIRTUAL_BOY) {
		return 1;
	}
#endif

	return 0;
}

int achievements_n64_snapshot_enabled(void)
{
	return g_n64_snapshot;
}

int achievements_watch_list(const uint32_t **addrs)
{
	if (addrs) *addrs = g_watch_addrs;
	return g_watch_count;
}

int achievements_smart_cleanup_enabled(void)
{
	return g_smart_cleanup;
}

int achievements_trigger_dump(void)
{
	// Gated on debug as well: without logging the window could never be
	// printed, so keeping it would only cost a DDRAM read every frame.
	return g_ra_debug && g_trigger_dump;
}

int achievements_justifier_test(void)
{
	return g_justifier_test;
}

void achievements_info(void)
{
	if (!ra_core_supported()) {
                InfoAligned("RetroAchievements\n\nCore not supported", 2000, g_popup_pos, 1);
                return;
        }

#ifdef HAS_RCHEEVOS
        if (!ra_has_internet_connectivity()) {
                InfoAligned("RetroAchievements\n\nSem internet\nConecte a rede para o RA funcionar", 2500, g_popup_pos, 1);
                return;
        }
#endif

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

	InfoAligned(buf, 4000, g_popup_pos, 1);
}

// ---------------------------------------------------------------------------
// Achievement list view (F6 shortcut)
// ---------------------------------------------------------------------------

#ifdef HAS_RCHEEVOS
struct AchViewItem {
	bool is_header;
	bool is_subline;
	const char *text;
	const rc_client_achievement_t *ach;
};
static rc_client_achievement_list_t *g_ach_view_list = nullptr;
static AchViewItem *g_ach_view_items = nullptr;
static rc_client_subset_list_t *g_ach_view_subsets = nullptr;
#endif
static int g_ach_view_first    = 0;
static int g_ach_view_selected = 0;
static int g_ach_view_total    = 0;
static int g_ach_view_set_idx  = 0;

// Number of achievement sets (subsets) of the loaded game. Multiset games
// (rcheevos 12+) get one list page per set, switched with Left/Right.
static int ra_ach_view_num_sets(void)
{
#ifdef HAS_RCHEEVOS
	if (g_ach_view_subsets) return (int)g_ach_view_subsets->num_subsets;
#endif
	return 1;
}

// OSD rows available for list items. The set-selector footer takes one row
// when the game has more than one set; the description ticker takes the
// bottom row when the ticker option is enabled.
static int ra_ach_view_rows(void)
{
	int rows = OsdGetSize();
	if (ra_ach_view_num_sets() > 1) rows--;
	if (g_desc_ticker) rows--;
	return rows;
}

#ifdef HAS_RCHEEVOS
// The achievement highlighted in the list view, or nullptr when the current
// row is a section header / progress sub-line or the list is not built.
static const rc_client_achievement_t *ra_list_selected_ach(void)
{
	int idx = g_ach_view_selected;
	if (!g_ach_view_items || idx < 0 || idx >= g_ach_view_total) return nullptr;
	const AchViewItem &it = g_ach_view_items[idx];
	if (it.is_header || it.is_subline || !it.ach) return nullptr;
	return it.ach;
}
#endif

int achievements_has_active_game(void)
{
#ifdef HAS_RCHEEVOS
	return g_logged_in && g_game_loaded;
#else
	return 0;
#endif
}

#ifdef HAS_RCHEEVOS
// (Re)build the flattened item array for the currently selected set.
// In multiset games the LOCK_STATE buckets are per-subset (bucket->subset_id
// is non-zero); only buckets of the selected set are taken.
static void ra_ach_view_build(void)
{
	if (g_ach_view_items) {
		delete[] g_ach_view_items;
		g_ach_view_items = nullptr;
	}
	g_ach_view_total    = 0;
	g_ach_view_first    = 0;
	g_ach_view_selected = 0;

	if (!g_ach_view_list)
		return;

	const uint32_t cur_set_id = (ra_ach_view_num_sets() > 1) ?
		g_ach_view_subsets->subsets[g_ach_view_set_idx]->id : 0;

	uint32_t total_ach = 0;
	for (uint32_t b = 0; b < g_ach_view_list->num_buckets; b++) {
		if (g_ach_view_list->buckets[b].subset_id != cur_set_id) continue;
		total_ach += g_ach_view_list->buckets[b].num_achievements;
	}

	const rc_client_achievement_t** achs_active = new const rc_client_achievement_t*[total_ach];
	const rc_client_achievement_t** achs_prog = new const rc_client_achievement_t*[total_ach];
	const rc_client_achievement_t** achs_locked = new const rc_client_achievement_t*[total_ach];
	const rc_client_achievement_t** achs_unlocked = new const rc_client_achievement_t*[total_ach];
	int n_active = 0, n_prog = 0, n_locked = 0, n_unlocked = 0;

	for (uint32_t b = 0; b < g_ach_view_list->num_buckets; b++) {
		if (g_ach_view_list->buckets[b].subset_id != cur_set_id) continue;
		for (uint32_t a = 0; a < g_ach_view_list->buckets[b].num_achievements; a++) {
			const rc_client_achievement_t* ach = g_ach_view_list->buckets[b].achievements[a];
			if (ach->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED) {
				achs_unlocked[n_unlocked++] = ach;
			} else if (ach->bucket == RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE) {
				achs_active[n_active++] = ach;
			} else if (ach->measured_progress[0] != '\0') {
				achs_prog[n_prog++] = ach;
			} else {
				achs_locked[n_locked++] = ach;
			}
		}
	}

	uint32_t total_items = 0;
	if (n_active > 0) total_items += 1 + n_active;
	if (n_prog > 0) total_items += 1 + (n_prog * 2);
	if (n_locked > 0) total_items += 1 + n_locked;
	if (n_unlocked > 0) total_items += 1 + n_unlocked;

	g_ach_view_items = new AchViewItem[total_items];
	int idx = 0;

	if (n_active > 0) {
		g_ach_view_items[idx].is_header = true;
		g_ach_view_items[idx].is_subline = false;
		g_ach_view_items[idx].text = "Active Challenges";
		g_ach_view_items[idx].ach = nullptr;
		idx++;
		for (int i = 0; i < n_active; i++) {
			g_ach_view_items[idx].is_header = false;
			g_ach_view_items[idx].is_subline = false;
			g_ach_view_items[idx].text = nullptr;
			g_ach_view_items[idx].ach = achs_active[i];
			idx++;
		}
	}
	if (n_prog > 0) {
		g_ach_view_items[idx].is_header = true;
		g_ach_view_items[idx].is_subline = false;
		g_ach_view_items[idx].text = "In Progress";
		g_ach_view_items[idx].ach = nullptr;
		idx++;
		for (int i = 0; i < n_prog; i++) {
			g_ach_view_items[idx].is_header = false;
			g_ach_view_items[idx].is_subline = false;
			g_ach_view_items[idx].text = nullptr;
			g_ach_view_items[idx].ach = achs_prog[i];
			idx++;
			
			g_ach_view_items[idx].is_header = false;
			g_ach_view_items[idx].is_subline = true;
			g_ach_view_items[idx].text = nullptr;
			g_ach_view_items[idx].ach = achs_prog[i];
			idx++;
		}
	}
	if (n_locked > 0) {
		g_ach_view_items[idx].is_header = true;
		g_ach_view_items[idx].is_subline = false;
		g_ach_view_items[idx].text = "Locked";
		g_ach_view_items[idx].ach = nullptr;
		idx++;
		for (int i = 0; i < n_locked; i++) {
			g_ach_view_items[idx].is_header = false;
			g_ach_view_items[idx].is_subline = false;
			g_ach_view_items[idx].text = nullptr;
			g_ach_view_items[idx].ach = achs_locked[i];
			idx++;
		}
	}
	if (n_unlocked > 0) {
		g_ach_view_items[idx].is_header = true;
		g_ach_view_items[idx].is_subline = false;
		g_ach_view_items[idx].text = "Unlocked";
		g_ach_view_items[idx].ach = nullptr;
		idx++;
		for (int i = 0; i < n_unlocked; i++) {
			g_ach_view_items[idx].is_header = false;
			g_ach_view_items[idx].is_subline = false;
			g_ach_view_items[idx].text = nullptr;
			g_ach_view_items[idx].ach = achs_unlocked[i];
			idx++;
		}
	}

	delete[] achs_active;
	delete[] achs_prog;
	delete[] achs_locked;
	delete[] achs_unlocked;

	g_ach_view_total = (int)total_items;
}
#endif // HAS_RCHEEVOS

int achievements_list_open(void)
{
	achievements_list_close();
#ifdef HAS_RCHEEVOS
	if (!g_client || !g_logged_in || !g_game_loaded)
		return 0;

	// Use LOCK_STATE grouping to get all achievements, we will categorize them manually
	g_ach_view_list = rc_client_create_achievement_list(g_client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
		RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!g_ach_view_list)
		return 0;

	g_ach_view_subsets = rc_client_create_subset_list(g_client);
	g_ach_view_set_idx = 0;
	ra_ach_view_build();
#endif
	return g_ach_view_total;
}

// Switch the list view to the previous/next achievement set (dir = -1/+1).
// Returns 1 if the view changed, 0 for single-set games (caller falls back
// to page scrolling so Left/Right keep their old behavior).
int achievements_list_switch_set(int dir)
{
#ifdef HAS_RCHEEVOS
	int n = ra_ach_view_num_sets();
	if (n <= 1) return 0;
	g_ach_view_set_idx = (g_ach_view_set_idx + (dir < 0 ? n - 1 : 1)) % n;
	ra_ach_view_build();
	return 1;
#else
	(void)dir;
	return 0;
#endif
}

void achievements_list_close(void)
{
#ifdef HAS_RCHEEVOS
	if (g_ach_view_items) {
		delete[] g_ach_view_items;
		g_ach_view_items = nullptr;
	}
	if (g_ach_view_list) {
		rc_client_destroy_achievement_list(g_ach_view_list);
		g_ach_view_list = nullptr;
	}
	if (g_ach_view_subsets) {
		rc_client_destroy_subset_list(g_ach_view_subsets);
		g_ach_view_subsets = nullptr;
	}
#endif
	g_ach_view_first    = 0;
	g_ach_view_selected = 0;
	g_ach_view_total    = 0;
	g_ach_view_set_idx  = 0;
}

int achievements_list_count(void)
{
	return g_ach_view_total;
}

void achievements_list_scan(int mode)
{
	int total    = g_ach_view_total;
	int osd_size = ra_ach_view_rows();
	if (total == 0) return;

	switch (mode) {
		case SCANF_INIT:
			g_ach_view_selected = 0;
			g_ach_view_first    = 0;
			break;
		case SCANF_END:
			g_ach_view_selected = total - 1;
			g_ach_view_first    = total - osd_size;
			if (g_ach_view_first < 0) g_ach_view_first = 0;
			break;
		case SCANF_NEXT:
			if (g_ach_view_selected < total - 1) {
				g_ach_view_selected++;
				if (g_ach_view_selected >= g_ach_view_first + osd_size)
					g_ach_view_first++;
			}
			break;
		case SCANF_PREV:
			if (g_ach_view_selected > 0) {
				g_ach_view_selected--;
				if (g_ach_view_selected < g_ach_view_first)
					g_ach_view_first--;
			}
			break;
		case SCANF_NEXT_PAGE:
			g_ach_view_selected += osd_size;
			if (g_ach_view_selected >= total) g_ach_view_selected = total - 1;
			g_ach_view_first += osd_size;
			if (g_ach_view_first > total - osd_size) g_ach_view_first = total - osd_size;
			if (g_ach_view_first < 0) g_ach_view_first = 0;
			break;
		case SCANF_PREV_PAGE:
			g_ach_view_selected -= osd_size;
			if (g_ach_view_selected < 0) g_ach_view_selected = 0;
			g_ach_view_first -= osd_size;
			if (g_ach_view_first < 0) g_ach_view_first = 0;
			break;
	}
}

void achievements_list_print(void)
{
	static char s[32];
	int total    = g_ach_view_total;
	int osd_size = ra_ach_view_rows();

	for (int i = 0; i < osd_size; i++) {
		int idx      = g_ach_view_first + i;
		char leftchar = 0;

		if (i == 0 && g_ach_view_first > 0)
			leftchar = 17;
		if (i == osd_size - 1 && (g_ach_view_first + osd_size) < total)
			leftchar = 16;

		if (idx < total) {
#ifdef HAS_RCHEEVOS
			if (g_ach_view_items) {
				const AchViewItem &item = g_ach_view_items[idx];
				if (item.is_header) {
					snprintf(s, 30, "--- %s", item.text ? item.text : "");
					s[29] = 0;
				} else if (item.is_subline) {
					snprintf(s, 30, "    \\-> (%s)", item.ach->measured_progress);
					s[29] = 0;
				} else if (item.ach) {
					const rc_client_achievement_t *ach = item.ach;
					bool unlocked = (ach->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);
					s[0] = ' ';
					s[1] = unlocked ? 0x1a : 0x1b;
					s[2] = ' ';
					
					strncpy(s + 3, ach->title, 26);
					s[29] = 0;
					int len = (int)strlen(s);
					if (len > 28) {
						s[28] = 22; // continuation marker (more text follows)
						s[29] = 0;
					}
				} else {
					memset(s, ' ', 29);
					s[29] = 0;
				}
			} else {
				memset(s, ' ', 29);
				s[29] = 0;
			}
#else
			memset(s, ' ', 29);
			s[29] = 0;
#endif
		} else {
			memset(s, ' ', 29);
			s[29] = 0;
		}

		OsdWriteOffset(i, s, (idx == g_ach_view_selected), 0, 0, leftchar);
	}

#ifdef HAS_RCHEEVOS
	// Multiset footer: set name + Left/Right arrows on the last OSD row.
	if (ra_ach_view_num_sets() > 1) {
		const rc_client_subset_t *set =
			g_ach_view_subsets->subsets[g_ach_view_set_idx];
		snprintf(s, 30, "\x11 %d/%d %-21.21s \x10",
			g_ach_view_set_idx + 1, ra_ach_view_num_sets(),
			(set && set->title) ? set->title : "");
		OsdWriteOffset(osd_size, s, 0, 0, 0, 0);
	}

	// Description ticker (retroachievements.cfg: list_desc_ticker). Draw the
	// static first line here; achievements_list_ticker() animates it across the
	// bottom row each UI tick when the text is longer than the row.
	if (g_desc_ticker) {
		const rc_client_achievement_t *ach = ra_list_selected_ach();
		const char *desc = ach ? ach->description : "";
		s[0] = ' ';
		strncpy(s + 1, desc, 30);
		s[31] = 0;
		OsdWriteOffset(OsdGetSize() - 1, s, 0, 0, 0, 0);
		ScrollReset(0);
	}
#endif
}

// Animate the description ticker across the bottom row of the list view. Call
// once per UI tick while the list is shown (no-op unless the ticker option is
// enabled). Mirrors cheats_scroll_name()/recent_scroll_name().
void achievements_list_ticker(void)
{
#ifdef HAS_RCHEEVOS
	if (!g_desc_ticker) return;
	const rc_client_achievement_t *ach = ra_list_selected_ach();
	const char *desc = ach ? ach->description : "";
	char name[512];
	name[0] = ' ';
	snprintf(name + 1, sizeof(name) - 1, "%s", desc);
	// idx 0 = list scroller slot (unused elsewhere in this view); off 1 keeps
	// the leading space fixed while the text scrolls underneath it.
	ScrollText(OsdGetSize() - 1, name, 1, 0, 30, 0, 0);
#endif
}

// 1 if the row highlighted in the list view is a real achievement (not a
// section header or progress sub-line). Gates the detail screen.
int achievements_list_selected_is_ach(void)
{
#ifdef HAS_RCHEEVOS
	return ra_list_selected_ach() != nullptr;
#else
	return 0;
#endif
}

int achievements_desc_ticker_enabled(void) { return g_desc_ticker; }
int achievements_list_hotkey_enabled(void) { return g_list_hotkey; }

#ifdef HAS_RCHEEVOS
// Write a '\n'-separated wrapped string to consecutive OSD rows starting at
// `row`, each prefixed with a leading space. Returns the next free row.
static int ra_osd_write_wrapped(int row, const char *wrapped, int invert)
{
	const char *p = wrapped;
	while (*p && row < OsdGetSize()) {
		char line[40];
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		if (n > sizeof(line) - 2) n = sizeof(line) - 2;
		line[0] = ' ';
		memcpy(line + 1, p, n);
		line[n + 1] = 0;
		OsdWrite(row++, line, invert, 0);
		if (!nl) break;
		p = nl + 1;
	}
	return row;
}
#endif

// Render the full-screen detail view (title, points/state, progress and the
// word-wrapped description) of the achievement highlighted in the list.
void achievements_detail_print(void)
{
#ifdef HAS_RCHEEVOS
	const rc_client_achievement_t *ach = ra_list_selected_ach();
	int size = OsdGetSize();
	for (int i = 0; i < size; i++) OsdWrite(i, "", 0, 0);
	if (!ach) return;

	bool unlocked = (ach->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED);

	// Title (always wrapped, up to 2 lines, highlighted)
	char title_wrap[96];
	ra_wrap_text(ach->title, title_wrap, sizeof(title_wrap), 28, 2);
	int row = ra_osd_write_wrapped(0, title_wrap, 1);

	// Points + state
	char meta[40];
	snprintf(meta, sizeof(meta), " %s%c %u pts",
		unlocked ? "Unlocked " : "Locked ", unlocked ? 0x1a : 0x1b, ach->points);
	OsdWrite(row++, meta, 0, 0);

	if (ach->measured_progress[0]) {
		char prog[40];
		snprintf(prog, sizeof(prog), " Progress: %.27s", ach->measured_progress);
		OsdWrite(row++, prog, 0, 0);
	}

	OsdWrite(row++, "", 0, 0);

	// Description over the remaining rows
	int maxlines = size - row;
	if (maxlines < 1) maxlines = 1;
	char desc_wrap[512];
	ra_wrap_text(ach->description, desc_wrap, sizeof(desc_wrap), 28, maxlines);
	ra_osd_write_wrapped(row, desc_wrap, 0);
#endif
}
