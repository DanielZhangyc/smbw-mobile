#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/mac-env.sh"
cd "$SMBW_REPO"

stage="${1:?Usage: prepare-mac.sh extract|rewrite|shaders|host}"
mkdir -p "$SMBW_WORKSPACE/build" "$SMBW_WORKSPACE/run/save" "$SMBW_WORKSPACE/run/temp"

case "$stage" in
    extract)
        rom_dir="${SMBW_NSP_DIR:-$(dirname "$(dirname "$SMBW_REPO")")/[NS]超级马里奥兄弟 惊奇/本体加补丁}"
        base="$rom_dir/[APP][010015100B514000][1.0.0][US][16.0.0].nsp"
        update="$rom_dir/[UPD][010015100B514800][1.2.1][US][22.1.0].nsp"
        keys="$SMBW_WORKSPACE/keys/prod.keys"
        test -f "$base"
        test -f "$update"
        test -f "$keys"
        dotnet build dm/tools/nx_extract -c Release -o "$SMBW_WORKSPACE/build/nx_extract"
        "$SMBW_WORKSPACE/build/nx_extract/nx_extract" "$keys" "$base" - "$SMBW_WORKSPACE/data/v0"
        "$SMBW_WORKSPACE/build/nx_extract/nx_extract" "$keys" "$base" "$update" \
            "$SMBW_WORKSPACE/data/$SMBW_VERSION" "$SMBW_WORKSPACE/data/v0/RomFS"
        ;;
    rewrite)
        for module in main sdk; do
            DM_RULESET=native "$SMBW_PYTHON" dm/tools/patch_nso.py \
                "$SMBW_WORKSPACE/data/$SMBW_VERSION/ExeFS/$module" \
                "$SMBW_WORKSPACE/build/patched_$SMBW_VERSION" "$module"
        done
        ;;
    shaders)
        "$SMBW_PYTHON" dm/gfx/tools/shader_split.py \
            "$SMBW_WORKSPACE/data/v0/RomFS,$SMBW_WORKSPACE/data/$SMBW_VERSION/RomFS" \
            "$SMBW_WORKSPACE/data/v0/ExeFS/main,$SMBW_WORKSPACE/data/$SMBW_VERSION/ExeFS/main" \
            "$SMBW_WORKSPACE/shader/pkg" \
            "$SMBW_WORKSPACE/shader/shader_map.tsv" \
            "$SMBW_WORKSPACE/shader/shader_facts.tsv" \
            "$SMBW_WORKSPACE/shader/shader_pairs.tsv"
        # 先核对输入产生的表，避免把不匹配的 ROM 数据写进游戏档案。
        # Compare tables from the input before changing the game profile.
        for table in shader_map.tsv shader_facts.tsv shader_pairs.tsv; do
            cmp "$SMBW_WORKSPACE/shader/$table" "dm/games/smbw/$table"
        done
        "$SMBW_PYTHON" dm/gfx/tools/dmsh_chain.py --check-tools
        "$SMBW_PYTHON" dm/gfx/tools/dmsh_chain.py --header
        "$SMBW_PYTHON" dm/gfx/tools/dmsh_build.py \
            "$SMBW_WORKSPACE/shader/pkg" "$SMBW_WORKSPACE/build/cache/metal_shader_cache"
        chain="$("$SMBW_PYTHON" dm/gfx/tools/dmsh_chain.py)"
        "$SMBW_PYTHON" dm/gfx/tools/c1_guard.py \
            "$SMBW_WORKSPACE/build/cache/metal_shader_cache/b-$chain" dm/games/smbw/c1_full.tsv
        ;;
    host)
        mkdir -p "$SMBW_WORKSPACE/build/font"
        "$SMBW_PYTHON" dm/tools/make_ext_font.py "$SMBW_WORKSPACE/build/font/nintendo_ext.ttf"
        make -C dm/host GAME=smbw -j8 dmhost
        ;;
    *)
        printf 'Unknown stage: %s\n' "$stage" >&2
        exit 2
        ;;
esac
