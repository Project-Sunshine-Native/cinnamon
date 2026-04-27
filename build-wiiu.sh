export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC

#!/bin/bash
set -e

# ── Config ────────────────────────────────────────────────────────────────────
APP_NAME="My App"
SHORT_NAME="MyApp"
AUTHOR="YourName"
VERSION="1.0.0"

SOURCE_DIR="./source"
BUILD_DIR="./build"
OUTPUT_DIR="./output/wiiu"

ICON="./resources/wiiu/icon.png"        # 128x128 PNG
TV_IMAGE="./resources/wiiu/tv.png"      # 1280x720 PNG  (TV banner)
DRC_IMAGE="./resources/wiiu/drc.png"    # 854x480 PNG   (Gamepad banner)

OUTPUT_WUHB="$OUTPUT_DIR/$SHORT_NAME.wuhb"
# ─────────────────────────────────────────────────────────────────────────────

# Check devkitPro env
if [ -z "$DEVKITPRO" ]; then
  echo "Error: DEVKITPRO is not set. Source your devkitPro environment first."
  echo "  e.g. export DEVKITPRO=/opt/devkitpro"
  exit 1
fi

if [ -z "$DEVKITPPC" ]; then
  export DEVKITPPC="$DEVKITPRO/devkitPPC"
fi

export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

# Check required tools
for tool in make wuhbtool elf2rpl; do
  if ! command -v "$tool" &>/dev/null; then
    echo "Error: '$tool' not found. Install wiiu-dev via devkitPro pacman."
    exit 1
  fi
done

# Check assets exist
for asset in "$ICON" "$TV_IMAGE" "$DRC_IMAGE"; do
  if [ ! -f "$asset" ]; then
    echo "Warning: Asset not found: $asset"
  fi
done

# Create dirs
mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

echo "──────────────────────────────────────"
echo " Building: $APP_NAME v$VERSION"
echo "──────────────────────────────────────"

# ── Step 1: Compile ───────────────────────────────────────────────────────────
echo "[1/3] Compiling source..."
make \
  BUILD="$BUILD_DIR" \
  SOURCE="$SOURCE_DIR" \
  --no-print-directory

# ── Step 2: ELF → RPX ────────────────────────────────────────────────────────
ELF_FILE=$(find "$BUILD_DIR" -name "*.elf" | head -n 1)

if [ -z "$ELF_FILE" ]; then
  echo "Error: No .elf file found in $BUILD_DIR after build."
  exit 1
fi

RPX_FILE="${ELF_FILE%.elf}.rpx"

echo "[2/3] Converting ELF → RPX..."
elf2rpl "$ELF_FILE" "$RPX_FILE"

# ── Step 3: Package WUHB ──────────────────────────────────────────────────────
echo "[3/3] Packaging WUHB..."

WUHBTOOL_ARGS=(
  "$OUTPUT_WUHB"
  "$RPX_FILE"
  --name        "$APP_NAME"
  --short-name  "$SHORT_NAME"
  --author      "$AUTHOR"
)

[ -f "$ICON" ]      && WUHBTOOL_ARGS+=(--icon      "$ICON")
[ -f "$TV_IMAGE" ]  && WUHBTOOL_ARGS+=(--tv-image  "$TV_IMAGE")
[ -f "$DRC_IMAGE" ] && WUHBTOOL_ARGS+=(--drc-image "$DRC_IMAGE")

wuhbtool "${WUHBTOOL_ARGS[@]}"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "✓ Done! Output: $OUTPUT_WUHB"
echo ""
echo "Copy to SD card:"
echo "  sd:/wiiu/apps/$SHORT_NAME/$SHORT_NAME.wuhb"
