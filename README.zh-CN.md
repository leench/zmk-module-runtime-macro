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
- 可选的模块自有 USB HID 配置接口（默认使用 HID_1；如果已有模块占用，也可以改用其他 HID instance）。
- Python `hidapi` 客户端，支持 v2 `auth-info`、`login`、`set-password`、`lock`、
  `list`、`get`、`set` 和 `clear`。
- 首次初始化时，为没有持久化值的 slot 设置默认内容 `Runtime Macro 1`、`Runtime Macro 2`……；已有值不会被覆盖，执行 `clear` 后也不会在重启时恢复默认值。

不支持 Unicode、中文、Emoji、自动键盘布局转换或多个宏并发执行。

## 安全注意事项

当前 USB HID 管理通道使用 v2 认证协议。新固件没有密码时处于 `OPEN`，可以一直不设置密码；设置非空密码后进入 `PROTECTED`，`LIST`、`GET`、`SET` 和 `CLEAR` 必须先通过 challenge-response 登录。协议没有清除密码的命令；忘记密码只能刷 settings-reset 固件，且该操作会清除包含宏 slot 和密码记录在内的 Settings 数据。

认证不会加密 USB 流量，也不保护宏触发时作为键盘输入输出的文本。需要限制本机 HID 管理访问时请设置密码；不可信主机应禁用 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`。完整流程见 [`docs/CLI.md`](docs/CLI.md) 和 [`docs/AUTHENTICATION_PROTOCOL.md`](docs/AUTHENTICATION_PROTOCOL.md)。

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

该选项只适用于启用了 USB 的 unibody 或 split central 构建。默认使用 HID_1，保留 ZMK 键盘的 HID_0。如果已有其他模块占用 HID_1，可以通过 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE` 指定未占用的 HID instance，并同步增加 `CONFIG_USB_HID_DEVICE_COUNT`。传输需要 32-byte interrupt-IN endpoint。不要启用 `CONFIG_ENABLE_HID_INT_OUT_EP`，因为它会同时影响 HID_0。

例如显示屏的 Raw HID 已占用 HID_1 时：

```conf
CONFIG_USB_HID_DEVICE_COUNT=3
CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE="HID_2"
```

执行速度使用 runtime macro 独立配置，默认跟随 ZMK 对应的全局宏配置：

```conf
CONFIG_ZMK_RUNTIME_MACRO_TAP_MS=30
CONFIG_ZMK_RUNTIME_MACRO_WAIT_MS=15
```

`TAP_MS` 是每个字符保持按下的时间，`WAIT_MS` 是释放后到下一个字符按下的等待时间。它们是编译时配置，不是每个 slot 独立配置，也不能由 Python 客户端修改。如果要匹配静态宏中明确设置的 `tap-ms = <0>` 和 `wait-ms = <0>`，将这两个配置都设为 `0`。这不会修改全局的 `CONFIG_ZMK_MACRO_DEFAULT_*` 配置。

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
python3 tools/runtime_macro_cli.py auth-info
# OPEN 设备可选：设置非空密码并重新认证。
python3 tools/runtime_macro_cli.py set-password
# PROTECTED 设备在管理命令前先登录。
python3 tools/runtime_macro_cli.py login
python3 tools/runtime_macro_cli.py lock
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

当前 v2 协议包含 `LIST`、`GET`、`SET`、`CLEAR`，以及 `AUTH_INFO`、`AUTH_CHALLENGE`、`AUTH_PROVE`、`PASSWORD_SET` 和 `LOCK`。长文本使用 22-byte payload 分块；`SET` 在最后一块收到之前不会修改 slot。密码凭据使用独立的 52-byte 分块 `PASSWORD_SET` 事务。

## 测试

Host 测试：

```sh
./tests/host/run.sh
python3 -m unittest discover -s tests/python -v
python3 -m py_compile tools/runtime_macro_cli.py
```

Host 测试覆盖 slot/Settings、ASCII 映射、异步执行器、协议核心和 USB transport stub；Python 测试使用 fake HID device。实际 board 构建需要目标 ZMK 工程和容器化 Zephyr 环境。

## 当前状态

认证核心、v2 协议/USB 生命周期接入和参考 Python 客户端已经完成并提交。一个兼容的 central 设备已完成未经认证的 HID round trip、slot 读写检查；实体密码登录、USB 重插、重启后的 NVS 保留、实际按键输出以及其他 board/主机平台仍需验证。当前状态不声称实体 USB 认证已经验证，也不代表所有设备和平台均已通过。

## 隐私和公开仓库注意事项

本项目文档和测试不得包含：

- 本机绝对路径、工作区路径或容器路径；
- USB 设备序列号、个人设备标识或真实 hidraw path；
- 私有仓库地址、访问令牌、日志中的用户数据或未公开配置。

示例中的路径、VID/PID、设备名和序列号都必须使用占位符。真实设备信息只应保留在本地测试记录中。

## 计划

历史阶段和未完成项目见 [`docs/PLAN.md`](docs/PLAN.md)。后续图形或后台客户端应复用文档化的 v2 wire protocol 和认证流程；Unicode、ZMK Studio 新 RPC 和 ZMK 主仓库修改不在当前范围内。
