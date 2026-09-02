# Runtime Macro 调研记录

## 1. 范围和结论

目标是实现一个不修改 ZMK 主仓库的 external module：

```text
&runtime_macro <slot>
        |
        +-- 从 RAM 中读取启动时由 Settings/NVS 恢复的 ASCII 文本
        +-- 转换为 HID usage + modifier
        +-- 通过 ZMK keycode event pipeline 逐字符发送

USB 主机 <-> 模块自有 USB HID transport（可选） <-> runtime macro protocol <-> slot RAM <-> Settings/NVS
```

总体结论：

- custom behavior、Settings handler、NVS 持久化和 ASCII 执行器都可以由本模块独立实现。
- 宏核心不依赖任何 USB HID 或其他主机通信传输。
- 如果启用主机配置功能，USB HID CLI 的设备、收发处理和协议都由本模块自己实现。
- 运行时字符串没有现成的 ZMK ASCII-to-HID 转换器，需要模块自己实现 US ASCII 映射。
- 当前 Studio RPC 不能仅靠 external module 增加新的 protobuf subsystem，因此不接入 Studio RPC。
- 模块使用 ZMK/Zephyr 提供的 USB HID 基础 API。

本文件记录的是调研结果和拟使用的接口，不代表已经实现。

## 2. 参考范围与验证目标

本调研面向支持 USB 和 Settings/NVS 的 ZMK split central 构建。模块自有 USB HID 的 device、descriptor 和 report size 在实现阶段定义；具体 ZMK、Zephyr 及其他依赖版本由使用方的构建清单决定。

参考源码位置：

```text
zmk/app/include/zmk/behavior.h
zmk/app/include/drivers/behavior.h
zmk/app/include/zmk/behavior_queue.h
zmk/app/src/behaviors/behavior_macro.c
zmk/app/include/zmk/events/keycode_state_changed.h
zmk/app/include/dt-bindings/zmk/keys.h
zmk/app/src/main.c
zmk/app/src/settings/
zmk/app/include/zmk/studio/rpc.h
zmk/app/src/studio/rpc.c
zmk/app/CMakeLists.txt
```

## 3. Custom behavior 接口

拟提供 DTS behavior：

```dts
runtime_macro: runtime_macro {
    compatible = "zmk,behavior-runtime-macro";
    #binding-cells = <1>;
    display-name = "Runtime Macro";
};
```

用户 keymap 需要显式包含：

```dts
#include <behaviors.dtsi>
#include <behaviors/runtime_macro.dtsi>
```

绑定形式：

```dts
&runtime_macro 0
```

模块需要提供：

```text
dts/behaviors/runtime_macro.dtsi
dts/bindings/behaviors/zmk,behavior-runtime-macro.yaml
```

主要 C 接口：

```c
BEHAVIOR_DT_DEFINE(...)
struct behavior_driver_api
binding_pressed
binding_released
ZMK_BEHAVIOR_OPAQUE
```

默认 behavior locality 为 central。Split 构建中，槽位、USB 通信和宏执行应放在 central。

## 4. 执行字符的接口

推荐调用：

```c
raise_zmk_keycode_state_changed_from_encoded(encoded, pressed, timestamp);
```

声明位置：

```text
zmk/app/include/zmk/events/keycode_state_changed.h
```

这个接口会继续走 ZMK 正常的 keycode event、HID report 和 endpoint pipeline。不要直接调用底层 `zmk_hid_keyboard_press()` 作为第一版执行入口。

已有 `behavior_macro.c` 使用：

```c
zmk_behavior_queue_add(...)
```

它适合静态 devicetree macro binding，但全局队列当前为 64 项，并且会与其他 behavior macro 共享。运行时长字符串不应一次性把所有 press/release 项目塞进这个队列。

拟使用模块自己的 `k_work_delayable` 状态机：

```text
press character
  -> tap_ms
release character
  -> wait_ms
next character
```

第一版建议只允许一个 runtime macro 同时执行；第二次触发返回 `BUSY`，不做并发宏队列。

## 5. ASCII 到 HID 转换

ZMK 已有的：

```c
ZMK_HID_USAGE(page, id)
ZMK_HID_USAGE_PAGE(encoded)
ZMK_HID_USAGE_ID(encoded)
SELECT_MODS(encoded)
```

`dt-bindings/zmk/keys.h` 中的 `A`、`ENTER`、`EXCLAMATION` 等主要是编译期宏，没有可直接调用的运行时字符串转换器。

第一版支持范围拟定为 US ASCII：

```text
a-z       -> keyboard A-Z
A-Z       -> keyboard A-Z + Left Shift
0-9       -> keyboard number usages
space     -> Space
\\n       -> Enter
\\t       -> Tab
\\b       -> Backspace
常用标点  -> US keyboard usage + 必要的 Left Shift
```

设置时就验证字符，遇到不支持的字节返回错误，不在执行过程中静默丢弃。

明确不支持：

- Unicode
- 中文
- Emoji
- 任意键盘布局自适应
- 通用 Unicode 输入法

USB HID 是按键 usage，不是字符流；最终字符仍然受主机键盘布局影响。

## 6. Settings/NVS 接口

使用 Zephyr Settings API，不直接调用 NVS 底层读写 API。

拟使用的 key：

```text
runtime_macro/slot/0
runtime_macro/slot/1
...
```

handler 根名称：

```text
runtime_macro
```

主要接口：

```c
SETTINGS_STATIC_HANDLER_DEFINE(...)
settings_name_steq(...)
settings_read_cb
settings_save_one(...)
settings_delete(...)
```

拟定行为：

- 启动时由 ZMK `main.c` 调用 `settings_load()`，handler 把内容加载到固定大小的 RAM 数组。
- `set` 完整接收并通过 ASCII 校验后，先更新 RAM，再调用 `settings_save_one()`。
- `clear` 清空 RAM，再调用 `settings_delete()`。
- 每个 slot 使用独立 key，避免整个配置 blob 的更新和损坏影响其他 slot。
- RAM 中保存以 `NUL` 结尾的字符串；NVS 中保存实际字符串字节，不依赖存储 NUL。
- 如果未来需要 `settings_save()` 全量保存，再补充 `h_export`；MVP 的 `set/clear` 可直接使用 `settings_save_one/settings_delete`。

依赖和限制：

```text
CONFIG_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y
CONFIG_FLASH_MAP=y
```

目标 board 必须有有效的 `storage_partition`。NVS 空间与 ZMK 其他 Settings 共享，并且频繁 `set` 会产生 Flash 写入和磨损。

## 7. 模块自有 USB HID CLI 接口

Raw HID 在这里仅是主机配置 CLI 的一种传输方式，不是 runtime macro 核心功能的依赖。实现放在本模块内。

模块需要自行完成：

1. 使用当前 Zephyr 的 USB HID API 注册自己的 HID device 和 report descriptor；
2. 在 HID 收发回调中接收、复制和发送 report；
3. 将接收数据交给模块自己的 work queue 和协议解析器；
4. 将协议响应封装为 HID report 发回主机。

协议实现不应依赖 ZMK Studio RPC。

默认可沿用以下协议识别参数，但它们属于本模块的配置，不表示依赖外部模块：

```text
Usage Page: 0xFF60
Usage:      0x61
Report:     32 bytes（可配置）
```

设备名称和额外 HID interface 的创建方式需要按目标 board 处理；必要时由本模块提供 shield/overlay。核心功能关闭 USB HID transport 后仍应可以编译和运行。

拟定命令：

```text
LIST   列出 slot 数量、占用状态和长度
GET    获取一个 slot 的文本
SET    写入一个 slot 的完整文本
CLEAR  删除一个 slot
```

协议草案，字段名称暂定：

```text
Request:
  version, opcode, request_id, slot, offset, length, payload

Response:
  version, status, request_id, slot, offset, total_length, payload
```

`GET` 和长 `SET` 需要基于 `offset/length` 分包。Python CLI 负责拼接和校验响应；Tauri 后期复用相同二进制协议，不重新设计固件接口。

Python 第一版建议通过 `hidapi` 按 Usage Page/Usage 查找设备，不硬编码 HID interface 序号。Linux 还需要处理 hidraw 权限。

## 8. USB 方案比较

| 方案 | 结论 |
|---|---|
| 模块自有 Raw HID | 推荐作为可选 CLI transport；可与当前 Studio CDC/BLE RPC 并存，协议和 Python 验证简单 |
| CDC ACM | `pyserial` 简单，但需要额外接口，不能复用 Studio 的 `zmk,studio-rpc-uart` |
| 自定义 USB vendor class | 可行，但 descriptor、驱动和主机兼容成本更高 |
| Studio RPC transport | 不适合；新增 protobuf 类型需要 schema/build/client 变化 |

## 9. Studio RPC 边界

当前生成的 `studio.pb.h` 只有：

```text
Request subsystem: core / behaviors / keymap
Response subsystem: meta / core / behaviors / keymap
```

`zmk/app/include/zmk/studio/rpc.h` 的宏会直接引用生成符号，例如：

```c
zmk_studio_Request_##prefix##_tag
zmk_##prefix##_Request_##request_id##_tag
```

所以：

```c
ZMK_RPC_SUBSYSTEM(runtime_macro)
```

在当前 schema 下无法编译。

`zmk/app/src/studio/rpc.c` 还会固定解码 `zmk_studio_Request_msg`，外部模块定义自己的 protobuf 不会自动进入 dispatcher。

真正添加 Studio RPC 需要：

- 修改 `zmk-studio-messages` 的 protobuf schema；
- 重新生成 nanopb；
- 让 ZMK CMake 接受新增 proto 输入；
- 修改 PC/Studio 客户端。

这不符合本项目“无 ZMK/ZMK Studio fork、可删除”的目标，因此不采用。

## 10. 外部模块边界

第一版不修改以下文件：

```text
zmk/app/src/*
zmk/app/include/*
zmk/app/CMakeLists.txt
zmk-studio-messages
ZMK Studio 客户端
```

用户项目只需后续增加：

```text
prj.conf：CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO=y 及槽位参数
prj.conf（需要主机 CLI 时）：CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y
.keymap：#include <behaviors/runtime_macro.dtsi> 和 &runtime_macro <slot>
构建参数：将本模块加入 ZMK_EXTRA_MODULES
```

若目标 board 需要额外的 USB HID interface，本模块负责提供对应的 shield/overlay 或配置说明。

## 11. 可弃用设计

- behavior 开关使用 `ZMK_BEHAVIOR_*` 命名空间，其余 runtime macro 配置使用 `ZMK_RUNTIME_MACRO_*` 命名空间。
- Settings 使用 `runtime_macro/` 命名空间。
- CLI 协议包含 `version` 字段。
- 不添加 ZMK 主仓库 patch，不 fork Studio RPC schema。
- 官方动态宏可用后，删除 external module、配置项和 keymap include；旧的 `runtime_macro/` settings 不会被其他模块解释。
