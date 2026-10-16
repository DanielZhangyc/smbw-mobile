#!/bin/bash
set -euo pipefail

# 只打包宿主与启动配置；游戏文件和缓存留在仓库外的工作区。
# Package only the host and launch configuration; game files and caches stay in the external workspace.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=${SMBW_WORKSPACE:-"$ROOT/../smbw-mac-work"}
WORKSPACE=$(cd "$WORKSPACE" && pwd)
APP_NAME="Mario Wonder"
APP_ID="org.smbwmobile.mac.local"
if [[ "${SMBW_TEST_APP:-0}" == 1 ]]; then
    APP_NAME="Mario Wonder Test"
    APP_ID="org.smbwmobile.mac.local.test"
fi
APP="$WORKSPACE/out/$APP_NAME.app"
PLISTBUDDY=/usr/libexec/PlistBuddy

require_file() {
    [[ -f "$1" ]] || { printf 'Missing required file: %s\n' "$1" >&2; exit 1; }
}
require_file "$ROOT/dm/host/dmhost"
[[ -x "$ROOT/dm/host/dmhost" ]] || { printf 'Host is not executable.\n' >&2; exit 1; }
require_file "$ROOT/scripts/mac-app/launcher.sh"
for table in shader_map.tsv c1_full.tsv pso_manifest.tsv; do
    require_file "$ROOT/dm/games/smbw/$table"
done
require_file "$WORKSPACE/data/v327680/ExeFS/main"
require_file "$WORKSPACE/data/v327680/ExeFS/sdk"
[[ -d "$WORKSPACE/data/v327680/RomFS/System/Resource" ]] || { printf 'Missing Mario RomFS marker.\n' >&2; exit 1; }
for module in main sdk; do
    require_file "$WORKSPACE/build/patched_v327680/$module.patched.bin"
    require_file "$WORKSPACE/build/patched_v327680/$module.fixups.tsv"
    require_file "$WORKSPACE/build/patched_v327680/$module.gates.tsv"
done
require_file "$WORKSPACE/build/font/nintendo_ext.ttf"
require_file /System/Library/Fonts/Supplemental/Arial\ Unicode.ttf
PYTHON="$WORKSPACE/.venv/bin/python"
[[ -x "$PYTHON" ]] || { printf 'Missing workspace Python environment.\n' >&2; exit 1; }
CHAIN=$(DM_GAME=smbw "$PYTHON" "$ROOT/dm/gfx/tools/dmsh_chain.py")
[[ "$CHAIN" =~ ^[0-9a-f]{12}$ ]] || { printf 'Invalid shader chain version.\n' >&2; exit 1; }
# 只接受本次源码对应的完整缓存，防止旧链的库掩盖漏编或失败。
# Require the complete cache for the current source chain so old libraries cannot hide missing or failed builds.
if ! strings "$ROOT/dm/host/dmhost" | rg -F -x "$CHAIN" > /dev/null; then
    printf 'Host does not contain the current shader chain; rebuild the host.\n' >&2
    exit 1
fi
"$PYTHON" "$ROOT/scripts/check-mac-shaders.py" \
    --cache "$WORKSPACE/build/cache/metal_shader_cache/b-$CHAIN" \
    --map "$ROOT/dm/games/smbw/shader_map.tsv" --expected-chain "$CHAIN"

# 先在同一输出目录完整生成并签名，再替换旧的 App。
# Build and sign a complete temporary bundle in the same output directory before replacing the old App.
mkdir -p "$WORKSPACE/out" "$WORKSPACE/run/save" "$WORKSPACE/run/temp"
STAGE=$(mktemp -d "$WORKSPACE/out/.mac-app.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
BUNDLE="$STAGE/$APP_NAME.app"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
# 宿主搬离源码目录后必须显式携带运行时查询表。
# Carry runtime lookup tables explicitly when moving the host out of the source directory.
mkdir -p "$BUNDLE/Contents/Resources/game"
for table in shader_map.tsv c1_full.tsv pso_manifest.tsv; do
    cp "$ROOT/dm/games/smbw/$table" "$BUNDLE/Contents/Resources/game/$table"
done
cp "$ROOT/dm/host/dmhost" "$BUNDLE/Contents/MacOS/dmhost"
cp "$ROOT/scripts/mac-app/launcher.sh" "$BUNDLE/Contents/MacOS/launcher"
chmod 755 "$BUNDLE/Contents/MacOS/dmhost" "$BUNDLE/Contents/MacOS/launcher"
printf '%s\n' "$WORKSPACE" > "$BUNDLE/Contents/Resources/workspace.txt"
PLIST="$BUNDLE/Contents/Info.plist"
"$PLISTBUDDY" -c 'Clear dict' "$PLIST"
"$PLISTBUDDY" -c 'Add :CFBundleExecutable string launcher' "$PLIST"
"$PLISTBUDDY" -c "Add :CFBundleIdentifier string $APP_ID" "$PLIST"
"$PLISTBUDDY" -c "Add :CFBundleName string $APP_NAME" "$PLIST"
"$PLISTBUDDY" -c "Add :CFBundleDisplayName string $APP_NAME" "$PLIST"
"$PLISTBUDDY" -c 'Add :CFBundlePackageType string APPL' "$PLIST"
"$PLISTBUDDY" -c 'Add :CFBundleShortVersionString string 1.2.1' "$PLIST"
"$PLISTBUDDY" -c 'Add :CFBundleVersion string 1' "$PLIST"
"$PLISTBUDDY" -c 'Add :LSMinimumSystemVersion string 27.0' "$PLIST"
"$PLISTBUDDY" -c 'Add :LSApplicationCategoryType string public.app-category.games' "$PLIST"
"$PLISTBUDDY" -c 'Add :LSSupportsGameMode bool true' "$PLIST"
plutil -lint "$PLIST"
codesign --force --sign - "$BUNDLE/Contents/MacOS/dmhost"
codesign --force --sign - "$BUNDLE"
codesign --verify --strict "$BUNDLE/Contents/MacOS/dmhost"
codesign --verify --strict "$BUNDLE"
if [[ -e "$APP" ]]; then rm -rf "$APP"; fi
mv "$BUNDLE" "$APP"
printf 'App: %s\nLog: %s/run/app.log\n' "$APP" "$WORKSPACE"
