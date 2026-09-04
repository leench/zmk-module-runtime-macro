# Runtime Macro 初步计划

阶段 1–5 已完成；阶段 6 仍待实际 dongle/键盘验证。

已完成：

- 阶段 1：模块骨架、DTS behavior 和最小可编译版本。
- 阶段 2：固定 RAM 槽位、Settings handler、set/clear 持久化 API，以及 host 单元测试。
- 阶段 3：US ASCII 映射、delayable work 逐字符 press/release 执行、单宏 busy 策略，以及 host/native_sim 验证。
- 阶段 4：32-byte 传输无关协议、原子分块 SET、可选 legacy USB HID HID_1 transport，以及 central clean-build 验证。
- 阶段 5：Python hidapi 验证客户端、固定帧协议校验、超时/重试处理和 fake-HID 单元测试。

## 阶段 1：模块骨架与 behavior

- 建立 Zephyr module metadata、`CMakeLists.txt`、`Kconfig`。
- 增加 `runtime_macro` DTS behavior 和 binding。
- 用固定 slot 数量和最大文本长度完成最小可编译版本。
- 验证 `&runtime_macro 0` 可以被 keymap 识别并触发。

## 阶段 2：Settings/NVS

- 实现 `runtime_macro/slot/<n>` settings handler。
- 启动时加载到 RAM。
- 实现 `set`、`clear` 的保存和删除接口。
- 验证重启后 slot 内容保留。

## 阶段 3：ASCII 执行器（已完成）

- 已实现 US ASCII 到 HID usage/modifier 的转换表。
- 已使用 delayable work 逐字符发送 press/release。
- 已处理空 slot、非法字符、超长文本和重复触发。
- 第一版只允许一个 runtime macro 同时执行，执行期间重复触发返回 busy；不实现并发宏队列。

## 阶段 4：模块自有 USB HID 通道与协议（已完成）

- 已固化 32-byte `list/get/set/clear` 二进制帧格式，包含 request ID、版本号、状态码和长文本分包。
- 已实现原子 SET staging、重复分块恢复语义、固定 HID descriptor、HID_1 收发回调和单个在途 IN transfer 节流。
- USB transport 默认关闭；核心 behavior、slot、Settings/NVS 和执行器在关闭该功能时独立编译。
- transport 仅编译于 USB-enabled unibody 或 split central，保留 ZMK HID_0，不启用全局 interrupt OUT。
- host transport/protocol 测试、nice_nano split central + Settings/NVS clean build，以及关闭 transport 的 clean build 已通过。
- 使用 ZMK 官方 `studio-rpc-usb-uart` snippet 的 nice_nano split central + CDC ACM transport 共存 clean build 已通过；临时 keymap/conf/physical-layout 只放在 `/tmp`，未修改 ZMK 主仓库。

## 阶段 5：Python 验证客户端（已完成）

- 使用 PyPI `hidapi`（Python import `hid`）按 Usage Page/Usage 查找设备，严格选择 HID_1。
- 实现 `list/get/set/clear` 命令，以及 `--path`、VID/PID、超时和重试选项。
- 增加 32/33-byte HID report 兼容、协议响应校验、分页、原子 SET 重启和稳定错误显示。
- `tests/python` 使用 fake HID module/device 覆盖固定帧、过滤、分页、控制字符、安全输出、陈旧 response、超时/重试和 close。
- `python3 -m unittest discover -s tests/python -v`、`py_compile`、`git diff --check` 和现有 host GCC/Clang normal/sanitizer 测试已通过。
- actual host/hidapi round trip、USB 重插、重启/NVS 和实体按键仍属于阶段 6，阶段 5 未宣称真机通过。

## 阶段 6：实际键盘验证（待执行）

- 以支持 USB 的 ZMK split central 构建为第一目标。
- 验证 ASCII 字母、数字、标点、Enter、Tab、Backspace。
- 验证 NVS 重启恢复、USB 重插、宏运行中再次触发。
- 检查 Flash 空间、RAM 占用、USB HID descriptor 和主机 hidraw 权限问题。

## 阶段 7：后续客户端

- 保持 Python 和固件协议稳定。
- 后期使用 Tauri 重写客户端 UI/设备管理。
- 不在 Tauri 阶段重新引入 Unicode，除非另行确定输入方案。

## 暂不做

- Unicode、中文和 Emoji。
- 新 Studio protobuf RPC。
- 多宏并发和宏队列。
- 动态键盘布局识别。
- ZMK 主仓库修改。
