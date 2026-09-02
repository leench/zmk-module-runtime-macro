# ZMK Runtime Macro

独立的 ZMK external module 预研项目：运行时通过可选的模块自有 USB HID CLI 修改宏槽位，按键绑定触发槽位中的 ASCII 文本。

> 当前阶段：阶段 1（模块骨架与最小 behavior）已实现，尚未实现 Settings/NVS、ASCII 执行器、USB HID/Raw HID、协议或 Python CLI。

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

阶段 1 已完成：本仓库已是一个可编译的 Zephyr external module（module metadata、`CMakeLists.txt`、`Kconfig`），并提供最小的 `&runtime_macro <slot>` behavior——校验 slot 范围后仅记录日志占位，不执行任何文本。其余功能（Settings/NVS 持久化、ASCII 执行器、模块自有 USB HID/Raw HID 通道、协议和 Python CLI）尚未实现，属于后续阶段。设计面向支持 USB 和 Settings/NVS 的 ZMK split central 构建，具体依赖版本由使用方的构建清单决定。

模块边界：宏槽位、Settings/NVS、ASCII 执行器和 behavior 属于核心功能，不能依赖 USB HID；模块自有 USB HID CLI 是可选的传输层。
