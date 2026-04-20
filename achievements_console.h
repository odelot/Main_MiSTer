#ifndef ACHIEVEMENTS_CONSOLE_H
#define ACHIEVEMENTS_CONSOLE_H

#include <stdint.h>
#include <time.h>

#ifdef HAS_RCHEEVOS
#include "rc_client.h"
#endif

// Common console state structure for Option C consoles
typedef struct {
	int optionc;              // 1 if FPGA has Option C protocol
	int collecting;           // 1 during address collection do_frame
	int cache_ready;          // 1 when FPGA response matches request
	int needs_recollect;      // 1 if re-collection needed (PSX/N64/NeoGeo)
	uint32_t last_resp_frame; // Last processed response frame
	uint32_t game_frames;     // Frames processed since cache active
	uint32_t poll_logged;     // Last game_frames milestone logged
	int resolve_pass;         // Current pointer-resolution pass number
	struct timespec cache_time; // Timestamp when cache became active
} console_state_t;

// Console-specific interface
typedef struct {
	// Initialize console-specific state (called once at startup)
	void (*init)(void);

	// Reset console state (called on load_game)
	void (*reset)(void);

	// Read memory for this console
	uint32_t (*read_memory)(void *map, uint32_t address, uint8_t *buffer, uint32_t num_bytes);

	// Poll achievements for this console (called every frame from achievements_poll).
	// Returns 1 if this console handled the frame (achievements_poll should return immediately).
	// Returns 0 to fall through to the default frame-tracking path (NES, SNES VBlank-gated).
	int (*poll)(void *map, void *client, int game_loaded);

	// Calculate ROM hash (returns 1 on success, 0 to use default MD5)
	int (*calculate_hash)(const char *rom_path, char *md5_hex_out);

	// Set hardcore mode bits (NULL = no FPGA mapping for this console)
	void (*set_hardcore)(int enabled);

	// Detect FPGA protocol type (called when DDRAM mirror first activates)
	void (*detect_protocol)(void *map);

	// Get console ID (RC_CONSOLE_*)
	int console_id;

	// Console name (matches user_io_get_core_name)
	const char *name;
} console_handler_t;

// Console handlers (implemented in separate files)
extern const console_handler_t g_console_nes;
extern const console_handler_t g_console_snes;
extern const console_handler_t g_console_genesis;
extern const console_handler_t g_console_psx;
extern const console_handler_t g_console_n64;
extern const console_handler_t g_console_gameboy;
extern const console_handler_t g_console_sms;
extern const console_handler_t g_console_neogeo;
extern const console_handler_t g_console_gba;
extern const console_handler_t g_console_megacd;
extern const console_handler_t g_console_atari2600;
extern const console_handler_t g_console_tgfx16;

// Get console handler by core name (returns NULL if not found)
const console_handler_t *get_console_handler_by_name(const char *core_name);

// Get console handler by ID (returns NULL if not found)
const console_handler_t *get_console_handler_by_id(int console_id);

// Initialize all console handlers (called once at startup)
void init_all_console_handlers(void);

#endif // ACHIEVEMENTS_CONSOLE_H
