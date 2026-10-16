# Shader translator third-party notices / 着色器翻译工具第三方声明

This directory contains the pinned shader translator and its managed dependencies, not game shaders. Keep the license files linked below with redistributions of these binaries. Versions come from `dmsh_xlate.deps.json` and the corresponding NuGet package metadata.

本目录包含固定版本的着色器翻译工具及其托管依赖，不包含游戏着色器。分发这些二进制时应一并保留下列许可文件。版本依据本目录的 `dmsh_xlate.deps.json` 及对应 NuGet 包元数据。

| Component / 组件 | Version / 版本 | Source / 来源 | License / 许可 |
|---|---|---|---|
| Ryujinx.Common, Ryujinx.Graphics.Shader | Ryujinx 1.3.2 with local modifications / 含本地修改 | [vendored source and change notes](../../../third_party/ryujinx/README.md) | [MIT](../../../third_party/ryujinx/LICENSE.txt) |
| Spv.Generator | Vendored Ryujinx subset / 仓内 Ryujinx 子集 | [source](../../../third_party/ryujinx/src/Spv.Generator) | [MIT](../../../third_party/ryujinx/src/Spv.Generator/LICENSE) |
| Gommon | 2.8.1.2 | [official source, package commit](https://github.com/GreemDev/Gommon/tree/222084874ac473e02f27faeb10e540f926702976) | [MIT](../../../../docs/licenses/Gommon-LICENSE.txt) |
| Humanizer.Core and all bundled locale resource assemblies / 及全部随附语言资源程序集 | 3.0.10 | [official source, package commit](https://github.com/Humanizr/Humanizer/tree/90f0cb1d7427635f38b46350bb1dc5fca2164f2d) | [MIT and included attributions](../../../../docs/licenses/Humanizer-LICENSE.txt) |
| Microsoft.IO.RecyclableMemoryStream | 3.0.1 | [official source, package commit](https://github.com/microsoft/Microsoft.IO.RecyclableMemoryStream/tree/e29a28387da9018fa9605a1dcb3f7a0435aa9974) | [MIT](../../../../docs/licenses/RecyclableMemoryStream-LICENSE.txt) |
| MsgPack.Cli (`MsgPack.dll`) | 1.0.1 | [official release source](https://github.com/msgpack/msgpack-cli/tree/1.0.1) | [Apache-2.0](../../../../docs/licenses/MsgPack.Cli-LICENSE.txt) |
| System.CodeDom | 4.4.0 | [official NuGet package](https://www.nuget.org/packages/System.CodeDom/4.4.0) | [MIT](../../../../docs/licenses/System.CodeDom-LICENSE.txt), [package third-party notices](../../../../docs/licenses/System.CodeDom-THIRD-PARTY-NOTICES.txt) |

MsgPack.Cli: Copyright 2010-2016 FUJIWARA, Yusuke, all rights reserved. This attribution is preserved from the 1.0.1 NuGet package metadata. No `NOTICE` file is present in that release's repository root or NuGet package.

MsgPack.Cli 的上述版权声明来自 1.0.1 NuGet 包元数据；该版本仓库根目录及 NuGet 包内没有 `NOTICE` 文件。

The Gommon license and both System.CodeDom notices are copied from the exact NuGet packages. Humanizer and RecyclableMemoryStream licenses are copied from the commits recorded in their package metadata; the MsgPack.Cli license is copied from tag `1.0.1`. License text is retained in its original language. The top-level repository license does not replace these licenses.

Gommon 及 System.CodeDom 的许可文件复制自上述确切版本的 NuGet 包；Humanizer 与 RecyclableMemoryStream 的许可复制自包元数据记录的提交；MsgPack.Cli 的许可复制自 `1.0.1` 标签。许可正文保留原文，仓库顶层许可证不替代这些许可。
