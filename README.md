# ZMK Runtime Macro

独立的 ZMK external module 预研项目：运行时通过可选的模块自有 USB HID CLI 修改宏槽位，按键绑定触发槽位中的 ASCII 文本。

> 当前阶段：阶段 3（ASCII 执行器）已实现；USB HID/Raw HID、协议和 Python CLI 尚未实现。

## 目标

- 提供 `&runtime_macro <slot>` custom behavior。
- 将槽位内容保存到 Zephyr Settings/NVS，而不是写入 devicetree。
- 由本模块自行实现可选的 USB HID 命令通道，提供 `list/get/set/clear`。
- 第一版仅支持 US ASCII，暂不支持 Unicode。
- 先使用 Python 脚本验证协议，后期再使用 Tauri 重写客户端。
- 不修改 ZMK 主仓库、ZMK Studio 或 `zmk-studio-messages`。

## 文档

- [调研接口与架构](docs/RESEARCH.md)
- [初步分步计划](docs/PLAN.md)

## 当前状态

阶段 1、2、3 已完成：本仓库是可编译的 Zephyr external module，并提供 `&runtime_macro <slot>` behavior、固定 RAM 槽位、公开的 `get/get_length/set/clear` API，以及 `runtime_macro/slot/<n>` Settings handler。槽位文本限制为 printable US ASCII 和 `\n`、`\t`、`\b`，并在 set/load 时校验；set/clear 会先更新 RAM，再调用 Settings 持久化接口。按下 behavior 后由单个 delayable work 按字符发送 press/release 事件，并使用 busy 状态拒绝重复触发；事件通过 ZMK 正常 keycode pipeline 发送。当前实现不支持并发宏，不包含 USB HID、协议或 CLI。设计面向支持 USB 和 Settings/NVS 的 ZMK split central 构建，具体依赖版本由使用方的构建清单决定。

启用该 behavior 需要 `CONFIG_SETTINGS=y`；使用 NVS 持久化时，目标 board 还需要有效的 `storage_partition` 及 `CONFIG_SETTINGS_NVS=y` 等 Zephyr 依赖。

模块边界：宏槽位、Settings/NVS、ASCII 执行器和 behavior 属于核心功能，不能依赖 USB HID；模块自有 USB HID CLI 是可选的传输层。
