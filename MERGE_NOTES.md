# Merge Notes: Butterscotch → Cinnamon (3DS)

## What Was Done

This project was produced by merging upstream Butterscotch performance fixes into
Cinnamon (the 3DS fork) while preserving all 3DS-specific code.

### Strategy
- **Base**: Cinnamon (all `src/n3ds/`, Makefile, build scripts, romfs, resources, tools kept intact)
- **Shared files**: Replaced with Butterscotch's newer versions (performance fixes, refactors)
- **3DS patches**: Re-applied Cinnamon's 3DS-specific changes on top of new shared files
- **New files from Butterscotch**: Added to `src/`

---

## Files Updated (shared → Butterscotch upstream)

All files in `src/` that exist in both projects were updated to Butterscotch's version:
`vm.c`, `vm_builtins.c`, `runner.c`, `runner.h`, `data_win.c`, `data_win.h`, `rvalue.h`,
`vm.h`, `renderer.h`, `collision.h`, `instance.c`, `instance.h`, `binary_reader.c`,
`binary_reader.h`, `binary_utils.h`, `runner_keyboard.c`, `runner_keyboard.h`,
`text_utils.h`, `utils.h`, `noop_file_system.c`, `noop_file_system.h`,
`data_win_print.c`, `ini.c`, `ini.h`, `input_recording.c`, `input_recording.h`,
`json_reader.c`, `json_reader.h`, `json_writer.c`, `json_writer.h`, `matrix_math.h`,
`file_system.h`, `audio_system.h`

## Files Added from Butterscotch (new, didn't exist in Cinnamon)

- `src/debug_overlay.c` / `src/debug_overlay.h` — Debug overlay system
- `src/gml_array.c` / `src/gml_array.h` — GML array refactor
- `src/gml_method.c` / `src/gml_method.h` — GML method refactor
- `src/noop_audio_system.c` / `src/noop_audio_system.h` — No-op audio backend
- `src/bytecode_versions.h` — Bytecode version compile-time selection macros
- `src/common.h` — Common macros (nullptr, MAYBE_UNUSED)
- `src/real_type.h` — GMLReal typedef (float vs double selection)

## Files Kept from Cinnamon Only (not in Butterscotch)

- `src/n3ds/` — Entire 3DS platform directory (untouched)
- `src/profiler.c` — 3DS profiler
- `include/profiler.h` — 3DS profiler header
- `src/stb_ds.h`, `src/stb_image.h`, `src/stb_image_write.h` — STB headers
- `Makefile`, `build-n3ds.sh`, `make-cia-n3ds.sh`, `build-cmake-n3ds.sh`
- `tools/`, `romfs/`, `resources/`, `include/`

---

## Changes Applied for 3DS Compatibility

### 1. `src/data_win.c` — 3DS OS header
Added back the `#ifdef __3DS__` include of `<3ds/os.h>` after the stb/utils includes.

### 2. `src/runner.h` — Sprite decimation fields
Added 3DS-specific fields back to the `Runner` struct:
```c
bool drawSpriteDecimationEnabled;
int drawSpriteDecimationPhase;
```

### 3. `src/runner.c` — Nullable renderer/audioSystem
Removed the `requireNotNull` assertions for `renderer` and `audioSystem` in
`Runner_create()`, since the 3DS platform creates those after runner initialization
and assigns them directly to `runner->renderer` / `runner->audioSystem`.

### 4. `src/n3ds/main.c` — Updated Runner_create call
Butterscotch changed `Runner_create` from 3 args to 5 args:
```c
// Old (3 args):
Runner_create(dataWin, vm, fileSystem)
// New (5 args, NULL for renderer/audioSystem set after):
Runner_create(dataWin, vm, NULL, fileSystem, NULL)
```

### 5. `Makefile` — Bytecode version flags
Added `-DENABLE_BC16 -DENABLE_BC17` to `CFLAGS` so `bytecode_versions.h` macros
resolve correctly. Both BC16 and BC17 are enabled (correct for Undertale).

---

## Known Remaining Issues / Things To Watch

### `renderer.h` beginFrame vtable signature
The vtable defines `beginFrame` as:
```c
void (*beginFrame)(Renderer*, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH);
```
But `n3ds_renderer.c`'s `CBeginFrame` and `n3ds/main.c`'s call site pass two extra
arguments (`clearColor` and `speed`). This was a pre-existing mismatch in Cinnamon
before this merge — it works on ARM via calling convention but is technically UB.
**If you see rendering issues, consider updating the vtable signature in `renderer.h`
to match the extended version, and updating GLFW/PS2 renderers to ignore the extra args.**

### dsMapPool / dsListPool moved into Runner struct
Butterscotch refactored these from file-global variables into fields on the `Runner`
struct (`runner->dsMapPool`, `runner->dsListPool`, `runner->gmlBufferPool`).
If the 3DS build crashes on DS map/list operations, this is the likely cause — check
that `vm_builtins.c` calls are going through the runner pointer correctly.

### `debug_overlay.c` pulls in `runner.h`
`debug_overlay.h` includes `runner.h` → `vm.h` → etc. It should compile fine but adds
to compile time. If it causes issues on 3DS (e.g. missing symbols), you can exclude it
from the Makefile with:
```makefile
EXCLUDE_SOURCES := debug_overlay.c
CFILES := $(filter-out $(EXCLUDE_SOURCES),$(CFILES))
```

### `noop_audio_system.c` is now compiled for 3DS
It's harmless (it just defines a stub audio backend), but if you want a clean build
with no dead code, add it to an exclusion list in the Makefile as above.
