#!/bin/bash
# 本机共享路径；游戏数据、密钥和日志都保存在 Git 仓库外。
# Shared local paths; game data, keys, and logs stay outside the Git repository.
SMBW_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SMBW_WORKSPACE="${SMBW_WORKSPACE:-$(dirname "$SMBW_REPO")/smbw-mac-work}"
export SMBW_VERSION="v327680"
export DM_ROOT="$SMBW_WORKSPACE"
export DM_GAME="smbw"
export DOTNET_ROOT="$(brew --prefix dotnet)/libexec"
export DOTNET_CLI_HOME="$SMBW_WORKSPACE/build/dotnet"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export PATH="$SMBW_WORKSPACE/.venv/bin:$PATH"
SMBW_PYTHON="$SMBW_WORKSPACE/.venv/bin/python"
