# Switch 2 Pro 原生 BLE 输入与双频震动

macOS 宿主提供可选的 Switch 2 Pro Controller 原生 BLE 适配。设置 `DM_NS2_PRO=ble` 后，游戏直接读取手柄输入，并将每侧的低频幅度、低频频率、高频幅度和高频频率编码到 Nintendo 原生震动报文。连接由 CoreBluetooth 管理，不经过 WaveBird、虚拟 Xbox 手柄或 Core Haptics 的强度合并。

这是本项目内的实验性适配，不是 macOS 通用手柄驱动，也不代表已实现 HD Rumble 2 的全部功能。真实手柄探针验证了输入、独立左右侧输出，以及同一侧同时输出两个频率；随后在实际游戏中验证了进入关卡、跳跃和双侧双频指令提交。用户确认初始设置下震感更细、更舒服，但力度偏弱；当前默认两倍增益尚待手感确认，原机力度匹配也未验证。平台测试范围沿用 [项目说明](../README.zh-CN.md#说明)。

## 构建与启用

先完成 [macOS 构建指南](build-macos.zh-CN.md)，准备游戏资源、改写模块和着色器。在仓库根目录的 Bash 会话中构建宿主，并将实验用宿主单独保存：

```sh
bash scripts/prepare-mac.sh host
source scripts/mac-env.sh
mkdir -p "$SMBW_WORKSPACE/build/ns2-pro"
cp dm/host/dmhost "$SMBW_WORKSPACE/build/ns2-pro/dmhost"
```

1. 退出 WaveBird，以及其他正在连接同一手柄的桥接程序。
2. 长按手柄顶部的 SYNC 键，直到指示灯循环闪烁，进入发现模式。
3. 启动实验宿主；首次连接时允许运行程序访问蓝牙。

```sh
DM_NS2_PRO=ble \
SMBW_HOST="$SMBW_WORKSPACE/build/ns2-pro/dmhost" \
SMBW_LOG="$SMBW_WORKSPACE/run/ns2-pro.log" \
bash scripts/run-mac.sh
```

日志中的 `NS2 Pro BLE ready` 表示输入订阅、工厂摇杆校准读取和初始化命令确认已完成。第八个初始化步骤设置玩家 1 灯，收到对应 ACK 后才订阅游戏输入并进入 ready 状态。该命令使用 `09/07` 和灯位掩码 `01`；这次修复后的实体灯状态尚待确认。连接失败时查看日志中的 `【红】NS2 Pro BLE` 信息；检查蓝牙电源、应用蓝牙权限、SYNC 发现状态，以及是否还有桥接程序占用手柄。

原生路径的力度由 `DM_NS2_RUMBLE_GAIN` 控制，默认 `2`，接受 `0..4` 的有限数值。设为 `1` 可恢复初始测试的幅码比例，设为 `0` 则输出零幅度；修改后重新启动宿主。原有 `DM_RUMBLE_GAIN` 不影响这条原生 BLE 路径。

该模式独占宿主的实体手柄输入来源。取消 `DM_NS2_PRO` 后，宿主使用原有 GameController 路径。实验期间使用独立宿主与日志；重新运行打包脚本才会更新本地启动应用。

## 停止与恢复

窗口失焦时，宿主清除按键和待发震动，并请求双侧停止；恢复焦点后等待游戏的新指令。正常关闭窗口或退出游戏也会请求发送停止报文。断连时清除输入、连接级校准与握手状态，以及待发震动；保留游戏客体的一次性设备初始化。重连后重新读取校准并完成握手，先发送零幅度，再接受新的震动指令，旧效果不会重播。

`shutdown ... submitted`、`rumble` 日志及 `GetActualVibrationValue` 反映软件提交状态。BLE 震动通道使用无响应写入，这些信息不是电机遥测，不能单独证明物理电机已经停止。本轮测试另外取得了用户对手柄实际停止震动的确认；异常终止和各种断连情形仍需分别验证。

恢复 WaveBird 使用时，先退出游戏，再取消环境变量并启动 WaveBird：

```sh
unset DM_NS2_PRO
```

如 WaveBird 未自动连接，按 SYNC 重新进入发现模式，由 WaveBird 连接。原生适配的初始化不包含改写配对密钥的步骤，保留已有的 WaveBird 配对记录即可。若程序异常结束后手柄仍在震动，可按 SYNC 中断当前连接，再重新连接。

## 编码与实测

游戏侧每个震动设备保留四个量：`low_amp`、`low_freq`、`high_amp`、`high_freq`。低、高两个频段在同一侧独立编码；两侧组成一个 42 字节 BLE 震动报文，持续输出的目标发送周期为 15 ms。报文使用滚动序号，发送受阻时保留最新状态，停止请求优先处理。

单个操作包含四个连续的 10 位字段，按小端序写为 5 字节：

```text
bits  0..9   low frequency code
bits 10..19  low amplitude code
bits 20..29  high frequency code
bits 30..39  high amplitude code
```

频率按下式量化：

\[
c=\operatorname{round}\!\left(1+96\log_2(f/10)\right),
\qquad f=10\cdot2^{(c-1)/96}.
\]

公开的真实主机 USB 捕获中，常见频码符合经典 Switch 对数频率索引的三倍分辨率；其中空闲频码 `385/481` 对应 `160/320 Hz`。2026 年 10 月 18 日，本项目用一台真实 Switch 2 Pro Controller 和麦克风录音检验该推导：

| 输出频段 | 原始频码 | 公式预测 | 录音测得峰值 |
| --- | ---: | ---: | ---: |
| LF | 225 | 50.397 Hz | 50.345 Hz |
| LF | 321 | 100.794 Hz | 100.873 Hz |
| LF | 417 | 201.587 Hz | 201.730 Hz |
| LF | 289 | 80 Hz | 79.975 Hz |
| LF | 385 | 160 Hz | 160.121 Hz |
| LF | 481 | 320 Hz | 320.242 Hz |
| HF | 385 | 160 Hz | 160.183 Hz |
| HF | 481 | 320 Hz | 320.244 Hz |
| HF | 577 | 640 Hz | 640.491 Hz |

同一侧的双频探针在同一录音窗口内出现 `160.121 Hz` 和 `320.242 Hz` 两个峰；右侧单独输出测得 `160.127 Hz`。频码 `577` 的结果说明第十位属于频率字段，不能将其截断为 9 位或单独当作使能位。

当前声学实测覆盖约 **50–640 Hz**。编码器接受的频率范围为 `40–1252 Hz`，但低于约 50 Hz 和高于 640 Hz 的部分尚未测量，不能据此声称整个范围都已验证。第一轮探针与录音使用了不同的时钟来源，分析按实际频谱活动选择窗口；第二轮统一使用主机时钟对齐。录音频谱验证了载频，未测量振动力、机械位移或完整瞬态响应。

### 幅度与限制

当前每个频段采用线性幅码：

\[
a_{\text{code}}=\operatorname{round}\!\left(192\cdot g\cdot\operatorname{clamp}(a,0,1)\right),
\qquad g\in[0,4],\quad g_{\text{default}}=2.
\]

`192` 是本轮声学测量覆盖的幅码上限，不是 Nintendo 主机的绝对力度校准结果。初始游戏测试使用 `g=1`；根据力度偏弱的反馈，当前默认改为 `g=2`，最大幅码为 `384`，允许设置范围内的最大幅码为 `768`。高于 `192` 的幅码尚未完成声学标定，默认两倍增益也尚未取得用户手感确认。

增益对两频段使用同一个线性比例，实现不对两个频段做 RSS 合并，也不使用平方根曲线放大弱震动。幅码量化仍会损失很小的输入：默认 `g=2` 时，归一化幅度小于约 `0.0013` 可能量化为零；`g=1` 时该阈值约为 `0.0026`。

固定 `160 Hz` 时，幅码 `24/48/96/192` 的录音基频幅度，相对幅码 `96` 分别为 `0.227/0.463/1/1.979`，接近线性的 `0.25/0.5/1/2`。这组数据比“经典 Switch 幅度索引乘 4”的候选更符合线性关系，支持当前线性实现。

这还不是完整的幅度标定。同轮较早和较晚的两个 `160 Hz / 幅码96` 录音幅度相差约 **76%**，握持、位置、声学传递或录音增益的变化尚未分离。下一轮应固定位置、交错重复参考幅码，再检验弱幅度和持续输出。当前未证明与原主机的绝对力度、频率补偿或触觉体验一致，也未把麦克风声压当成电机振动力。

### 游戏集成与软件检查

独立测试应用从存档副本启动后，真实手柄可进入云朵关卡并操作 Mario 跳跃。游戏运行日志记录了左右侧持续提交的双频值，例如 `低频0.0059 @178.30 Hz / 高频0.0088 @281.00 Hz`，后续也出现了 `190.27/327.01 Hz` 的载频组合。这确认了游戏指令进入原生 BLE 路径。用户随后反馈初始增益下震感更细、更舒服，但仍偏弱；这不构成与原机力度一致的证据。

编码回归检查可在外部工作区运行：

```sh
source scripts/mac-env.sh
make -C dm/host/tests ns2 B="$SMBW_WORKSPACE/build/ns2-pro-tests"
```

该入口的 ASan/UBSan 检查已实际运行通过。编码检查、真实手柄探针、游戏输入观察和 BLE 提交日志分别验证不同层面，不替代完整关卡与触觉体验测试。

## 为什么没有直接采用现成桥接

本轮检查的方案中，没有找到已证明能够从游戏四个输入量完整还原真实双频输出的通用适配器：

- **WaveBird Xbox 模式**接收普通双马达强度；频率由桥接设置生成，游戏的独立频率信息无法经此路径传递。
- **WaveBird Switch Pro 模式**虽接收 NS1 HD Rumble 字节，当前实现对每侧两频段幅值取最大值，并丢弃输入频率。
- **WaveBird NS2 raw passthrough、procon2-mac**可转发已编码的原生 NS2 帧，但原始帧转发本身不提供游戏浮点 Hz、幅度到 NS2 字段的完整转换。procon2-mac 的 USB 测试图案同样不能证明游戏指令转换正确。
- **SDL Switch 2 驱动**提供原生打包和 USB 初始化参考，但普通 rumble 接口使用固定载频，不能直接承载游戏每侧独立变化的双频指令。
- **S2P-XInput-Lite、[Switch2Connect](https://github.com/TommyWabg/Switch2Connect)、Karakuri**提供了有用的协议和实现线索，也使用了不同的强度曲线、固定载频或经验映射。本轮未找到它们附带的完整 Hz、幅度及原机响应校准证据。

这些结论针对本轮读到的实现；后续版本需重新核对。

## 来源与实现位置

- [ndeadly：Switch 2 HID reports](https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md)：BLE 特征、42 字节报文及左右组布局；同仓库的 `captures/usb/rumble-procon-gccon.pcapng.gz` 提供真实主机输出捕获。
- [ndeadly：玩家灯命令](https://github.com/ndeadly/switch2_controller_research/blob/master/commands.md#command-0x09---player-leds)、[官方软件的 BLE 初始化捕获](https://github.com/ndeadly/switch2_controller_research/blob/master/bluetooth_interface.md#initialisation-sequence)：`09/07` 灯位掩码请求及成功 ACK；WaveBird 的 [命令定义](https://github.com/murphyjt/wavebird/blob/main/WaveBird/Core/NS2Commands.swift)也使用同一报文。
- [dekuNukem：经典 Switch 频率和幅度表](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/rumble_data_table.md)：对数频率索引与幅度候选的比较依据。
- [WaveBird](https://github.com/murphyjt/wavebird)：macOS BLE 初始化与桥接流程参考，MIT 许可。
- [procon2-mac](https://github.com/caqlayan/procon2-mac)：macOS USB/BLE 传输参考，MIT 许可。
- [SDL Switch 2 驱动](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_switch2.c)：USB 初始化和原生打包参考，zlib 许可。
- [S2P 幅码与频码封装](https://github.com/duoduo-88/S2P-XInput-Lite/blob/main/src/rumble_protocol.py)、[实机扫频工具](https://github.com/duoduo-88/S2P-XInput-Lite/blob/main/tests/live_rumble_sweep.py)、[Karakuri 独立逆向说明](https://github.com/eggletric/karakuri-firmware/blob/main/firmware/pico_switch_pad/procon2_dongle.h)：候选模型和验证范围的比较资料。
- [Nintendo 振动控制专利 JP2026015701A](https://patents.google.com/patent/JP2026015701A/en)：双指令、频率相关幅度调整和插值的设计背景；专利没有给出本实现所需的完整线缆编码或设备校准表。

仓库内的 BLE 会话位于 [`front_ns2_mac.m`](../dm/host/front_ns2_mac.m)，编码位于 [`ns2_rumble.c`](../dm/host/ns2_rumble.c)，宿主接口位于 [`ns2.h`](../dm/host/ns2.h)。第三方版权与许可声明见源文件和 [`docs/licenses`](licenses)。
