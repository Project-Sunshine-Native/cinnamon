# Cinnamon
A fork of butterscotch with 3DS and Wii U support!

### TO-DO:
- Replace all glfw references with citro3d (not using citro2d because citro3d is closer) in n3ds/main.c
- Dynamically split textures bigger than 1024x1024
- Pad textures that do not have resolutions ^2
- Add audio
- Remove controller stubs and use them for input
- Force second camera to always render to bottom screen
- Make touchscreen register mouse click and mouse movements

### File Explanations:
- runner.c handles events, rooms, instances, etc.
- vm.c runs the bytecode of the game
- renderer.h does, guess what, rendering!
- vm_builtins.c handles built in variables and funcs

### Profiling (optional)
- Build with profiling enabled: `make PROFILE=1`
- Adjust report cadence: `make PROFILE=1 PROFILE_REPORT_EVERY=180`
- Adjust spike threshold (ms): `make PROFILE=1 PROFILE_SPIKE_MS=33.0`
- Profiling output is written to stderr/stdout logs and prints:
	- periodic averages (`input`, `step`, `audio`, `render`, `present`, `throttle`)
	- per-frame spike breakdowns when a frame exceeds the threshold
  
