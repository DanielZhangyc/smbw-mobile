# libvpx (VP9 decoder subset)

The VP9 decoder from libvpx v1.17.0 (upstream commit `6df3ec3`, 2026-08-20), unmodified. Upstream: <https://chromium.googlesource.com/webm/libvpx> (mirror <https://github.com/webmproject/libvpx>).
License: BSD 3-Clause, see `LICENSE`; the patent grant is in `PATENTS`, the authors in `AUTHORS`.
The host decodes the games' VP9 movies with it (`dm/host/hle_movie.c`, Fire Emblem Engage's cutscenes). `dm/host/Makefile` builds every `.c` here into `gen/vpx/libvpx.a`; the iOS build compiles the same files (`dm/ios/tools/gen_build.py` reads them from the Makefile).

Only the files that build needs are kept: the 71 sources libvpx's own build compiles for this configuration, and the headers they include. `config/` holds what `configure` generated (`vpx_config.h`, `vpx_version.h`, the `*_rtcd.h` dispatch headers). One arm64 configuration serves Mac and iOS; NEON is used directly, with no run-time CPU detection. To regenerate, run in an empty directory:

```
<libvpx>/configure --target=arm64-darwin24-gcc --disable-vp8 --enable-vp9-decoder --disable-vp9-encoder \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-runtime-cpu-detect \
  --disable-neon-dotprod --disable-neon-i8mm --disable-sve --disable-sve2 --disable-webm-io --disable-libyuv \
  --disable-install-docs --disable-install-bins
make libvpx.a V=1
```

The `-c` lines of that build give the source list (without the generated `vpx_config.c`), and the `.d` files give the headers.

libvpx v1.17.0 的 VP9 解码器（上游提交 `6df3ec3`，2026-08-20），未做修改。许可证 BSD 3-Clause，见 `LICENSE`；专利授权在 `PATENTS`，作者在 `AUTHORS`。
宿主用它解游戏的 VP9 影片（`dm/host/hle_movie.c`，火焰纹章 Engage 的过场动画）。`dm/host/Makefile` 把这里所有 `.c` 编成 `gen/vpx/libvpx.a`；iOS 构建编同一批文件（`dm/ios/tools/gen_build.py` 从 Makefile 取）。
只留这套构建要的文件：libvpx 自己的构建在上面的配置下编的 71 个源文件和它们包含的头文件。`config/` 是 `configure` 生成的（`vpx_config.h`、`vpx_version.h`、各 `*_rtcd.h` 分派头）。Mac 与 iOS 共用一套 arm64 配置，直接用 NEON，不做运行时 CPU 检测。重新生成：在空目录里跑上面的命令，源文件清单取构建的 `-c` 行（去掉生成的 `vpx_config.c`），头文件取 `.d` 文件。
