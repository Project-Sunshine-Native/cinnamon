<h1 align="center">🥧 Butterscotch 🥧</h1>

> [!IMPORTANT]  
> Butterscotch is still VERY early in development and it is NOT that good yet.

When you create a game in GameMaker: Studio and export it, GameMaker: Studio exports the game code as bytecode instead of native compiled code, and that bytecode is compatible with any other GameMaker: Studio runner (also known as YoYo runner), as long as they have matching GameMaker: Studio versions. This is similar to how Java applications work.

This is how projects such as [Droidtale](https://mrpowergamerbr.com/projects/droidtale) (which was also made by yours truly) can exist. We exploit that GameMaker: Studio games compile to bytecode, which means they can be ran on *any* platform that has an official runner for it!

Ever since I created Droidtale 10+ years ago, I had that lingering thought in my mind... If GameMaker games use bytecode, what prevents us from creating our *own* runner? And if we can write our *own* runner, what prevents us from porting GameMaker: Studio games to other platforms?

And that's where Butterscotch comes in! Butterscotch is an open source re-implementation of GameMaker: Studio's runner.

**Butterscotch PlayStation 2 ISO Generator:** https://butterscotch.mrpowergamerbr.com/

## Game Compatibility

Butterscotch's goal is to be able to have Undertale v1.08 (GameMaker: Studio 1.4.1804, Bytecode Version 16) fully playable. But we do want to support more GameMaker: Studio games in the future too!

While our target is Undertale v1.08, that doesn't mean that other games CAN'T run in Butterscotch! Because Butterscotch is a runner and not a Undertale port/remake, you CAN run other GameMaker: Studio games with it and, as long as the game is compiled with GameMaker: Studio 1.4.1804 and they only use GML variables and functions that Butterscotch supports, it should work fine.

Of course, there are exceptions that break game compatibility altogether:

* Games compiled with YYC, because they use native code instead of bytecode. 
* Games compiled with the new [GMRT](https://github.com/YoYoGames/GMRT-Beta/tree/main), because they use native code instead of bytecode.

## Supported Platforms

* Linux (GLFW, OpenGL)
* PlayStation 2 (ps2sdk, gsKit)
* ...and maybe more in the future!

## Building Butterscotch

```bash
mkdir build && cd build
cmake -DPLATFORM=glfw -DCMAKE_BUILD_TYPE=Debug ..
make
```

If you are using CLion, set the platform in `Settings` > `Build, Execution, Deployment` > `CMake` and add `-DPLATFORM=glfw`

Then run Butterscotch with `./butterscotch /path/to/data.win`!

## CLI parameters

The GLFW target has a lot of nifty CLI parameters that you can use to trace and debug games running on it.

* `--debug`: Enables debugging hotkeys
* `--screenshot=file_%d.png`: Screenshots the runner, requires `--screenshot-at-frame`.
* `--screenshot-at-frame=Frame`: Screenshots the runner at a specific frame. Can be used multiple times.
* `--headless`: Runs the runner in headless mode. When running in headless mode, the game will run at the max speed that your system can handle.
* `--print-rooms`: Prints all the rooms in the `data.win` file and exits.
* `--print-declared-functions`: Prints all the declared functions (scripts, object events, etc) in the `data.win` file and exists.
* `--trace-variable-reads`: Traces variable reads
* `--trace-variable-writes`: Traces variable writes
* `--trace-function-calls`: Traces function calls
* `--trace-alarms`: Traces alarms
* `--trace-instance-lifecycles`: Traces instance creations and deletions
* `--trace-events`: Traces events
* `--trace-event-inherited`: Traces event inherited calls
* `--trace-tiles`: Traces drawn tiles
* `--trace-opcodes`: Traces opcodes
* `--trace-stack`: Traces stack
* `--trace-frames`: Logs when a frame starts and when a frame ends, including how much time it took to process each frame.
* `--exit-at-frame=Frame`: Automatically exit the runner after X frames.
* `--speed`: Speed multiplier
* `--seed=Seed`: Sets a fixed seed for the runner, useful for reproduceable runs.
* `--print-rooms`: Prints all rooms to the console, along with all objects present in the room.
* `--print-declared-functions`: Prints all declared GML scripts by the game
* `--disassemble`: Dissassembles a specific script
* `--record-inputs`: Records user inputs
* `--playback-inputs`: Playbacks user inputs
* `--debug`: Enable debug features

## Debug Features

When running Butterscotch with `--debug`, the following hotkeys are enabled:

* `Page Up`: Moves forward one room
* `Page Down`: Moves backwards one room
* `P`: Pauses the game
* `O`: While paused, advances the game loop by one frame
* `F12`: Dumps the current runner state to the console
* `F11`: Dumps the current runner state to the console (JSON format), or dumps it to a file if `--dump-frame-json-file` is set.
* `F10`: Sets the `global.interact` flag to `0`. Useful in Undertale when you are moving through rooms and one of them starts a cutscene that doesn't let you move.

## Performance

Performance is pretty good on any modern computer, but when running on low end targets (like the PS2) it is *very* slow when there's a lot of instances on screen, or when a instance does a for loop.

## Then why not have a transpiler?

The issue with a transpiler is that, if you try transpiling the game in the "naive" way, that is, emitting VM calls like it was the original bytecode, you won't get any 
*improvement* from it, you would need to create a *good* transpiler that actually transpiles it into *good* code, and that's way harder.

Having a transpiler also have other disadvantages:

1. You lose the ability of debugging the runner at a "high level" by tracing opcodes.
2. Compilation is SLOW, transpiling Undertale in a naive way to C and building it takes 90 seconds on a modern computer, and building it to other targets is so slow that I wasn't even able to test it.

## Screenshots

### Undertale (GLFW)

<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/6651cc2e-0d6d-4354-b98d-081e84a981df" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/1d6edc51-2829-4f8f-b900-393f21a6655b" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/0d41f16c-7ee5-47de-a2e8-5831cdcd2745" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/45dc47fb-6d8a-44d4-8cbb-2e5791100144" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/7db1c869-e625-4558-9119-0f23da0f020c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/71fc7616-d580-48fe-aa6d-1e6ceea41bdb" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/4098936e-a1b9-4971-901d-702ec390afa7" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/dd3dcce3-3d78-452f-9af0-27133497650c" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/2e356d04-5aaf-47d4-9bc3-4abba78cd18d" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/a9cbc57f-e9c1-4985-a6af-a98e5fce5ff3" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/e5c67781-0ffc-43c8-9c7d-333254eed704" />
<img width="160" height="120" alt="Image" src="https://github.com/user-attachments/assets/93900e3c-79b5-4a05-bd6c-d68814e9e101" />

### Undertale (PlayStation 2)

Here's a video :3 https://youtu.be/3MoAPO8H85U

## Tales of Agentic Engineering

Since I created Droidtale, I've always had the thought about "if it is a VM, then you could reimplement it and port any GameMaker game to any platform, right?".

Fast forward ten years, and while I was toying around with Claude Code I've thought about "what if I asked Claude to vibe code a GameMaker runner re-implementation in Kotlin lol". Of course, this did not go well because Claude was not even able to render the Undertale intro sequence, until I intervened and started nudging it in the right direction. After nudging it, and debugging it manually, it actually got somewhere and was able to render Undertale's intro sequence up until the main menu...

...and that's when I found out that we were onto something here and that the idea could be viable. And that's when I started learning C and rewriting the runner to be cross-platform.

Butterscotch is not "vibe coded", that is, I didn't just let Claude go wild and implement everything. This does not work, and anyone that says that "vibe coding" works and will replace all developers is lying, or they aren't skilled enough to know what good code looks like, or they are rich and have a lot of money to spend while Claude keeps getting stuck trying to resolve bugs that they don't know how to fix like [it was a infinite monkey theorem](https://en.wikipedia.org/wiki/Infinite_monkey_theorem).

Butterscotch is made with an "agentic engineering" approach. I did use Claude Code to implement a lot of things in Butterscotch, but every line of code was reviewed and refactored by me to polish the code that Claude made (Claude LOVES creating duplicate code and creating some non-sensical things).

That does not mean that Butterscotch's code is good however, I'm not a C developer (JVM my beloved) and I know just enough C to be dangerous (who doesn't like a use after free crash??) so some of the code decisions are questionable at best, harmful at worst. That's why if you see the codebase, it really feels like it is a Java developer trying to create a C application.

That said, here are some tips and tricks with using Claude Code for projects like this:

* The project itself, when you think about it, is somewhat trivial, which is why Claude excelled on it.
    * [UndertaleModTool](https://github.com/UnderminersTeam/UndertaleModTool) has bytecode documentation and you can decompile Undertale's source code to GML, so Claude can compare the bytecode to the original GML code to figure out what the interpreter is doing wrong.
    * [GameMaker-HTML5](https://github.com/YoYoGames/GameMaker-HTML5) has GML's builtin variables and functions straight from YoYo Games, so it can just port the functions from JavaScript to C.
    * While decompiled code is hard for us to understand, LLMs are quite good at it. Telling Claude to read Ghidra's decompiled GameMaker: Studio runner code to figure out some bytecode interpreter opcodes shenanigans actually had surprising results.
    * This isn't the first project that attempts something like this! [OpenGM](https://github.com/misternebula/OpenGM) is also a open source GameMaker: Studio runner re-implementation, written in C# instead of C, and it exists since 2024! While Claude did *not* read OpenGM's source code during Butterscotch's development, that doesn't mean that Claude never studied OpenGM's source code during its training. So maybe when it was implementing things in Butterscotch, it may have pulled things that it had previously learned from OpenGM and other similar projects.
* Adding ways for Claude to know what's going on in the runner makes it way easier for it to know what is the issue. Things that are useful for us are also useful for them. That's why Butterscotch has a lot of `--trace-*` functions, because Claude can use it to trace what is going on in the runner to try pointing it to the right direction to where it may be going wrong. And when that fails, we as humans can use the very useful `--trace-*` functions too.
* Adding ways for Claude to take screenshots lets them try to tackle graphical related issues, because you can tell them "if the old screenshot is different than the new screenshot, you've probably fixed the issue".
* Adding input recording and playback allows us to record a set of inputs that reproduce a specific bug, and then we can tell Claude to playback our inputs to figure out what could be going wrong to cause that specific bug.
* You still need to know EXACTLY what you want, and know HOW are you going to break down the problem into small pieces. Just like what you already do when programming. If I didn't already have previous knowledge on how the YoYo runner worked, I would've probably said "pls port Undertale for me kthxbye" and that would've probably gone nowhere.

However I still needed to keep an eye out on whatever Claude was doing if they were trying to fix a non-trivial problem, because if I didn't attempt to nudge it in the right direction, it would go completely off the rails.
