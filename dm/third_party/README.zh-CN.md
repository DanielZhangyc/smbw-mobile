# dm/third_party：第三方代码

English: [README.md](README.md)

这里是别人写的代码，随仓库一起提供，不用另外下载。它们各自沿用原来的许可证，许可证文件就在各自目录里。

| 目录 | 是什么 | 版本与改动 | 用在哪 | 许可证文件 |
|---|---|---|---|---|
| `bcdec/` | BC 压缩贴图的解压，单个头文件 `bcdec.h` | 0.985（上游提交 `80859ed`），未改动 | 图形层：显卡不支持 BC 格式时在 CPU 上解压贴图（`dm/gfx/tex/nvn_bcdec.c`） | `bcdec/LICENSE`（MIT 或 Unlicense，任选其一） |
| `flip/` | NVIDIA FLIP：模拟人眼观看的图像差异度量。`FLIP.h` 加命令行工具的源码 `tool/` | 目录里没有记录 | 验证：`dm/verify/gfxq/` 的画质度量，先 `make -C dm/third_party/flip` 编出 `flip` | `flip/LICENSE`（BSD 3-Clause） |
| `libdeflate/` | 解压库 | 上游提交 `92e6a0d`，未改动 | 宿主：游戏资源块的 zlib 解码（`dm/host/hle_tact.c`）。只编解压相关的五个源文件，名单在 `dm/host/Makefile` | `libdeflate/COPYING`（MIT） |
| `libvpx/` | libvpx 的 VP9 解码器：只编 VP9 解码器时编到的 71 个源文件、它们的头文件，以及 `config/` 里生成的配置 | v1.17.0（上游提交 `6df3ec3`），未改动 | 宿主：解游戏的 VP9 影片（`dm/host/hle_movie.c`）。`dm/host/Makefile` 把它编成 `gen/vpx/libvpx.a`，iOS 构建编同一批文件 | `libvpx/LICENSE`（BSD 3-Clause），专利授权见 `libvpx/PATENTS` |
| `ryujinx/` | Ryujinx 的着色器翻译库，三个项目：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator` | Ryujinx 1.3.2，含本地着色器翻译与反射修改，见 `ryujinx/README.md` | 着色器翻译工具 `dm/gfx/tools/dmsh_xlate` 引用它，把 Switch 原版着色器翻译成 SPIR-V | `ryujinx/LICENSE.txt`（MIT）；`ryujinx/src/Spv.Generator/LICENSE` 是该子项目自带的许可证文本 |

几点补充：

- `flip/tool/` 里还带着 FLIP 命令行工具用到的三个单头文件库：`stb_image.h`（2.23）、`stb_image_write.h`（1.13）、`tinyexr.h`。它们的许可声明写在各自文件里。
- `bcdec/README.md`、`libdeflate/VERSION.md`、`libvpx/README.md`、`ryujinx/README.md` 记着各自的来源与用途。
- 本目录之外，[NVN 声明](../gfx/nvn/include/nvn.h) 在文件头保留完整 MIT 许可。[nvn_tex.c](../gfx/nvn/nvn_tex.c) 的排布尺寸算法是 [deko3d](https://github.com/devkitPro/deko3d) 的本地移植，完整 [zlib 许可](../../docs/licenses/deko3d-LICENSE.txt) 单独保留。许可核对自上游提交 `350f2b00a3e76ecd4f00191f8c5d6544ffbcb9db`；原始移植未记录确切的上游修订。
- 固定版本的着色器翻译工具还随附 NuGet 托管依赖，确切版本、来源与完整许可见[翻译工具第三方声明](../gfx/tools/dmsh_xlate_bin/THIRD-PARTY-NOTICES.md)。
