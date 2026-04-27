export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM

MAKEROM="$DEVKITPRO/tools/bin/makerom"

# Basic makerom params (avoid using RSF). Override values as needed.
OUT_DIR=./build/n3ds
ELF="$OUT_DIR/cinnamon.elf"
OUT_CIA="$OUT_DIR/cinnamon.cia"

APP_TITLE="Cinnamon"
APP_PRODUCT_CODE="000400000DC07F00"
APP_UNIQUE_ID="000400000DC07F00"

ARGS=( -f cia -o "$OUT_CIA" -target t -exefslogo -elf "$ELF" -v -DAPP_ENCRYPTED=false )

# Pass -D overrides
ARGS+=( -DAPP_TITLE="${APP_TITLE}" -DAPP_PRODUCT_CODE="${APP_PRODUCT_CODE}" -DAPP_UNIQUE_ID="${APP_UNIQUE_ID}" )

# Optional icon/banner if present in project root
if [ -f icon.icn ]; then
	ARGS+=( -icon icon.icn )
fi
if [ -f banner.bnr ]; then
	ARGS+=( -banner banner.bnr )
fi

# Generate `icon.icn` from `icon.png` if missing and `bannertool` is available
BANNERTOOL="$DEVKITPRO/tools/bin/bannertool"
if [ ! -f icon.icn ] && [ -f icon.png ] && [ -x "$BANNERTOOL" ]; then
	echo "Generating icon.icn from icon.png"
	"$BANNERTOOL" makesmdh -s "${APP_TITLE}" -l "${APP_TITLE}" -p "${APP_TITLE}" -i icon.png -o icon.icn || echo "bannertool makesmdh failed"
	if [ -f icon.icn ]; then
		ARGS+=( -icon icon.icn )
	fi
fi

# Generate `banner.bnr` from `icon.png` if missing and bannertool is available
if [ ! -f banner.bnr ] && [ -f icon.png ] && [ -x "$BANNERTOOL" ]; then
	echo "Generating banner.bnr from icon.png"
	# Use icon.png for the banner image and no audio
	"$BANNERTOOL" makebanner -i icon.png -o banner.bnr || echo "bannertool makebanner failed"
	if [ -f banner.bnr ]; then
		ARGS+=( -banner banner.bnr )
	fi
fi

# makerom requires an RSF file; generate a minimal temporary RSF to avoid using the repo RSF
TMP_RSF="$OUT_DIR/tmp_rsf_for_makerom.rsf"
mkdir -p "$OUT_DIR"
cat > "$TMP_RSF" <<EOF
BasicInfo:
	Title                   : ${APP_TITLE}
	ProductCode             : ${APP_PRODUCT_CODE}
	Logo                    : Homebrew # Nintendo / Licensed / Distributed / iQue / iQueForSystem

TitleInfo:
	Category                : Application
	UniqueId                : ${APP_UNIQUE_ID}

Option:
	UseOnSD                 : true
	FreeProductCode         : true
	EnableCompress          : true
EOF

RSF_PATH=""
if [ -f ./cinnamon.rsf ]; then
	RSF_PATH="./cinnamon.rsf"
else
	RSF_PATH="$TMP_RSF"
fi

ARGS_WITH_RSF=( -rsf "$RSF_PATH" )
ARGS_WITH_RSF+=( "${ARGS[@]}" )

echo "Running: $MAKEROM ${ARGS_WITH_RSF[*]}"
"$MAKEROM" "${ARGS_WITH_RSF[@]}"

# Remove the temporary RSF only if we generated it and the repo RSF wasn't used
if [ "$RSF_PATH" = "$TMP_RSF" ]; then
	rm -f "$TMP_RSF"
fi