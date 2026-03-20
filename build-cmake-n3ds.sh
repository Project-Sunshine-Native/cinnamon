cmake -S . -B ./build-n3ds \
  -G "Unix Makefiles" \
  -DPLATFORM=n3ds -DN3DS=1 \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake"