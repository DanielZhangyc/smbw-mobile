DM_ROOT ?= $(HOME)/dm-work
# 游戏档案（dm/games/<名>/）：宿主按它编（make GAME=名，缺省 smbw）。game_profile.h 由 game.tsv 生成在 gen/<名>/，dmsh_chain.h 在档案目录
# Game profile (dm/games/<id>/): the host is built for it (make GAME=id, default smbw). game_profile.h is generated from game.tsv into gen/<id>/, dmsh_chain.h lives in the profile folder
GAME ?= smbw
GAME_DIR = ../games/$(GAME)
GAME_H = gen/$(GAME)/game_profile.h
CFLAGS=-O1 -g -Wall -Wno-unused-function
# 服务层接管 hle_*.c 按通配纳入：新增文件不改这一行（免合并冲突）
# Service-layer takeover hle_*.c files are included by wildcard: adding a file does not touch this line (avoids merge conflicts)
SRC=main.c loader.c hle.c host_stubs.c crash.c probe.c thread.c memstat.m ../ios/Sources/dm_touch_controls.c $(sort $(wildcard hle_*.c)) front_mac.m front_controller_mac.m front_haptics_mac.m front_ns2_mac.m ns2_rumble.c entry.S dm_i18n.c
# NVN 空设备 + nn::vi（dm/gfx/）
# 按通配纳入 dm/gfx 与 dm/gfx/nvn 下的源（tests/ metal/ 不进宿主）：新增文件不改这一行
# NVN null device + nn::vi (dm/gfx/)
# Sources under dm/gfx and dm/gfx/nvn are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/include/*.h ../gfx/tex/*.h))
# 第三方：libdeflate 只编解压相关源（TACT BLTE zlib 块，hle_tact.c）
# Third party: only the decompression-related sources of libdeflate are built (TACT BLTE zlib blocks, hle_tact.c)
LIBDEFLATE=../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c
# 第三方：libvpx 只带 VP9 解码器的源（影片解码，hle_movie.c），配置头在 config/（arm64，Mac 与 iOS 共用）。单独编成静态库、按文件增量编，改宿主不重编它
# Third party: libvpx ships only the VP9 decoder's sources (movie decoding, hle_movie.c), with its configuration headers in config/ (arm64, shared by Mac and iOS). Built separately into a static library, file by file, so host changes do not recompile it
VPX=../third_party/libvpx
VPX_SRC=$(sort $(shell find $(VPX) -name '*.c'))
VPX_OBJ=$(patsubst $(VPX)/%.c,gen/vpx/%.o,$(VPX_SRC))
VPX_LIB=gen/vpx/libvpx.a
gen/vpx/%.o: $(VPX)/%.c $(wildcard $(VPX)/config/*.h)
	@mkdir -p $(dir $@)
	clang -O2 -g -I$(VPX) -I$(VPX)/config -c $< -o $@
$(VPX_LIB): $(VPX_OBJ)
	rm -f $@; ar rcs $@ $^
.DEFAULT_GOAL := dmhost   # 下面先写了表头生成规则，缺省目标要明说（否则 make 只生成表头、不编宿主） | The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
# 由原版表生成的头文件：不进仓库，构建时从玩家自己的游戏文件（Switch 版 3.3.94012 的 Update/RomFS）生成；路径用环境变量 DM_ROMFS_SRC 指定
# Headers generated from the original game's tables: not checked into the repository, generated at build time from the player's own game files (Update/RomFS of Switch version 3.3.94012); the path is given by the environment variable DM_ROMFS_SRC
ROMFS?=$(or $(DM_ROMFS_SRC),$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS)
GENH=dm_vault_tab.h dm_hud_exp.h dm_bright_levels.inc
NEED_ROMFS=@test -f "$(ROMFS)/data/build_manifest.psv" || { echo "🔴 没找到游戏文件：$(ROMFS)/data/build_manifest.psv。把 Switch 版 3.3.94012 的 Update/RomFS 放到这里，或用环境变量 DM_ROMFS_SRC 指过去（见 dm/README.md「从你的游戏文件生成资源」）"; exit 1; }
dm_vault_tab.h: ../ios/tools/gen_vault_table.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_vault_table.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_hud_exp.h: ../ios/tools/gen_hud_exp.py ../ios/tools/romfs_art.py
	$(NEED_ROMFS)
	python3 ../ios/tools/gen_hud_exp.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
dm_bright_levels.inc: ../tools/gen_bright_levels.py ../ios/tools/touch_layout.py
	$(NEED_ROMFS)
	python3 ../tools/gen_bright_levels.py "$(ROMFS)" > $@.tmp && mv $@.tmp $@
.PHONY: game_profile
game_profile: $(GAME_H)
$(GAME_H): $(GAME_DIR)/game.tsv ../games/game.py
	python3 ../games/game.py header $(GAME) $@
# 记下宿主是按哪个游戏编的：换 GAME 时戳记内容变、宿主重编（否则 make 看 dmhost 比两份表头都新，照旧用另一个游戏的宿主）
# Records which game the host was built for: changing GAME changes the stamp's content and rebuilds the host (otherwise make sees dmhost as newer than both headers and keeps the other game's host)
GAME_STAMP = gen/game.stamp
$(GAME_STAMP): FORCE
	@mkdir -p gen; echo $(GAME) | cmp -s - $@ || echo $(GAME) > $@
.PHONY: FORCE
FORCE:
dmhost: $(SRC) $(GFX) dm.h hle.h dm_input.h front.h ns2.h touch_mac.h ../ios/Sources/dm_touch_controls.[ch] $(GFX_H) $(GAME_H) $(GAME_DIR)/dmsh_chain.h $(GAME_STAMP) $(VPX_LIB)
	clang $(CFLAGS) -I../gfx/nvn/include -I$(dir $(GAME_H)) -I$(GAME_DIR) -I$(VPX) -fobjc-arc -o $@ $(SRC) $(GFX) $(VPX_LIB) -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework ImageIO -framework UniformTypeIdentifiers -framework GameController -framework CoreHaptics -framework CoreBluetooth
