# Main_MiSTer — RetroAchievements Fork

This is a fork of the official [MiSTer Main binary](https://github.com/MiSTer-devel/Main_MiSTer) with **RetroAchievements** support for MiSTer FPGA.

> **Status:** Experimental / Proof of Concept — currently the **NES**, **SNES**, and **PSX** cores are supported.

## Supported Cores

| Core | Console ID | Modified Core Repo |
|------|-----------|--------------------|
| NES | 7 | [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) |
| SNES | 3 | [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer) |
| PSX | 12 | [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer) |

## What's Different from the Original

The upstream Main_MiSTer binary manages cores, user input, video output, and the OSD menu on the MiSTer platform. This fork adds a full RetroAchievements integration layer on top of that, without modifying any existing core functionality. The following files were **added**:

| File | Purpose |
|------|--------|
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
│  FPGA Core (NES / SNES / PSX)                   │
│  ra_ram_mirror*.sv exposes emulated RAM to DDRAM  │
│  every VBlank (~60 Hz)                           │
└──────────────────┬──────────────────────────────┘
                   │  DDRAM at physical 0x3D000000
                   ▼
┌─────────────────────────────────────────────────┐
│  ARM Binary (this repo)                          │
│  shmem.cpp  — mmap /dev/mem to read DDRAM        │
│  ra_ramread — parse header, map console addresses │
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

Every supported core writes a structured block at ARM physical address `0x3D000000`:

| Offset | Content |
|--------|---------|
| `0x00000` | Header: magic `"RACH"`, region count, flags (busy bit), frame counter |
| `0x00100+` | RAM data area (layout varies per core) |
| `0x40000` | Address request list — ARM writes here (Option C protocol) |
| `0x48000` | Value response cache — FPGA writes results here |

### Per-Core RAM Exposure Strategy

#### NES — Full Mirror (`ra_ram_mirror.sv`)
The FPGA copies all relevant RAM to DDRAM on every VBlank (~10 KB total):
- **CPU-RAM** ($0000–$07FF) — 2 KB at DDRAM offset `0x100`
- **Cart SRAM** ($6000–$7FFF) — 8 KB at DDRAM offset `0x900`

#### SNES — Selective Address Protocol (`ra_ram_mirror_snes.sv`)
Due to the larger RAM space, the ARM binary writes the list of addresses it needs, and the FPGA reads only those (~185 per frame, ~30 µs overhead):
- **WRAM** ($000000–$01FFFF) — 128 KB, mirrored at DDRAM offset `0x100`
- **BSRAM** ($020000+) — up to 256 KB, mirrored at DDRAM offset `0x20100`

#### PSX — Selective Address Protocol (`ra_ram_mirror_psx.sv`)
Same request/response protocol as SNES. The FPGA reads requested addresses from SDRAM via a dedicated channel (CH4):
- **Main RAM** ($000000–$1FFFFF) — 2 MB, byte-addressed linearly
- Values returned via DDRAM offset `0x48000` response cache

### ROM / Disc Hashing

| Core | Method |
|------|--------|
| NES | MD5 of ROM data, skipping 16-byte iNES header and optional 512-byte trainer |
| SNES | MD5 of ROM data, skipping optional 512-byte SMC/SWC copier header (detected when `file_size % 1024 == 512`) |
| PSX | `rc_hash_generate_from_file()` from rcheevos — handles `.cue+.bin`, `.chd`, and `.iso` disc images natively |

### How It Works

1. **On startup**, the binary loads credentials from `retroachievements.cfg`, initializes the rcheevos client, and logs in asynchronously.
2. **When a game is loaded**, the binary hashes the ROM or disc image and queries the RA server to identify the game and fetch its achievement set.
3. **Every frame**, `achievements_poll()` checks the DDRAM mirror's frame counter. When it advances, it calls `rc_client_do_frame()`, which reads emulated RAM via the mirror and evaluates all active achievement conditions.
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
4. You will also need one or more of the **modified cores**:
   - **NES**: [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
   - **SNES**: [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer)
   - **PSX**: [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer)
5. Reboot your MiSTer, load the core, and open a game that has achievements on [retroachievements.org](https://retroachievements.org/).
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
- Modified NES core: [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer)
- Modified SNES core: [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer)
- Modified PSX core: [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer)
- RetroAchievements: [retroachievements.org](https://retroachievements.org/)
- rcheevos library: [RetroAchievements/rcheevos](https://github.com/RetroAchievements/rcheevos)
