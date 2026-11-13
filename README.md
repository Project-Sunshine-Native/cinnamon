

<h1 align="center">Cinnamon</h1>


</p>
<p align="center">
    <a href="https://discord.gg/undertale3ds"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

---
> [!IMPORTANT]  
> This is an older repository of Cinnamon, and will only sometimes be updated with the latest source. For the most up to date source, please check https://github.com/Project-Sunshine-Native/cinnamon-runner

Cinnamon is a **fully human made** fork of the Butterscotch GameMaker runner specifically targeting the Nintendo 3DS, Wii, and Wii U

The 3DS, Wii, and Wii U backends were built by humans and humans only. No AI tools were used for development!

## Game Compatibility

Cinnamon's goal is to be able to have most GameMaker games fully playable. 

Of course, there are hardware limitations. These are consoles that are more than a decade old and while Cinnamon is heavily optimized, games with heavy 3D or heavy particle systems may run poorly or not run at all.

Here are the Bytecode Versions that Cinnamon currently supports

- WAD Version 8 (GameMaker: Studio 1.0.198+)
- WAD Version 9 (GameMaker: Studio 1.0.527+)
- WAD Version 10 (GameMaker: Studio 1.1.609+)
- WAD Version 11 (GameMaker: Studio 1.1.754+)
- WAD Version 12 (GameMaker: Studio 1.1.867+)
- WAD Version 13 (GameMaker: Studio 1.1.917+)
- WAD Version 14 (GameMaker: Studio 1.4.1464+)
- WAD Version 15 (GameMaker: Studio 1.4.1675+)
- WAD Version 16 (GameMaker: Studio 1.4.1767+)
- WAD Version 17 (GameMaker: Studio 2.2+)


Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode. 
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.
  
This includes games like Forager, Hyperlight Drifter, and more. 

## Supported Platforms
* Nintendo 3DS
* Nintendo Wii U
* Nintendo Wii
* ...and maybe more Nintendo consoles in the future!

Cinnamon was made for Project Sunshine's ports.

## Project Sunshine
* Project Sunshine is a project that aims to use Cinnamon to port a variety of games (such as UNDERTALE, DELTARUNE, and maybe more games in the future) to the Wii U, 3DS, Wii, GameCube, and maybe more consoles in the future! You can get beta builds on our [Discord](https://discord.gg/undertale3ds) aswell as news on the ports.
### UNDERTALE: Wii U Edition
* A released, full port of UNDERTALE on the Wii U. You can download this port on our releases page or on our Discord.
### DELTARUNE: Wii U Edition
* A full port of DELTARUNE Chapters 1-5 to the Wii U. Still currently in development.
### UNDERTALE: 3DS Edition
* A full port of UNDERTALE to the 3DS with 3DS exclusive features such as 3D and bottom screen features.
### DELTARUNE: 3DS Edition
* A full port of DELTARUNE Chapters 1-5 to the old and new 3DS.
### UNDERTALE: Wii Edition
* A full port of UNDERTALE to the Wii.
### DELTARUNE Wii Edition
* A full port of DELTARUNE to the Wii.
### UNDERTALE: GameCube Edition
* A full port of UNDERTALE to the GameCube.
### DELTARUNE GameCube Edition
* A full port of DELTARUNE Chapter 1 to the GameCube.

## Building
Before proceeding, ensure you have a proper devkitpro environment set up for your platform

### Building For 3DS

Install the SDK and audio libraries:

```bash
sudo dkp-pacman -Syu --needed 3ds-dev 3ds-libvorbisidec 3ds-libogg 
```
then build:

```bash
cmake --preset 3ds
cmake --build --preset 3ds
```

Outputs:

- `build/3dsx/cinnamon.3dsx`: executable for the Homebrew Launcher.
- `build/3dsx/cinnamon.cia`: installable CIA for launching from the home screen.

This builds the standard Release configuration with games loaded from the SD
card. For installing games, see [the preprocessor](https://github.com/Project-Sunshine-Native/cinnamon-preprocessor)

### Building for Wii

Install the SDK and audio libraries:

```bash
sudo dkp-pacman -Syu --needed wii-dev ppc-libvorbisidec ppc-libogg
```
then build:

```bash
cmake --preset wii
cmake --build --preset wii
```

Outputs:

- `build/wii/boot.dol`: executable for the Homebrew Channel.
- `build/wii/boot.elf`: ELF executable.
- `build/wii/icon.png`: Homebrew Channel icon.
- `build/wii/meta.xml`: Homebrew Channel metadata.

Copy `boot.dol`, `icon.png`, and `meta.xml` into `/apps/cinnamon/` on the SD
card, then launch Cinnamon from the Homebrew Channel. For installing games, see [the preprocessor](https://github.com/Project-Sunshine-Native/cinnamon-preprocessor)


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
