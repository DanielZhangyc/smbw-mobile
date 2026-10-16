# Ryujinx shader translator (vendored subset)

Three projects from Ryujinx 1.3.2: `Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator`. Local shader fixes lower dynamic texture offsets through the existing transform and declare the SPIR-V capabilities required for formatless storage-image reads and writes; a read-only SSA pass exports conservative texture sample LOD / cube-layer selectors.
The shader translation tool in `dm/gfx/tools/dmsh_xlate` references them to translate the original Switch shaders to SPIR-V.
License: MIT, see `LICENSE.txt`. Upstream: <https://github.com/Ryubing>.

Ryujinx 1.3.2 的三个项目：`Ryujinx.Graphics.Shader`、`Ryujinx.Common`、`Spv.Generator`。本地着色器修复通过现有变换展开动态贴图偏移，并声明无格式存储图像读写所需的 SPIR-V 能力；只读 SSA 分析另导出保守的贴图采样 LOD / cube 层选择器。
`dm/gfx/tools/dmsh_xlate` 的着色器翻译工具引用它们，把 Switch 原版着色器翻译成 SPIR-V。许可证 MIT，见 `LICENSE.txt`。
