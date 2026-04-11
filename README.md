# Main_MiSTer — RetroAchievements Fork

This is a fork of the official [MiSTer Main binary](https://github.com/MiSTer-devel/Main_MiSTer) with **RetroAchievements** support for MiSTer FPGA.

> **Status:** Experimental / Proof of Concept — currently only the **NES** core is supported.

## What's Different from the Original

The upstream Main_MiSTer binary manages cores, user input, video output, and the OSD menu on the MiSTer platform. This fork adds a full RetroAchievements integration layer on top of that, without modifying any existing core functionality. The following files were **added**:

| File | Purpose |
|------|---------|
| `achievements.cpp / .h` | Lifecycle management — init, load game, per-frame poll, unload, OSD popups |
| `ra_http.cpp / .h` | Async HTTP worker thread that executes RetroAchievements API calls via `curl` |
| `ra_ramread.cpp / .h` | Reads emulated console RAM from the DDRAM mirror written by the FPGA core |
| `shmem.cpp / .h` | Thin wrapper around `/dev/mem` + `mmap` for ARM ↔ FPGA shared memory access |
| `retroachievements.cfg` | User credentials file (username / password) |
| `lib/rcheevos/` | The [rcheevos](https://github.com/RetroAchievements/rcheevos) library (achievement logic, server protocol) |

Minor modifications were made to existing files to hook into the main loop:

- **`scheduler.cpp`** — calls `achievements_poll()` every frame
- **`menu.cpp`** — calls `achievements_load_game()` when a ROM is selected
- **`main.cpp`** — calls `achievements_init()` / `achievements_deinit()` at startup/shutdown
- **`Makefile`** — conditionally compiles rcheevos sources and links against them

No other existing behavior is changed.

## Architecture

The RetroAchievements integration uses a four-layer pipeline:

```
┌─────────────────────────────────────────────────┐
│  FPGA Core (NES_MiSTer)                         │
│  ra_ram_mirror.sv copies emulated RAM to DDRAM   │
│  every VBlank (~60 Hz)                           │
└──────────────────┬──────────────────────────────┘
                   │  DDRAM at physical 0x3D000000
                   ▼
┌─────────────────────────────────────────────────┐
│  ARM Binary (this repo)                          │
│  shmem.cpp  — mmap /dev/mem to read DDRAM        │
│  ra_ramread — parse header, map NES addresses     │
└──────────────────┬──────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────────┐
│  rcheevos SDK                                    │
│  Evaluates achievement conditions against RAM    │
│  Manages unlock state, leaderboards, progress    │
└──────────────────┬──────────────────────────────┘
                   │  Async HTTP (ra_http worker)
                   ▼
┌─────────────────────────────────────────────────┐
│  RetroAchievements Server                        │
│  Login, game identification, unlock reporting    │
└─────────────────────────────────────────────────┘
```

### DDRAM Mirror Layout

The FPGA core writes a structured block at physical address `0x3D000000`:

| Offset | Content |
|--------|---------|
| `0x00` | Magic `0x52414348` ("RACH") |
| `0x04` | Region count + flags (busy bit) |
| `0x08` | Frame counter (increments each VBlank) |
| `0x10+` | Region descriptors (source addr, size, DDRAM offset) |
| `0x100+` | Actual RAM data (8-byte aligned) |

For NES, two regions are exposed:
- **CPU-RAM** ($0000–$07FF) — 2 KB
- **Cart SRAM** ($6000–$7FFF) — 8 KB

### How It Works

1. **On startup**, the binary loads credentials from `retroachievements.cfg`, initializes the rcheevos client, and logs in asynchronously.
2. **When a ROM is loaded**, the binary computes an MD5 hash (skipping the iNES header for NES ROMs) and sends it to the RA server to identify the game and fetch its achievement set.
3. **Every frame**, `achievements_poll()` checks the DDRAM mirror's frame counter. When it advances, it calls `rc_client_do_frame()`, which reads emulated RAM through the mirror and evaluates all active achievement conditions.
4. **When an achievement triggers**, an OSD popup is displayed with the title and description, and an optional sound effect (`/media/fat/achievement.wav`) is played.
5. **Unlocks are reported** to the RA server via the async HTTP worker.

Currently achievements run in **softcore mode** (savestates allowed). Hardcore mode is disabled since there is no anti-tamper mechanism yet.

## How to Try It

1. Download the latest release binaries from the [Releases](https://github.com/odelot/Main_MiSTer/releases) page.
2. Edit `retroachievements.cfg` with your [RetroAchievements](https://retroachievements.org/) credentials:
   ```ini
   username=YourRAUsername
   password=YourRAPassword
   ```
3. Copy **all files** from the release to `/media/fat` on your MiSTer SD card (the `MiSTer` binary, `retroachievements.cfg`, and the `lib/` folder if included).
4. You will also need the **modified NES core** from [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) — see that repo for installation instructions.
5. Reboot your MiSTer, load the NES core, and open a game that has achievements on [retroachievements.org](https://retroachievements.org/).
6. (Optional) Place an `achievement.wav` file in `/media/fat/` to hear a sound effect on unlock.

## Building from Source

Follow the standard MiSTer cross-compilation guide [here](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/#general-prerequisites-for-arm-cross-compiling), then:

```bash
# Clone with submodules (rcheevos)
git clone --recursive https://github.com/odelot/Main_MiSTer.git
cd Main_MiSTer

# If you forgot --recursive:
git submodule update --init --recursive

make
```

The Makefile automatically detects the rcheevos library and enables it if present.

## Links

- Original MiSTer Main binary: [MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer)
- Modified NES core (required): [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
- RetroAchievements: [retroachievements.org](https://retroachievements.org/)
- rcheevos library: [RetroAchievements/rcheevos](https://github.com/RetroAchievements/rcheevos)
