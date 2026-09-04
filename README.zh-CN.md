# ZMK Runtime Macro

[English README](README.md)

一个不修改 ZMK 主仓库的 Zephyr/ZMK external module：通过
`&runtime_macro <slot>` 触发保存于 Settings/NVS 的 ASCII 文本，并将文本按键盘事件逐字符发送。

## 功能

- 固定数量的 runtime macro slots。
- 使用 Zephyr Settings API 持久化；可配合 NVS 保存到设备 Flash。
- 支持完整 printable US ASCII（`0x20–0x7e`）以及换行（LF）、Tab 和 Backspace。
- 通过 ZMK 正常的 keycode event pipeline 发送按键，不直接构造键盘 HID report。
- 单个 `k_work_delayable` 执行器；宏执行期间再次触发返回 `-EBUSY`，不会排队。
- 可选的模块自有 USB HID 配置接口（第二个 HID interface，HID_1）。
- Python `hidapi` 客户端，支持 `list`、`get`、`set` 和 `clear`。
- 首次初始化时，为没有持久化值的 slot 设置默认内容 `Runtime Macro 1`、`Runtime Macro 2`……；已有值不会被覆盖，执行 `clear` 后也不会在重启时恢复默认值。

不支持 Unicode、中文、Emoji、自动键盘布局转换或多个宏并发执行。

## 安全注意事项

本模块没有针对敏感信息做任何安全方面的考量：USB HID 配置通道没有认证、授权或加密，能够访问设备 HID interface 的本机程序可以读取和修改 slots；slot 内容还可能持久化到设备 Flash/NVS，并在触发时作为键盘输入输出。因此不建议使用本模块保存或发送用户名、密码、OTP、令牌、密钥或其他敏感信息。请仅用于非敏感文本，并在不可信主机上禁用 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`。

## 加入 ZMK 构建

将本仓库作为 `ZMK_EXTRA_MODULES` 加入目标构建。例如：

```sh
west build -b <board> -- \
  -DZMK_CONFIG=/path/to/your/zmk-config \
  -DZMK_EXTRA_MODULES=/path/to/zmk-module-runtime-macro
```

如果构建同时使用其他 external module，请按构建环境的 CMake 规则传入多个模块路径。本项目不要求修改 ZMK 主仓库。

## Keymap 配置

在使用 behavior 的 keymap 中显式包含 DTS 文件：

```dts
#include <behaviors/runtime_macro.dtsi>
```

然后绑定 slot：

```dts
&runtime_macro 0
&runtime_macro 1
```

`<slot>` 从 `0` 开始，最大值由 `CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT` 决定。使用 split keyboard 时，behavior、slot、Settings/NVS 和 USB 配置接口运行在 split central；左右 peripheral 继续上报按键位置。

## Kconfig

核心 behavior 需要 Settings：

```conf
CONFIG_SETTINGS=y
CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT=8
CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN=64
```

使用 NVS 时，目标 board 还必须提供有效的存储分区，并启用相应的 Flash 配置，例如：

```conf
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y
CONFIG_FLASH_MAP=y
```

启用可选 USB HID 配置接口：

```conf
CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y
```

该选项只适用于启用了 USB 的 unibody 或 split central 构建。它使用 HID_1，保留 ZMK 键盘的 HID_0；模块默认使用两个 HID instance 和 32-byte interrupt-IN endpoint。不要启用 `CONFIG_ENABLE_HID_INT_OUT_EP`，因为它会同时影响 HID_0。

执行速度使用 ZMK 的全局宏配置：

```conf
CONFIG_ZMK_MACRO_DEFAULT_TAP_MS=30
CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS=15
```

`TAP_MS` 是每个字符保持按下的时间，`WAIT_MS` 是释放后到下一个字符按下的等待时间。它们是编译时的全局配置，不是每个 slot 独立配置，也不能由 Python 客户端修改。

## Python 客户端

客户端需要 Python 3 和 PyPI `hidapi`：

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
```

完整命令、参数、Python module API、协议错误和 Linux 设备权限说明见 [`docs/CLI.md`](docs/CLI.md)。固定 32-byte 协议和 USB HID 映射见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)。

常用命令：

```sh
python3 tools/runtime_macro_cli.py list
python3 tools/runtime_macro_cli.py get 0
python3 tools/runtime_macro_cli.py get 0 --raw > slot-0.txt
python3 tools/runtime_macro_cli.py set 0 --text 'Hello'
printf 'Hello\n' | python3 tools/runtime_macro_cli.py set 0 --stdin
python3 tools/runtime_macro_cli.py set 0 --file slot-0.txt
python3 tools/runtime_macro_cli.py clear 0
```

全局选项必须放在子命令之前。连接多个兼容设备时，使用设备枚举得到的 path 精确选择：

```sh
python3 tools/runtime_macro_cli.py --path "$HID_PATH" list
```

`--text` 按字面读取 ASCII，不解释反斜杠；需要 LF、Tab 或 Backspace 时使用 `--stdin`、`--file` 或 shell 的 ANSI-C quoting。某些 Linux `hidapi` 后端不会返回解析后的 Usage Page/Usage，此时使用明确的 `--path`，不要自动猜测设备。

## 协议和接口

- wire protocol：[`docs/PROTOCOL.md`](docs/PROTOCOL.md)
- Python CLI 和 Python module API：[`docs/CLI.md`](docs/CLI.md)
- C API：[`include/zmk/runtime_macro.h`](include/zmk/runtime_macro.h)
- protocol C API：[`include/zmk/runtime_macro_protocol.h`](include/zmk/runtime_macro_protocol.h)

协议命令为 `LIST`、`GET`、`SET`、`CLEAR`。长文本使用 22-byte payload 分块；`SET` 在最后一块收到之前不会修改 slot。

## 测试

Host 测试：

```sh
./tests/host/run.sh
python3 -m unittest discover -s tests/python -v
python3 -m py_compile tools/runtime_macro_cli.py
```

Host 测试覆盖 slot/Settings、ASCII 映射、异步执行器、协议核心和 USB transport stub；Python 测试使用 fake HID device。实际 board 构建需要目标 ZMK 工程和容器化 Zephyr 环境。

## 当前状态

阶段 1–5 已完成并提交。阶段 6 已在一个兼容的 central 设备上验证 HID round trip、slot 读写和协议响应；实际按键输出、USB 重插、重启后的 NVS 保留以及其他 board/主机平台仍需验证。该状态不代表所有设备和平台均已通过。

## 隐私和公开仓库注意事项

本项目文档和测试不得包含：

- 本机绝对路径、工作区路径或容器路径；
- USB 设备序列号、个人设备标识或真实 hidraw path；
- 私有仓库地址、访问令牌、日志中的用户数据或未公开配置。

示例中的路径、VID/PID、设备名和序列号都必须使用占位符。真实设备信息只应保留在本地测试记录中。

## 计划

历史阶段和未完成项目见 [`docs/PLAN.md`](docs/PLAN.md)。后续可在保持 v1 wire protocol 兼容的前提下开发图形客户端；Unicode、ZMK Studio 新 RPC 和 ZMK 主仓库修改不在当前范围内。
