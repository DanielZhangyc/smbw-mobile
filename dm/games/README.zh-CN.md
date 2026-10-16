# 游戏档案

本移植认得的每个游戏都有一个目录 `dm/games/<名>/`。马力欧的是 `smbw`。目录里有：

- **`game.tsv`**：这个游戏的常数，每行 `键<TAB>值`，注释独占一行。
- **着色器表**（`shader_split.py` 写出）：`shader_map.tsv`（含 `.conflicts.tsv`）、`shader_facts.tsv`、`shader_pairs.tsv`。
- **`c1_full.tsv`**：每次出着色器库后由 `c1_guard.py` 重生成。
- **`pso_manifest.tsv`**：预热种子清单。
- **`bf_want.tsv`**。
- **`dmsh_chain.h`**：翻译链版本，由 `dmsh_chain.py --header` 写出。

和游戏有关的都放这里。代码读档案，不写死某个游戏。

| 谁 | 用什么选档案 | 读什么 |
|---|---|---|
| 着色器工具（`shader_split.py`、`dmsh_chain.py`、`dmsh_build.py`、`dmsh_facts.py`） | `DM_GAME=<名>` | 各表，`nvn_*`、`shader_*` 键 |
| 宿主（`make -C dm/host`、`dm/gfx/tests`） | `GAME=<名>` | `gen/<名>/game_profile.h`（`game.py header` 由 `game.tsv` 生成） |
| `dm/ios/tools/build_ipa.sh` | `GAME=<名>` | App 名、包名、IPA 名、代码模块、RomFS 认定路径、各表 |
| `dm/ios/tools/build_ipa_from_nsp.py` | NSP 的标题号 | 以上全部。没有对应档案时只写粗查报告 |

缺省都是 `smbw`。查档案：
- `python3 dm/games/game.py list` 列出全部档案。
- `python3 dm/games/game.py <名> <键>` 打印一个键。

## 键

| 键 | 含义 |
|---|---|
| `title_id`、`program_id` | 身份。`program_id` 是 `svcGetInfo(ProgramId)` 报给游戏的值。游戏名不存：NSP 脚本从 NSP 的控制数据（NACP）读 |
| `app_name`、`bundle_id`、`ipa`、`app_category` | iOS App。`app_name` 只是后备：NSP 脚本用自己的 `APP_NAME` 设置，没设就用 NSP 里的游戏名；手动跑 `build_ipa.sh` 和 Mac 窗口标题用 `app_name` |
| `build <版本> <显示版本> <模块>=<构建号>…` | 每个已知版本一行。NSP 脚本只认这些构建；宿主按 `main` 的构建号报显示版本 |
| `package` | 缺省打进 IPA 的版本 |
| `shader_versions` | 着色器表是这几个版本的并集（基础版在前）。出包时这些版本都要有 |
| `modules` | 改写并装载的代码模块，按装载顺序。`rtld` 由宿主代替 |
| `romfs_marker` | RomFS 里必须有的路径 |
| `zip_store` | IPA 里不再压缩的扩展名 |
| `mem_reserve`、`mem_alias`、`mem_stack`、`app_pool` | 客体地址空间分区与应用内存池（`hle_svc.c`） |
| `nvn_tex_base`、`nvn_tex_slots`、`nvn_samp_base`、`nvn_img_base`、`nvn_img_slots`、`nvn_ssbo_bases` | NVN 驱动常量段，按全库着色器实测 |
| `shader_groups`、`shader_note` | `shader_split.py` 的来源组与映射表表头说明 |

翻译器（`dmsh_xlate/Program.cs`）里仍内置贴图 / 采样器句柄表基址与深度口径。档案的 `nvn_tex_base` / `nvn_samp_base` 与之不同时 `dmsh_build.py` 会停下，所以需要别的值的游戏要先让翻译器可配。

## 加一个游戏

1. **粗查。** 对这个游戏的 NSP 文件夹跑 `dm/ios/tools/build_ipa_from_nsp.py`。没有档案时它只解出代码模块和 RomFS 清单，写出 `<NSP 文件夹>/<标题号>_survey/survey.md`。报告包括模块与构建号、SDK、引擎与图形接口标记、RomFS 容器、改写点统计、导入与宿主钩子的对比。
2. **档案。** 把 `smbw/game.tsv` 拷成 `dm/games/<名>/game.tsv`，填身份、`build` 行和 `modules`。内存与 NVN 的值先沿用马力欧的，等起机时实测再改。
3. **着色器表。** 跑 `DM_GAME=<名> shader_split.py … dm/games/<名>/shader_map.tsv dm/games/<名>/shader_facts.tsv dm/games/<名>/shader_pairs.tsv`。再补空的 `shader_map.tsv.conflicts.tsv`、`bf_want.tsv` 和 `pso_manifest.tsv`（表头照马力欧的抄）。
4. **翻译链。**
   1. `DM_GAME=<名> dmsh_chain.py --header`
   2. `DM_GAME=<名> [IOSOUT=…] dmsh_build.py <包目录> <缓存目录>`
   3. `c1_guard.py <缓存目录>/b-<V> dm/games/<名>/c1_full.tsv`
5. **宿主起机。** 跑 `make -C dm/host GAME=<名>`，逐项核对模块装载、服务初始化和图形输出。只适合某一个游戏的代码按它的构建号限定（`dm_main_build()`），不留给所有游戏跑。
6. **IPA。** 用 `GAME=<名> dm/ios/tools/build_ipa.sh`，或者用 NSP 脚本（它现在找得到这个档案了）。
