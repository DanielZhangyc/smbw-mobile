#!/bin/bash
set -euo pipefail

# 读取打包时记录的绝对工作区路径；调用者可显式覆盖。
# Read the absolute workspace recorded at packaging time; callers may explicitly override it.
MACOS=$(cd "$(dirname "$0")" && pwd)
WORKSPACE=${SMBW_WORKSPACE:-$(cat "$MACOS/../Resources/workspace.txt")}
WORKSPACE=$(cd "$WORKSPACE" && pwd)
mkdir -p "$WORKSPACE/run/save" "$WORKSPACE/run/temp"
exec >> "${SMBW_LOG:-$WORKSPACE/run/app.log}" 2>&1
printf '\nLaunch: %s\nWorkspace: %s\n' "$(date '+%Y-%m-%d %H:%M:%S %z')" "$WORKSPACE"

# 启动真实原生宿主；宿主继续验证改写模块与匹配的着色器链。
# Launch the real native host; the host continues validating rewritten modules and the matching shader chain.
PATCHED_DIR=${SMBW_PATCHED_DIR:-$WORKSPACE/build/patched_v327680}
[[ -f "$PATCHED_DIR/main.patched.bin" ]]
[[ -f "$PATCHED_DIR/sdk.patched.bin" ]]
[[ -d "$WORKSPACE/data/v327680/RomFS/System/Resource" ]]
export DM_ROOT="$WORKSPACE"
export DM_ROMFS="$WORKSPACE/data/v327680/RomFS"
export DM_SAVE="${DM_SAVE:-$WORKSPACE/run/save}"
export DM_TEMP="${DM_TEMP:-$WORKSPACE/run/temp}"
export DM_MSL_DIR="${DM_MSL_DIR:-$WORKSPACE/build/cache/metal_shader_cache}"
export DM_NVN_SHMAP="$MACOS/../Resources/game/shader_map.tsv"
# 默认同时读取随包清单和运行中记录的用户清单，减少效果首次出现时的编译卡顿。
# Load both the bundled seed and recorded user manifest by default to reduce first-use compilation stalls.
export DM_FONT_DIR="$WORKSPACE/build/font"
export DM_SHARED_FONT=/System/Library/Fonts/Supplemental/Arial\ Unicode.ttf
export DM_WINDOW=${DM_WINDOW:-1}
export DM_AUDIO=${DM_AUDIO:-1}
# 默认启用手柄振动；诊断运行可显式设为 0，沿用宿主的感知强度曲线。
# Enable controller vibration by default; diagnostic runs can explicitly set zero, retaining the host's perceptual intensity curve.
export DM_RUMBLE_GAIN="${DM_RUMBLE_GAIN:-1}"
# Mac 请求更密的显示回调；按真实目标时间推进游戏周期，60 Hz 显示也保持约 60 帧。
# Request denser Mac display callbacks; pace guest periods by real targets so 60 Hz displays still run near 60 fps.
export DM_PRESENT_LINK="${DM_PRESENT_LINK:-2}"
export DM_PRESENT_AHEAD="${DM_PRESENT_AHEAD:-0}"
export DM_MAC_DRAWABLES="${DM_MAC_DRAWABLES:-3}"
mkdir -p "$DM_SAVE" "$DM_TEMP"
cd "$WORKSPACE/run"
exec "$MACOS/dmhost" "$PATCHED_DIR"
