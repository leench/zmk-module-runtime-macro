# ZMK Runtime Macro

独立的 ZMK external module 预研项目：运行时通过可选的模块自有 USB HID CLI 修改宏槽位，按键绑定触发槽位中的 ASCII 文本。

> 当前阶段：仅完成调研文档和初步计划，尚未实现固件、Python CLI 或可编译的 Zephyr module。

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

当前仓库仍只有调研文档，尚未实现 USB HID 设备、协议或 behavior。设计面向支持 USB 和 Settings/NVS 的 ZMK split central 构建，具体依赖版本由使用方的构建清单决定。

模块边界：宏槽位、Settings/NVS、ASCII 执行器和 behavior 属于核心功能，不能依赖 USB HID；模块自有 USB HID CLI 是可选的传输层。
