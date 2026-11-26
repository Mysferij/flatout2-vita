#!/usr/bin/env bash
# Build the FlatOut 2 (Xbox) recompilation as a PS Vita homebrew VPK:
#
#   <SD>/ux0:/data/fo2x/fo2x.vpk
#   ux0:/data/fo2x/game/        the extracted disc: default.xbe, ... (copied by hand, not in the VPK)
#   ux0:/data/fo2x/fo2x_log.txt  written at runtime
#
# The VPK carries only eboot.bin and sce_sys/param.sfo. The game reads the
# unpacked disc files directly from ux0:data/fo2x/game/ -- never the ISO.
#
# Environment:
#   VITASDK              default /usr/local/vitasdk
#   XBOXRECOMP_DIR       toolkit (default: the vendored xboxrecomp/)
#   FO2X_GEN_DIR         lifted C from tools/regen.sh (default /root/fo2x/gen)
#   BUILD_DIR            default /root/fo2x/build-vita
#   JOBS                 parallel compile jobs (default: nproc, capped by memory)

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export VITASDK="${VITASDK:-/usr/local/vitasdk}"
TK="${XBOXRECOMP_DIR:-$REPO/xboxrecomp}"
GEN="${FO2X_GEN_DIR:-/root/fo2x/gen}"
BUILD="${BUILD_DIR:-/root/fo2x/build-vita}"

fail() { echo "error: $*" >&2; exit 1; }
[ -f "$VITASDK/share/vita.toolchain.cmake" ] || fail "VITASDK not found at $VITASDK"
[ -f "$GEN/recomp_funcs.h" ] || fail "no generated code in $GEN (run tools/regen.sh)"

if [ -z "${JOBS:-}" ]; then
    mem_mb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))
    JOBS=$(( mem_mb / 1500 )); [ "$JOBS" -lt 1 ] && JOBS=1
    [ "$JOBS" -gt "$(nproc)" ] && JOBS=$(nproc)
fi

cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$TK"  -DVITA=ON -DNFSU2_GEN_DIR="$GEN" >/dev/null
ninja -C "$BUILD" -j"$JOBS"

# Create the VPK structure
VPK_STAGE="$BUILD/vpk_stage"
rm -rf "$VPK_STAGE"
mkdir -p "$VPK_STAGE/sce_sys"

# eboot.bin: the ELF converted to a Vita ELF (velf) and signed as a self with
# the homebrew auth ID, as the VitaSDK vita_create_self macro does
# (vita-pack-vpk -b takes it)
vita-elf-create "$BUILD/fo2_recomp" "$BUILD/fo2_recomp.velf"
vita-make-fself -a 0x2800000000000001 "$BUILD/fo2_recomp.velf" "$VPK_STAGE/eboot.bin"

# PARAM.SFO: vita-mksfoex [-s NAME=VALUE]... TITLE output.sfo
# Options must come before the positional TITLE and output path.
# TITLE_ID is exactly 9 characters (4 uppercase letters + 5 digits); a
# shorter one makes the installer reject the VPK. CATEGORY=gd is the
# game-data category a VPK needs; APP_VER is the app version.
# CONTENT_ID is the 36-character Sony form: <region 2 letters><4 digits>-
# <TITLE_ID>_00-<16 chars>.
vita-mksfoex \
    -s TITLE_ID=FO2X00001 \
    -s CATEGORY=gd \
    -s APP_VER=01.00 \
    -s VERSION=01.00 \
    -s TITLE="FlatOut 2 (Xbox recomp)" \
    -s STITLE="FO2X" \
    -s CONTENT_ID="UP0001-FO2X00001_00-FO2X000000000001" \
    -d ATTRIBUTE2=12 \
    "FlatOut 2 (Xbox recomp)" "$VPK_STAGE/sce_sys/param.sfo"

# LiveArea assets (placeholders from the SDK sample until real art exists)
ASSETS="$REPO/vita/assets/sce_sys"
for f in icon0.png livearea/contents/bg.png livearea/contents/startup.png \
         livearea/contents/template.xml; do
    [ -f "$ASSETS/$f" ] || fail "missing LiveArea asset $ASSETS/$f"
done

# Pack the VPK (eboot.bin + param.sfo + LiveArea; the disc lives in ux0:data/fo2x/game/)
rm -f "$BUILD/fo2x.vpk"
vita-pack-vpk -s "$VPK_STAGE/sce_sys/param.sfo" \
    -b "$VPK_STAGE/eboot.bin" \
    -a "$ASSETS/icon0.png=sce_sys/icon0.png" \
    -a "$ASSETS/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png" \
    -a "$ASSETS/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png" \
    -a "$ASSETS/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml" \
    "$BUILD/fo2x.vpk"

echo
echo "created $BUILD/fo2x.vpk"
VPK_SIZE=$(stat -c %s "$BUILD/fo2x.vpk")
echo "  fo2x.vpk $(numfmt --to=iec-i --suffix=B $VPK_SIZE 2>/dev/null || echo $VPK_SIZE bytes)"
echo "  install with VitaShell, then copy the unpacked disc to ux0:data/fo2x/game/"
