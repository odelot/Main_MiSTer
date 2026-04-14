# Main_MiSTer — RetroAchievements Fork

This is a fork of the official [MiSTer Main binary](https://github.com/MiSTer-devel/Main_MiSTer) with **RetroAchievements** support for MiSTer FPGA.

> **Status:** Experimental / Proof of Concept

## Supported Cores

| Core | Console ID | Modified Core Repo |
|------|-----------|--------------------|
| NES | 7 | [odelot/NES_MiSTer](https://github.com/odelot/NES_MiSTer) |
| SNES | 3 | [odelot/SNES_MiSTer](https://github.com/odelot/SNES_MiSTer) |
| Genesis / Mega Drive | 1 | [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer) |
| Master System / Game Gear | 11 | [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer) |
| Gameboy / Gameboy Color | 4 | [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer) |
| N64 | 2 | [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer) |
| PSX | 12 | [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer) |

## How to Test

Pre-built binaries are available on the [Releases](https://github.com/odelot/Main_MiSTer/releases) page — no compilation needed.

1. Download the latest release and extract all files.
2. Edit `retroachievements.cfg` with your [RetroAchievements](https://retroachievements.org/) credentials (username and password).
3. Copy all files to `/media/fat` on your MiSTer SD card.
4. You will also need the modified core for the console you want to play. See the table above for the corresponding core repo (each one has its own release binaries).

For example, for **Master System / Game Gear**, get the modified SMS core from [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer).

## What's Different from the Original

The [upstream Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) binary manages cores, user input, video output, and the OSD menu on the MiSTer platform. This fork adds a full RetroAchievements integration layer on top, without modifying any existing core functionality.

### Files Added

| File | Purpose |
|------|--------|
| `achievements.cpp / .h` | Lifecycle management — init, load game, per-frame poll, unload, OSD popups |
| `ra_http.cpp / .h` | Async HTTP worker thread that executes RetroAchievements API calls via `curl` |
| `ra_ramread.cpp / .h` | Reads emulated console RAM from the DDRAM mirror written by the FPGA core |
| `shmem.cpp / .h` | Thin wrapper around `/dev/mem` + `mmap` for ARM ↔ FPGA shared memory access |
| `retroachievements.cfg` | User credentials file (username / password) |
| `lib/rcheevos/` | The [rcheevos](https://github.com/RetroAchievements/rcheevos) library (achievement logic, server protocol) |

### Files Modified (hooks into the main loop)

| File | Change |
|------|--------|
| `scheduler.cpp` | Calls `achievements_poll()` every frame |
| `menu.cpp` | Calls `achievements_load_game()` when a ROM is selected |
| `main.cpp` | Calls `achievements_init()` / `achievements_deinit()` at startup/shutdown |
| `Makefile` | Conditionally compiles rcheevos sources and links against them |

No other existing behavior is changed.

## Architecture

The RetroAchievements integration uses a four-layer pipeline that connects the FPGA hardware to the RetroAchievements server:

```
┌───────────────────────────────────────────────────────┐
│  FPGA Core (NES / SNES / Genesis / SMS / GB / N64 / PSX) │
│  ra_ram_mirror*.sv exposes emulated RAM to DDRAM      │
│  every VBlank (~60 Hz)                                │
└──────────────────────┬────────────────────────────────┘
                       │  DDRAM at physical 0x3D000000
                       ▼
┌───────────────────────────────────────────────────────┐
│  ARM Binary (this repo)                               │
│  shmem.cpp  — mmap /dev/mem to read DDRAM             │
│  ra_ramread — parse header, map console addresses     │
└──────────────────────┬────────────────────────────────┘
                       │
                       ▼
┌───────────────────────────────────────────────────────┐
│  rcheevos SDK                                         │
│  Evaluates achievement conditions against RAM         │
│  Manages unlock state, leaderboards, progress         │
└──────────────────────┬────────────────────────────────┘
                       │  Async HTTP (ra_http worker)
                       ▼
┌───────────────────────────────────────────────────────┐
│  RetroAchievements Server                             │
│  Login, game identification, unlock reporting         │
└───────────────────────────────────────────────────────┘
```

### How It Works

1. **Init** — On startup, the ARM binary maps the DDRAM mirror region, starts an HTTP worker thread, loads credentials from `retroachievements.cfg`, and logs in to RetroAchievements.
2. **Load Game** — When a ROM is selected in the MiSTer menu, the binary computes the ROM's MD5 hash and identifies the game against the RA database.
3. **Per-Frame Poll** — Every frame (~60 Hz), `achievements_poll()` checks whether the FPGA has written new data. If a new frame is available, it calls `rc_client_do_frame()` from the rcheevos library, which evaluates achievement conditions against the current RAM state.
4. **Unlock / Notify** — When an achievement triggers, the event handler displays an OSD notification and optionally plays a sound (`/media/fat/achievement.wav`). The unlock is reported to the RA server asynchronously.
5. **Unload / Shutdown** — State is cleaned up when switching cores or shutting down.

### DDRAM Mirror Layout

Every supported core writes a structured block at ARM physical address `0x3D000000`:

| Offset | Content |
|--------|---------|
| `0x00000` | Header: magic `"RACH"`, region count, flags (busy bit), frame counter |
| `0x00100+` | RAM data area (layout varies per core — used by Full Mirror protocol) |
| `0x40000` | Address request list — ARM writes here (Selective Address protocol) |
| `0x48000` | Value response cache — FPGA writes results here (Selective Address protocol) |

### Per-Core RAM Exposure Strategy

There are two protocols for exposing emulated RAM to the ARM:

- **Full Mirror** — The FPGA copies all relevant RAM to DDRAM every VBlank. Simple but only viable for small RAM spaces.
- **Selective Address (Option C)** — The ARM writes a list of addresses it needs; the FPGA reads only those values and writes them back. Required for cores with large address spaces.

#### NES — Full Mirror (`ra_ram_mirror.sv`)
The FPGA copies all relevant RAM to DDRAM on every VBlank (~10 KB total):
- **CPU-RAM** ($0000–$07FF) — 2 KB
- **Cart SRAM** ($6000–$7FFF) — 8 KB

#### SNES — Selective Address (`ra_ram_mirror_snes.sv`)
Due to the larger RAM space, the ARM binary writes the list of addresses it needs, and the FPGA reads only those (~185 per frame):
- **WRAM** ($000000–$01FFFF) — 128 KB
- **BSRAM** ($020000+) — up to 256 KB

#### Genesis / Mega Drive — Selective Address (`ra_ram_mirror_md.sv`)
Same request/response protocol. The FPGA reads requested addresses directly from the 68K Work RAM BRAM:
- **68K Work RAM** ($000000–$00FFFF) — 64 KB
- Includes hardware-accurate FC1004 address bit-13 inversion for correct BRAM mapping

#### Master System / Game Gear — Selective Address (`ra_ram_mirror_sms.sv`)
Uses the same selective address protocol. The FPGA reads from dual-ported System RAM and NVRAM:
- **System RAM** ($0000–$1FFF) — 8 KB (Z80 $C000–$DFFF mirrored)
- **NVRAM / Cart RAM** ($2000–$9FFF) — up to 32 KB

The SMS core converts its existing single-port RAMs to dual-port (`dpram`) so the RA mirror can read on Port B without disturbing the CPU on Port A. A custom DDRAM arbiter (`ddram_arb_sms.sv`) shares bus access between the framebuffer and the RA mirror.

#### Gameboy / Gameboy Color — Selective Address (`ra_ram_mirror_gb.sv`)
Uses the selective address protocol. The FPGA reads from dual-ported WRAM and ZPRAM (HRAM), plus cart RAM via a dedicated SDRAM channel:
- **WRAM** ($C000–$DFFF + GBC banks 2–7 via $10000–$15FFF) — up to 32 KB
- **Cart RAM** ($A000–$BFFF + banks 1–15 via $16000–$33FFF) — up to 128 KB via SDRAM ch2
- **HRAM / ZPRAM** ($FF80–$FFFE) — 127 bytes
- **Echo RAM** ($E000–$FDFF) — automatically translated to $C000–$DDFF

The Gameboy core multiplexes Port B of existing dual-port WRAM and ZPRAM BRAMs between savestate access and RA reads (RA takes priority when `ra_wram_req` / `ra_zpram_req` is asserted). Cart RAM is accessed read-only through SDRAM channel 2 with busy handshaking. A 2-cycle BRAM read path is used (address latch → output valid). The DDRAM arbiter in the core's `ddram.sv` module provides a dedicated channel for RA read/write operations alongside the savestate channel.

#### N64 — Selective Address (`ra_ram_mirror_n64.sv`)
Uses the same selective address protocol but with N64-specific adaptations. The FPGA reads requested byte addresses directly from RDRAM stored in DDR3:
- **RDRAM** ($000000–$7FFFFF) — 4 to 8 MB

Key differences from other cores:
- **Separate DDRAM base address** — N64 savestates occupy 0x3C000000–0x3FFFFFFF, colliding with the default RA mirror base. The N64 mirror is relocated to ARM physical 0x38000000.
- **DDR3 bus arbitration** — A dedicated arbiter (`ddram_arb_n64.sv`) shares the DDR3 bus between the N64 core's 8 DDR3Mux clients (CPU, RSP, RDP, VI, etc.) and the RA mirror. A starvation counter (512-cycle limit, ~4 µs) ensures the RA mirror eventually gets bus access, and a burst mode optimization chains consecutive RA operations without re-entering the starvation wait.
- **Clock domain crossing** — The N64 core's DDR3 controller runs at 125 MHz (`clk_2x`) while the RA mirror runs at ~62.5 MHz (`clk_1x`). Two-stage flip-flop synchronizers handle CDC for the toggle req/ack signals.
- **Direct byte mapping** — RDRAM byte N maps directly to DDRAM byte N within each 64-bit word (the CPU's byteswap32 + memorymux half-swap already places bytes correctly). No additional XOR or byte reordering is needed on the FPGA side.
- **8-VBlank warmup** — The mirror waits ~133 ms after reset before starting, to ensure the VI module (which generates VBlank) has stabilized.

#### PSX — Selective Address (`ra_ram_mirror_psx.sv`)
Same request/response protocol. The FPGA reads requested addresses from SDRAM via a dedicated channel (CH4):
- **Main RAM** ($000000–$1FFFFF) — 2 MB

### ROM / Disc Hashing

| Core | Method |
|------|--------|
| NES | MD5 of ROM data, skipping 16-byte iNES header and optional 512-byte trainer |
| SNES | MD5 of ROM data, skipping optional 512-byte SMC/SWC copier header (detected when `file_size % 1024 == 512`) |
| Genesis | MD5 of the raw ROM file (no header skipping needed) |
| Gameboy / GBC | MD5 of the raw ROM file (no header skipping needed) |
| N64 | `rc_hash_generate_from_file()` from rcheevos — handles `.z64`, `.n64`, and `.v64` byte orders natively |
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
   - **Genesis / Mega Drive**: [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer)
   - **Master System / Game Gear**: [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer)
   - **Gameboy / Gameboy Color**: [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer)
   - **N64**: [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer)
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
- Modified Genesis core: [odelot/MegaDrive_MiSTer](https://github.com/odelot/MegaDrive_MiSTer)
- Modified SMS core: [odelot/SMS_MiSTer](https://github.com/odelot/SMS_MiSTer)
- Modified Gameboy core: [odelot/Gameboy_MiSTer](https://github.com/odelot/Gameboy_MiSTer)
- Modified N64 core: [odelot/N64_MiSTer](https://github.com/odelot/N64_MiSTer)
- Modified PSX core: [odelot/PSX_MiSTer](https://github.com/odelot/PSX_MiSTer)
- RetroAchievements: [retroachievements.org](https://retroachievements.org/)
- rcheevos library: [RetroAchievements/rcheevos](https://github.com/RetroAchievements/rcheevos)
