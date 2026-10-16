#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/mac-env.sh"

export DM_ROMFS="$SMBW_WORKSPACE/data/$SMBW_VERSION/RomFS"
export DM_SAVE="${DM_SAVE:-$SMBW_WORKSPACE/run/save}"
export DM_TEMP="${DM_TEMP:-$SMBW_WORKSPACE/run/temp}"
export DM_MSL_DIR="${DM_MSL_DIR:-$SMBW_WORKSPACE/build/cache/metal_shader_cache}"
export DM_NVN_SHMAP="${DM_NVN_SHMAP:-$SMBW_REPO/dm/games/smbw/shader_map.tsv}"
export DM_FONT_DIR="$SMBW_WORKSPACE/build/font"
export DM_SHARED_FONT="/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
if [[ "${SMBW_HEADLESS:-0}" == 1 ]]; then
    unset DM_WINDOW
else
    export DM_WINDOW=1
fi
export DM_AUDIO="${DM_AUDIO:-1}"

host="${SMBW_HOST:-$SMBW_REPO/dm/host/dmhost}"
test -x "$host"
test -f "$SMBW_WORKSPACE/build/patched_$SMBW_VERSION/main.patched.bin"
test -f "$SMBW_WORKSPACE/build/patched_$SMBW_VERSION/sdk.patched.bin"
test -d "$DM_ROMFS/System/Resource"
test -d "$DM_MSL_DIR"
test -f "$DM_NVN_SHMAP"
test -f "$DM_FONT_DIR/nintendo_ext.ttf"
mkdir -p "$DM_SAVE" "$DM_TEMP"
exec "$host" "$SMBW_WORKSPACE/build/patched_$SMBW_VERSION" main sdk \
    > "${SMBW_LOG:-$SMBW_WORKSPACE/run/manual.log}" 2>&1
