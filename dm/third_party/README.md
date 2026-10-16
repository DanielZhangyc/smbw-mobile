# dm/third_party: third-party code

中文: [README.zh-CN.md](README.zh-CN.md)

This is code written by others, shipped with the repository so there is nothing to download. Each component keeps its original license, and the license file sits in its own directory.

| Directory | What it is | Version and changes | Where it is used | License file |
|---|---|---|---|---|
| `bcdec/` | Decompression of BC-compressed textures, a single header `bcdec.h` | 0.985 (upstream commit `80859ed`), unmodified | Graphics layer: decompresses textures on the CPU when the GPU does not support BC formats (`dm/gfx/tex/nvn_bcdec.c`) | `bcdec/LICENSE` (MIT or Unlicense, your choice) |
| `flip/` | NVIDIA FLIP, an image difference metric that models human viewing. `FLIP.h` plus the source of the command-line tool in `tool/` | Not recorded in the directory | Verification: the image quality measurements in `dm/verify/gfxq/`; build `flip` first with `make -C dm/third_party/flip` | `flip/LICENSE` (BSD 3-Clause) |
| `libdeflate/` | Decompression library | Upstream commit `92e6a0d`, unmodified | Host: zlib decoding of the game's resource blocks (`dm/host/hle_tact.c`). Only the five decompression-related source files are built; they are listed in `dm/host/Makefile` | `libdeflate/COPYING` (MIT) |
| `libvpx/` | The VP9 decoder from libvpx: the 71 source files a VP9-decoder-only build compiles, their headers, and the generated configuration in `config/` | v1.17.0 (upstream commit `6df3ec3`), unmodified | Host: decodes the games' VP9 movies (`dm/host/hle_movie.c`). `dm/host/Makefile` builds it into `gen/vpx/libvpx.a`; the iOS build compiles the same files | `libvpx/LICENSE` (BSD 3-Clause), with the patent grant in `libvpx/PATENTS` |
| `ryujinx/` | The shader translation library from Ryujinx, three projects: `Ryujinx.Graphics.Shader`, `Ryujinx.Common`, `Spv.Generator` | Ryujinx 1.3.2 with local shader translation and reflection changes; see `ryujinx/README.md` | Referenced by the shader translation tool `dm/gfx/tools/dmsh_xlate`, which translates the original Switch shaders to SPIR-V | `ryujinx/LICENSE.txt` (MIT); `ryujinx/src/Spv.Generator/LICENSE` is that subproject's own license text |

A few more notes:

- `flip/tool/` also carries three single-header libraries used by the FLIP command-line tool: `stb_image.h` (2.23), `stb_image_write.h` (1.13) and `tinyexr.h`. Their license notices are in the files themselves.
- `bcdec/README.md`, `libdeflate/VERSION.md`, `libvpx/README.md` and `ryujinx/README.md` record where each came from and what it is used for.
- Outside this directory, [NVN declarations](../gfx/nvn/include/nvn.h) retain their full MIT notice in the file header. The layout size algorithm in [nvn_tex.c](../gfx/nvn/nvn_tex.c) is a local port of [deko3d](https://github.com/devkitPro/deko3d); its [zlib license](../../docs/licenses/deko3d-LICENSE.txt) is preserved separately. That license was checked at upstream commit `350f2b00a3e76ecd4f00191f8c5d6544ffbcb9db`; the original port's precise upstream revision was not recorded.
- The pinned shader translator also ships managed NuGet dependencies. Their exact versions, sources and full license files are listed in [the translator's third-party notices](../gfx/tools/dmsh_xlate_bin/THIRD-PARTY-NOTICES.md).
