# ZMK Runtime Macro

独立的 ZMK external module 预研项目：运行时通过可选的模块自有 USB HID CLI 修改宏槽位，按键绑定触发槽位中的 ASCII 文本。

> 当前阶段：阶段 4（协议核心与可选 USB HID transport）已实现；Python CLI 属于阶段 5，真实硬件验证属于阶段 6。

## 目标

- 提供 `&runtime_macro <slot>` custom behavior。
- 将槽位内容保存到 Zephyr Settings/NVS，而不是写入 devicetree。
- 由本模块自行实现可选的 USB HID 命令通道，提供 `list/get/set/clear`。
- 第一版仅支持 US ASCII，暂不支持 Unicode。
- 先使用 Python 脚本验证协议，后期再使用 Tauri 重写客户端。
- 不修改 ZMK 主仓库、ZMK Studio 或 `zmk-studio-messages`。

## 文档

- [调研接口与架构](docs/RESEARCH.md)
- [协议与 USB HID 映射](docs/PROTOCOL.md)
- [初步分步计划](docs/PLAN.md)

## 当前状态

阶段 1–5 已完成：本仓库是可编译的 Zephyr external module，并提供 `&runtime_macro <slot>` behavior、固定 RAM 槽位、公开的 `get/get_length/set/clear` API、`runtime_macro/slot/<n>` Settings handler、ASCII 执行器、传输无关的 32-byte 协议、可选的模块自有 USB HID transport，以及 Python 验证客户端。槽位文本限制为 printable US ASCII 和 `\n`、`\t`、`\b`，并在 set/load 时校验；set/clear 会先更新 RAM，再调用 Settings 持久化接口。按下 behavior 后由单个 delayable work 按字符发送 press/release 事件，并使用 busy 状态拒绝重复触发；事件通过 ZMK 正常 keycode pipeline 发送。

启用 behavior 需要 `CONFIG_SETTINGS=y`；使用 NVS 持久化时，目标 board 还需要有效的 `storage_partition`、`CONFIG_SETTINGS_NVS=y`、`CONFIG_NVS=y` 和相应的 Flash 依赖。

启用 USB transport：

```conf
CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y
```

该选项默认关闭，只在 `CONFIG_ZMK_USB=y`、legacy `CONFIG_USB_DEVICE_STACK=y` 且 unibody 或 split central 构建中可用。模块使用第二个 HID interface（HID_1），保留 ZMK 键盘 HID_0；Kconfig 会给出 `CONFIG_USB_HID_DEVICE_COUNT=2` 和 `CONFIG_HID_INTERRUPT_EP_MPS=32` 的安全默认。不要启用 `CONFIG_ENABLE_HID_INT_OUT_EP`，因为它是所有 HID interface 共用的设置并会改变 HID_0。USB transport 的完整映射和 control `SET_REPORT` / interrupt `IN` 方向见 [docs/PROTOCOL.md](docs/PROTOCOL.md)。

对于无线 split，宏槽位和 USB transport 运行在 dongle 的 split central：电脑只通过 USB 连接 dongle，slots 保存到 dongle 的 NVS；左右 peripheral 不分别保存或提供该 USB 接口。阶段 4 已完成容器 clean build、host 测试，以及使用 ZMK 官方 `studio-rpc-usb-uart` snippet 的 CDC ACM transport 共存 clean build；Python 客户端的 fake-HID 测试已完成，但尚未进行真实硬件或 actual host/hidapi round trip，后者属于阶段 6。

模块边界：宏槽位、Settings/NVS、ASCII 执行器和 behavior 属于核心功能，不能依赖 USB HID；模块自有 USB HID transport 是可选层。

## 阶段 5 Python 客户端

建议在电脑上用虚拟环境安装客户端依赖：

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
```

客户端按 vendor Usage Page `0xff60` / Usage `0x61` 查找模块的 HID_1，不会误连键盘 HID_0：

```sh
python3 tools/runtime_macro_cli.py list
python3 tools/runtime_macro_cli.py get 0
python3 tools/runtime_macro_cli.py get 0 --raw > slot-0.txt
printf 'Hello\n' | python3 tools/runtime_macro_cli.py set 0 --stdin
python3 tools/runtime_macro_cli.py set 0 --file slot-0.txt
cat slot-0.txt | python3 tools/runtime_macro_cli.py set 0 --stdin
python3 tools/runtime_macro_cli.py clear 0
```

`--text` 按字面读取 ASCII，不解释反斜杠转义；需要换行、Tab 或 Backspace 时，请使用 `--file`、`--stdin`（如上面的 `printf`）或 Bash 的 `$'Hello\n'` 语法。全局设备选项可放在子命令前：`--path PATH` 精确选择设备，或用 `--vid 0x1234 --pid 0x5678` 缩小匹配范围；`--timeout-ms` 和 `--retries` 控制超时与可恢复传输重试。多个 HID_1 匹配时必须使用 `--path`。`get` 默认转义换行、Tab、Backspace；`--raw` 才输出原始槽位 bytes。

Linux 若提示未找到或无法打开设备，先检查 `hidraw` 节点权限和 `udevadm info -q property -n /dev/hidrawN`。不要长期使用全局 `chmod 666`；可按实际枚举到的 VID/PID 创建规则，例如：

```udev
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", GROUP="plugdev"
```

VID/PID 可能被用户固件覆盖，示例值不是固定产品 ID。重新加载 udev 规则并重新插拔 dongle 后再测试。

## 阶段 6 手工流程（尚未执行）

连接并刷写启用本模块的 dongle 后，依次运行 `list` → `set` → `get`，按实体 `&runtime_macro <slot>` 按键确认输出，再测试 USB 重插、dongle 重启后的 NVS 恢复，最后运行 `clear` 并用 `get` 确认为空。真实设备、权限和 NVS 测试均不属于阶段 5。
