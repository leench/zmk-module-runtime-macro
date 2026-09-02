# Runtime Macro 初步计划

当前只保留高层步骤；具体实现时再拆分任务和验收项。

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

## 阶段 3：ASCII 执行器

- 实现 US ASCII 到 HID usage/modifier 的转换表。
- 使用 delayable work 逐字符发送 press/release。
- 处理空 slot、非法字符、超长文本和重复触发。
- 第一版只允许一个宏同时运行。

## 阶段 4：模块自有 USB HID 通道与协议

- 在本模块内实现 USB HID report descriptor、收发回调和发送节流。
- 将 USB HID 通道作为可选功能；核心 behavior、slot、Settings/NVS 和执行器在关闭该功能时仍可独立编译。
- 对需要额外 HID interface 的 board，由本模块提供所需的 shield/overlay 或明确的最小配置。
- 固化 `list/get/set/clear` 的二进制帧格式。
- 增加 request ID、版本号、状态码和长文本分包。
- 验证与当前 Studio CDC/BLE RPC 并存。

## 阶段 5：Python 验证客户端

- 使用 `hidapi` 按 Usage Page/Usage 查找设备。
- 实现 `list/get/set/clear` 命令。
- 增加协议超时、响应校验和错误显示。
- 用脚本完成手动测试流程。

## 阶段 6：实际键盘验证

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
