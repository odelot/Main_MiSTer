// achievements_console_lookup.cpp — Console handler lookup table

#include "achievements_console.h"
#include <string.h>
#include <strings.h>

// Console handlers from separate files
extern const console_handler_t g_console_nes;
extern const console_handler_t g_console_snes;
extern const console_handler_t g_console_genesis;
extern const console_handler_t g_console_psx;
extern const console_handler_t g_console_n64;
extern const console_handler_t g_console_gameboy;
extern const console_handler_t g_console_gba;
extern const console_handler_t g_console_sms;
extern const console_handler_t g_console_neogeo;

// Master lookup table
static const console_handler_t *g_console_handlers[] = {
	&g_console_nes,
	&g_console_snes,
	&g_console_genesis,
	&g_console_psx,
	&g_console_n64,
	&g_console_gameboy,
	&g_console_gba,
	&g_console_sms,
	&g_console_neogeo,
	NULL
};

const console_handler_t *get_console_handler_by_name(const char *core_name)
{
	if (!core_name) return NULL;

	// Check exact matches first
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (!strcasecmp(core_name, g_console_handlers[i]->name)) {
			return g_console_handlers[i];
		}
	}

	// Check aliases
	if (!strcasecmp(core_name, "MegaDrive")) {
		return &g_console_genesis;
	}
	if (!strcasecmp(core_name, "GBC")) {
		return &g_console_gameboy;  // GameBoy handler handles both GB and GBC
	}

	return NULL;
}

const console_handler_t *get_console_handler_by_id(int console_id)
{
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (g_console_handlers[i]->console_id == console_id) {
			return g_console_handlers[i];
		}
	}

	// Special cases: GameBoy Color (ID 6) uses GameBoy handler (ID 4)
	if (console_id == 6) {
		return &g_console_gameboy;
	}
	// Game Gear (ID 15) uses SMS handler (ID 11)
	if (console_id == 15) {
		return &g_console_sms;
	}

	return NULL;
}

// Initialize all console handlers
void init_all_console_handlers(void)
{
	for (int i = 0; g_console_handlers[i] != NULL; i++) {
		if (g_console_handlers[i]->init) {
			g_console_handlers[i]->init();
		}
	}
}
