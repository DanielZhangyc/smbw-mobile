# macOS 构建与启动

先在仓库根目录运行 `bash`，再在该 Bash 会话中依次执行以下命令。游戏文件、密钥、提取结果和缓存均保存在仓库外；不要将其加入 Git 或 Release。

## 环境与输入

安装并选择 Xcode 27，确保 `xcrun -sdk macosx metal --version` 可执行。提取器目标框架为 .NET 10；着色器工具必须匹配 `dm/gfx/tools/dmsh_chain.lock`，不要用重新发布的翻译器覆盖仓库中的版本。

```sh
brew install dotnet python spirv-cross
export SMBW_WORKSPACE="$(dirname "$PWD")/smbw-mac-work"
mkdir -p "$SMBW_WORKSPACE"
python3 -m venv "$SMBW_WORKSPACE/.venv"
"$SMBW_WORKSPACE/.venv/bin/python" -m pip install -r scripts/requirements-mac.txt

# 改成你自己的文件路径；路径中有空格时保留引号。
BASE_NSP="/absolute/path/base.nsp"
UPDATE_NSP="/absolute/path/update-1.2.1.nsp"
PROD_KEYS="/absolute/path/prod.keys"
```

## 提取并构建

先提取本体，再叠加更新。着色器构建需要两个版本的输入。

```sh
source scripts/mac-env.sh
mkdir -p "$SMBW_WORKSPACE/build" "$SMBW_WORKSPACE/shader"
dotnet build dm/tools/nx_extract -c Release -o "$SMBW_WORKSPACE/build/nx_extract"
"$SMBW_WORKSPACE/build/nx_extract/nx_extract" "$PROD_KEYS" "$BASE_NSP" - \
  "$SMBW_WORKSPACE/data/v0"
"$SMBW_WORKSPACE/build/nx_extract/nx_extract" "$PROD_KEYS" "$BASE_NSP" "$UPDATE_NSP" \
  "$SMBW_WORKSPACE/data/v327680" "$SMBW_WORKSPACE/data/v0/RomFS"

bash scripts/prepare-mac.sh rewrite
bash scripts/prepare-mac.sh shaders
bash scripts/prepare-mac.sh host
bash scripts/run-mac.sh
```

`shaders` 会核对从输入生成的表及工具指纹。若不匹配，应先核对游戏版本与工具版本，不要跳过检查或直接更新锁文件。构建会更新本地生成的链版本头和验证表；提交时另行审查这些差异。首次编译耗时较长，完成后无需每次启动重编。

宿主从系统读取 `Arial Unicode.ttf`，并在工作区生成按钮图标字体；不分发系统字体。当前脚本要求系统字体位于 `/System/Library/Fonts/Supplemental/Arial Unicode.ttf`。

## 日常使用

后续在仓库根目录运行 `bash scripts/run-mac.sh` 即可；若工作区不在默认位置，先设置 `SMBW_WORKSPACE`。`run/save/` 是存档，`run/temp/` 是临时目录，`run/manual.log` 是启动日志。

可选运行 `bash scripts/package-mac.sh` 生成仅供本机使用的启动应用。它引用工作区绝对路径，不是可独立分发的游戏包。不要上传工作区、游戏着色器缓存、提取代码或 iOS 打包产物。
