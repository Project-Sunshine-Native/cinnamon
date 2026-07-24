<h1 align="center">Cinnamon</h1>

<p align="center">
    <img src="icon.png" height="128px"></img>
</p>
<p align="center">
    <a href="https://discord.gg/undertale3ds"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

---

> [!IMPORTANT]  
> Cinnamon may not work with every game and games may have quirks that do not exist in the original GML runner.

When you create a game in GameMaker: Studio and export it, GameMaker: Studio exports the game code as bytecode instead of native compiled code, and that bytecode is compatible with any other GameMaker: Studio runner (also known as YoYo runner), as long as they have matching GameMaker: Studio versions. This is similar to how Java applications work.

This is how projects such as Droidtale can exist. We exploit that GameMaker: Studio games compile to bytecode, which means they can be ran on any platform that has an official runner for it!

If GameMaker games use bytecode, what prevents us from creating our own runner? And if we can write our own runner, what prevents us from porting GameMaker: Studio games to other platforms?

Thats where projects like [Butterscotch](https://github.com/MrPowerGamerBR/Butterscotch) come in! Butterscotch is an open source reimplementation of GameMaker: Studio's runner.

If this already exists, then whats stopping people from porting Butterscotch to MORE consoles? Whats stopping *us* from porting a variety of GameMaker: Studio games to the 3DS and Wii U?

This is where Cinnamon, a fork of Butterscotch comes in!

Cinnamon aims to be a open source re-implementation of GameMaker Studios runner **for the 3DS and Wii U.** This opens up lots of opportunities for games like Pizza Tower, Undertale Yellow, Undertale, and Deltarune to run on the 3DS and Wii U.


## Game Compatibility

Cinnamon's goal is to be able to have most GML games fully playable. 

Of course, there are hardware limits. These are consoles that are more than a decade old and while Cinnamon is heavily optimized, games with heavy 3D or heavy particle systems MAY not work.

Here are the Bytecode Versions that Cinnamon supports

* Bytecode Version 16
* Bytecode Version 17

However, that doesn't mean that a game that uses a compatible version WILL run! The bytecode support is still a WIP, and Cinnamon may have quirks that the original GameMaker: Studio runner may not have.

We do want to support more bytecode versions in the future (since Butterscotch supports more than 16 and 17) but currently, bytecode versions that arent 16 or 17 wont work.

Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode. 
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.
This includes game like Forager, Hyperlight Drifter, and more. 

## Supported Platforms
* Nintendo 3DS
* Nintendo Wii U
* ...and maybe more Nintendo consoles like the Wii soon!

Cinnamon was made for Project Sunshines ports.

## Project Sunshine
* Project Sunshine is a project that aims to use Cinnamon to port a variety of games (such as UNDERTALE, DELTARUNE, and maybe more games in the future) to the Wii U, 3DS, and maybe more consoles like the GameCube in the future! You can get beta builds on our [Discord](https://discord.gg/AahyBCvVR2) aswell as news on the ports.
### UNDERTALE: Wii U Edition
* A released, full port of UNDERTALE on the Wii U. You can download this port on our releases page or on our Discord.
### DELTARUNE: Wii U Edition
* A full port of DELTARUNE Chapters 1-5 to the Wii U. Still currently in development.
### UNDERTALE: 3DS Edition
* A full port of UNDERTALE to the 3DS with 3DS exclusive features such as 3D and bottom screen features.
### DELTARUNE: 3DS Edition
* A full port of DELTARUNE Chapters 1-5 to the old and new 3DS.

## Building For Wii U

You must have a proper devkitPro Wii U enviroment set up and configured for your platform. The `wiiu-sdl2` and `ppc-bzip2` devkitPro packages also need to be installed.

On Windows, make sure MinGW is located in your system PATH (C:/MinGW/bin) before proceeding with build instructions.

Configure with the Wii U CMake wrapper and then build:

```bash
powerpc-eabi-cmake -S . -B build/wiiu -DPLATFORM=wiiu -DCMAKE_BUILD_TYPE=Release
cmake --build build/wiiu
```

This produces `Cinnamon.elf`, `Cinnamon.rpx`, and a `.wuhb` bundle in `build/wiiu`.

You can also configure Wii U builds with the toolchain file directly:

```bash
cmake -S . -B build/wiiu -DPLATFORM=wiiu \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/WiiU.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/wiiu
```

On Windows, build from PowerShell with:

```powershell
.\build-windows-wiiu.ps1
```

## Building for 3DS

You must have a proper devkitPro 3DS environment set up and configured for your platform.

On Windows, make sure MinGW is located in your system PATH (C:/MinGW/bin) before proceeding with build instructions.

Configure and build it with the devkitPro 3DS toolchain:

```bash
arm-none-eabi-cmake -S . -B build/n3ds -DPLATFORM=n3ds -DCMAKE_BUILD_TYPE=Release
cmake --build build/n3ds
```

On Windows, build from PowerShell with:


```powershell
.\build-windows-n3ds.ps1
```


You can also use the repository Makefile on Linux or from an MSYS2/devkitPro shell on Windows:

```bash
make 3ds
```

If you prefer plain CMake, pass the toolchain file explicitly:

```bash
cmake -S . -B build/n3ds -DPLATFORM=n3ds \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/n3ds
```

To build without bottom screen features, pass the disable flag like so:

```bash
cmake -S . -B build/n3ds -DN3DS_DISABLE_BOTTOM_SCREEN=ON
```

The main output is `build/n3ds/cinnamon.3dsx`.

The 3DS build will package `resources/3ds/romfs` into the `.3dsx` if that directory exists. The runner also checks `sdmc:/3ds/cinnamon` at runtime, so you can either bundle preprocessed assets into `romfs` or copy them onto the SD card.

## Using the 3DS preprocessor

Cinnamon expects converted textures and audio instead of original texture and audio assets from data.win. Those files are generated by the standalone `n3ds-preprocess` host tool in `tools/n3ds-preprocess`.

Build the preprocessor with:

```bash
cmake -S tools/n3ds-preprocess -B build/n3ds-preprocess -DCMAKE_BUILD_TYPE=Release
cmake --build build/n3ds-preprocess
```

The preprocessor uses:

* `tex3ds` from devkitPro for texture conversion
* `stb_vorbis` for decoding OGG vorbis audio files for converting to BCWAV 4-bit ADPCM

On Linux:

* Ensure `tex3ds` is available (usually `/opt/devkitpro/tools/bin/tex3ds`).
* Run the built binary directly:

```bash
build/n3ds-preprocess/n3ds-preprocess /path/to/data.win resources/3ds/romfs
```

If your tools are not in default locations, pass explicit paths:

```bash
build/n3ds-preprocess/n3ds-preprocess /path/to/data.win resources/3ds/romfs \
  --tex3ds /opt/devkitpro/tools/bin/tex3ds
```

On Windows, running `n3ds-preprocess.exe` with no arguments starts an interactive setup that tries to find Undertale automatically and then writes to your SD card layout.

Command line usage:

```bash
n3ds-preprocess <path-to-data.win> <output-dir> [options]
```

By default the preprocessor uses hybrid atlas formatting: sprite/background pages stay `rgba5551` for cleaner edges, while safer non-sprite pages may still use `etc1a4`. Use `--texture-format etc1a4`, `--texture-format rgba5551`, or `--page-format-overrides <file>` if you need to force a specific format.

Useful output directory choices:

* `resources/3ds/romfs` to bundle the generated assets into the next 3DS build
* Your SD card's `3ds/cinnamon` folder to test assets without rebuilding the `.3dsx`

Example: generate bundled ROMFS assets from Undertale's `data.win`:

```bash
build/n3ds-preprocess/n3ds-preprocess /path/to/data.win resources/3ds/romfs
```

Example: write directly to an SD card layout:

```bash
build/n3ds-preprocess/n3ds-preprocess /path/to/data.win /path/to/SD/3ds/cinnamon
```

The preprocessor writes:

* `gfx/atlas.bin` and converted texture pages to `gfx/`
* `gfx/direct_assets.bin`, containing packed direct sprite/background/font `.t3x` data with seek metadata
* SOND data packed directly in `audio/sound_bank.bin` 
* streamed music `.bcwav` files at the output root

Optional sprite replacements can be placed in a `Sprite_replacements` folder next to the `n3ds-preprocess` executable or in `tools/n3ds-preprocess/Sprite_replacements`. The preprocessor accepts PNGs named by sprite name or sprite index, such as `spr_battlebutton_0.png`, `spr_battlebutton_0_frame_00000.png`, `spr_00042.png`, or `spr_00042_frame_00000.png`. Replacement PNGs must match the logical sprite frame size.

Optional room border PNGs can be placed in a `Borders` folder next to the `n3ds-preprocess` executable or in `tools/n3ds-preprocess/Borders`. Every top-level `*.png` in that folder is converted to `gfx/borders/<name>.t3x`; useful names include `border_none.png`, `room_ruins.png`, `room_tundra.png`, `room_water.png`, `room_fire.png`, `room_castle.png`, `room_truelab.png`, and `room_gaster.png`.

At runtime, direct textures are loaded from `gfx/direct_assets.bin` when present, with loose-file fallback for overrides/custom files.
Generated direct sprite/background/font `.t3x` files are removed after packing to keep output size down.
To reduce SD wear, the preprocessor stages intermediate/unfinalized files in a local temp folder next to the preprocessor executable, then syncs only finalized/changed outputs to your selected destination.

## Showcase

### Wii U (Real Hardware)

- **UNDERTALE (Bytecode 16)**
<img width="160" height="120" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/db8de335d5067d628d3bae1c90a705a57db3dbf9/resources/readme/screenshots/1000057247.jpg" />
<img width="160" height="120" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/db8de335d5067d628d3bae1c90a705a57db3dbf9/resources/readme/screenshots/1000057245.jpg" />
<img width="160" height="120" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/db8de335d5067d628d3bae1c90a705a57db3dbf9/resources/readme/screenshots/1000057243.jpg" />

- **SURVEY_PROGRAM (Bytecode 16)**
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/db8de335d5067d628d3bae1c90a705a57db3dbf9/resources/readme/screenshots/1000045789.jpg" />
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/db8de335d5067d628d3bae1c90a705a57db3dbf9/resources/readme/screenshots/1000057255.jpg" />

### Wii U (Cemu/Emulator)

- **SURVEY_PROGRAM (Bytecode 16)**
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/fe76737aea9d6402a55eb0059444c5283f88dff1/resources/readme/screenshots/1000057249.jpg" />
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/fe76737aea9d6402a55eb0059444c5283f88dff1/resources/readme/screenshots/1000057251.jpg" />

- **Pizza Tower Demo (Demo 1, Sage 2019 Demo) (Bytecode 16)**
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/fe76737aea9d6402a55eb0059444c5283f88dff1/resources/readme/screenshots/1000057253.png" />
<img width="160" height="200" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/fe76737aea9d6402a55eb0059444c5283f88dff1/resources/readme/screenshots/1000057254.png" />

### 3DS (Real Hardware)

- **UNDERTALE (Bytecode 16)**
<img width="160" height="300" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/d2b7229ec79a30dc5cabaad163a595741d37897a/resources/readme/screenshots/1000057263.jpg" />
<img width="160" height="300" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/d2b7229ec79a30dc5cabaad163a595741d37897a/resources/readme/screenshots/1000057262.jpg" />
<img width="160" height="300" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/d2b7229ec79a30dc5cabaad163a595741d37897a/resources/readme/screenshots/1000057257.jpg" />
<img width="160" height="300" alt="Image" src="https://github.com/Project-Sunshine-Native/cinnamon/blob/d2b7229ec79a30dc5cabaad163a595741d37897a/resources/readme/screenshots/1000049179.jpg" />

## Disclaimer

Cinnamon has no association, endorsement, or any connection whatsoever with any of the software that it facilitates, and does not provide any of the software it can run by itself. In order to use Cinnamon, you will need to provide your own game files.

