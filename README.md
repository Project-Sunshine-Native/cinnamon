<p align="center">
<a href="https://discord.gg/AahyBCvVR2"><img src="https://img.shields.io/discord/1406856655920168971?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

<h1 align="center">Cinnamon</h1>

> [!IMPORTANT]  
> Cinnamon is not finished and will have bugs. 3DS is going through a rewrite so you cannot build at this time.

Cinnamon aims to be a open source re-implementation of GameMaker: Studio's runner for the 3DS and Wii U. This opens up lots of opportunities for games like Pizza Tower, Forager, Undertale, and Deltarune to run on the 3DS and Wii U.

Games like UNDERTALE have already been successfully ported to the Wii U and 3DS and are playable the whole way through. While only Bytecode version 16 is supported as of now, more bytecodes and features will be implemented in the future.

### File Explanations:
- runner.c handles events, rooms, instances, etc.
- vm.c runs the bytecode of the game
- renderer.h does, guess what, rendering!
- vm_builtins.c handles built in variables and funcs
  
