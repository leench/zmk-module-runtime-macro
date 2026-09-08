# RAM-only Dynamic Macro 详细分段实施计划

## 1. 文档目的

本文把 [`PLAN.md`](PLAN.md) 中已经确认的 RAM-only dynamic macro 产品方向拆分为可独立评审、实现、验证和提交的阶段。

本文是**实施计划**，不是当前功能说明，也不是最终 wire protocol 契约。Phase 0 已冻结，当前实现进度与阶段门禁记录在本文各阶段完成记录中。

文档关系：

- [`PLAN.md`](PLAN.md)：产品范围、已确认行为和明确非目标；
- [`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md)：现有 v2 认证与 wire contract 的权威文档；
- 本文：动态宏的实施顺序、文件边界、测试矩阵和阶段门禁；
- 计划新增的 `DYNAMIC_PROTOCOL.md`：动态命令最终字节定义与客户端兼容规则。

若文档之间发生冲突，先停止实现并更新设计文档，不在代码中静默选择一种解释。

## 2. 已确认范围

第一版必须满足以下约束：

1. 只有一个动态宏对象，最多 256 bytes；
2. 允许的字节与现有静态宏一致：可打印 US ASCII、LF、Tab、Backspace；
3. 动态文本和上传 staging 只存在于 RAM，不调用 Settings/NVS；
4. 只能通过物理 `&runtime_macro_dynamic` behavior 执行；
5. 不提供动态 `GET`、`LIST` 或协议执行命令；
6. 静态宏和动态宏共用现有单执行器，不排队；
7. 执行器接受动态文本后立即消费 committed buffer；若执行器忙或启动失败则保留；
8. 完整新上传原子替换旧 committed text；失败或不完整上传不得破坏旧值；
9. TTL 从完整 commit 时开始，未指定时默认 5 分钟；
10. reboot/reset、TTL 到期、显式 clear 和默认启用的真实管理 USB disconnect 清空动态状态；
11. Bluetooth profile 或 USB/Bluetooth output 切换默认保留；
12. 动态命令不进入静态宏认证 gate，也不刷新认证 session；
13. 不改变普通键盘 HID 输出路径，不新增第二执行器，不修改 ZMK 主仓库。

## 3. 实施原则

### 3.1 分段门禁

每个阶段都按以下顺序执行：

1. 用户确认开始该阶段；
2. 只实现该阶段列出的范围；
3. 完成代码审查和本阶段测试；
4. 在 `zmk-dev` devcontainer 内完成要求的 ZMK build；
5. 记录 RAM/Flash 或协议变化；
6. 独立 commit 并 push；
7. 用户确认后才能进入下一阶段。

任何阶段出现未解决的正确性、内存、协议兼容或构建问题时，不得把阶段标记为完成。

### 3.2 最小改动

- 复用现有 ASCII 映射、32-byte frame、USB HID transport 和执行器；
- 动态 store 与 Flash-backed static slot store 分离；
- 不为未来多动态槽、执行队列或其他 transport 提前抽象；
- 不修改现有静态 `LIST/GET/SET/CLEAR` 和认证行为；
- feature 未启用时不应分配动态宏大缓冲区，也不应改变现有固件行为。

### 3.3 敏感数据处理

虽然第一版不宣称是安全秘密通道，仍应避免无意义地延长 RAM 中文本寿命：

- clear、替换、TTL 到期和上传取消时 zeroize 对应缓冲区；
- executor 在完成、失败或取消后 zeroize 自己的 snapshot；
- 日志只记录长度、状态和错误码，不记录动态文本、chunk 内容或认证材料；
- 测试失败输出不得打印真实测试秘密，统一使用固定假数据。

## 4. 总体阶段

| 阶段 | 名称 | 主要产物 | 是否修改运行代码 |
|---|---|---|---|
| 0 | 设计冻结 | `DYNAMIC_PROTOCOL.md`、Kconfig/DTS 决策、RAM 预算 | 否 |
| 1 | 编译门控与 RAM 基线 | feature gate、构建矩阵、map 对比 | 是 |
| 2 | RAM store 与 TTL | committed/staging state、原子 commit、clear | 是 |
| 3 | 共用执行器改造 | 通用 snapshot 启动入口、动态消费语义 | 是 |
| 4 | 动态 keymap behavior | `&runtime_macro_dynamic` | 是 |
| 5 | 动态协议 | capability、BEGIN/DATA/CLEAR | 是 |
| 6 | 生命周期策略 | USB disconnect、profile/output policy | 是 |
| 7 | Python client 与 CLI | upload/clear API 和命令 | 是 |
| 8 | 集成、硬件验证与发布文档 | 完整回归、真实工作流、用户文档 | 是/文档 |

阶段顺序原则上不得交换。第 4 阶段可以在第 5 阶段前用测试接口填充动态文本，但不得因此添加生产调试 API。

---

## 5. 阶段 0：设计和 wire contract 冻结

### 5.1 目标

在编写固件代码前消除所有会影响兼容性、RAM 布局和生命周期语义的未决项。

### 5.2 必须冻结的决策

#### A. Feature gate

评审并确认：

- 新增独立 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC`；
- 默认关闭，避免现有用户无条件增加约 256-byte 级别缓冲区；
- 第一版是否强依赖 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`；
- keymap 引用了 `&runtime_macro_dynamic` 但 feature/transport 未启用时，是明确 build failure，还是自动启用；
- 仅引用动态 behavior、未引用静态 `&runtime_macro <slot>` 时，devicetree/Kconfig/CMake 是否仍能正确构建。

推荐方案：动态 feature 独立门控、默认关闭；第一版依赖 USB HID transport；引用 behavior 时自动选中或给出确定的构建错误，不能生成缺失 driver 的固件。

#### B. DTS/behavior 结构

现有静态 binding 是 one-parameter behavior，不能直接表示 zero-parameter 动态 behavior。必须确认：

- 使用独立 compatible 和 `zero_param.yaml` binding；
- 在现有 `runtime_macro.dtsi` 中暴露 `runtime_macro_dynamic` label；
- 采用独立 behavior driver，或在不混淆 binding cell 数量的前提下复用 driver 逻辑；
- 保持 `BEHAVIOR_LOCALITY_CENTRAL`。

推荐方案：独立 DTS compatible、独立小型 behavior driver，共用动态 store/executor API。

#### C. Opcode 和 capability discovery

最终文档必须固定：

- `DYNAMIC_BEGIN`、`DYNAMIC_DATA`、`DYNAMIC_CLEAR` 的 opcode；
- 客户端如何在不写入、不 clear 的情况下探测动态支持；
- capability 是否返回最大长度、默认 TTL、TTL 上下限和 lifecycle flags；
- 旧 v2 客户端连接新固件时仍能正常使用静态与认证命令。

推荐保留 `0x20..0x22` 给三个动态命令，并增加一个只读、无副作用的通用 capability 查询。若不增加 capability opcode，必须提供同样无副作用且可区分旧固件的探测方式；不能用 `DYNAMIC_CLEAR` 作为探测。

#### D. `DYNAMIC_BEGIN` 字节定义

必须固定：

- dynamic object 使用哪个 `slot` sentinel；
- `total_length` 的合法范围；
- omitted TTL 的编码；
- 显式 TTL 的字节序、单位和合法范围；
- `offset`、`payload_length` 和未使用尾部字节的要求；
- zero-length upload 是拒绝还是等价于 clear。

推荐语义：

- `slot = 0xff`；
- `total_length = 1..256`，空文本使用 `DYNAMIC_CLEAR`；
- 无 payload 表示默认 300 秒，4-byte little-endian payload 表示显式 TTL 秒数；
- `offset = 0`；
- TTL 最大值在本阶段评审后写死在 contract，不留“实现自行决定”。

#### E. `DYNAMIC_DATA` 事务规则

必须固定：

- DATA 与 BEGIN 使用同一 request ID 和 total length；
- offset 必须从 0 开始连续增长，每 chunk 为 1..22 bytes；
- 新 BEGIN 如何替换旧 incomplete transaction；
- duplicate、out-of-order、旧 request ID、total mismatch 的状态码；
- invalid frame 是否取消当前 dynamic staging；
- final ACK 丢失后的客户端恢复方式；
- 响应只返回状态、next offset 和 total，不返回文本。

推荐语义：任何事务一致性错误都 zeroize 并取消 incomplete staging，但保留旧 committed text；客户端在超时或可恢复事务错误后以新 request ID 从 BEGIN 完整重传。

#### F. 认证边界

必须把以下行为明确写入 wire contract：

- capability 和三个动态命令不要求登录；
- 即使设备处于 `PROTECTED` 或 credential `ERROR_LOCKED`，动态命令仍按其自身格式处理；
- 动态命令成功或失败都不刷新静态管理 session；
- 动态命令不调用 static slot set/clear；
- `LOCK`、登录、密码更换和认证 session 过期是否只取消 incomplete dynamic upload，且绝不清除已 committed text；
- USB transport reset 可取消 incomplete upload，但 committed text 只按动态生命周期策略清除。

这是一项有意的安全边界，必须由用户在阶段 0 明确批准，不能仅作为代码实现细节。

#### G. 生命周期精确定义

必须区分：

1. protocol staging discard；
2. authentication transport reset；
3. dynamic committed-text clear。

“真实管理 USB disconnect”推荐定义为：USB raw status 在生命周期处理前后稳定，且明确为 `USB_DC_DISCONNECTED`。`RESET`、`SUSPEND`、`RESUME`、`CONFIGURED`、`UNKNOWN`、`ERROR` 和普通 HID notification 不自动等价为 dynamic disconnect clear。

ZMK 当前可用事件边界：

- `zmk_ble_active_profile_changed`：能观察 active BLE profile 变化；
- `zmk_endpoint_changed`：能观察当前 selected endpoint 变化；
- ZMK 没有独立事件保证能观察每次 `OUT_USB/OUT_BLE` preferred transport 写入，尤其是 preferred 值变化但 selected endpoint 未变化时。

因此阶段 0 必须确认可配置项描述为“clear on selected endpoint change”，还是要求精确的“clear on preferred output change”。后者在不修改 ZMK 主仓库的约束下可能无法完整实现，不能在文档中承诺超过现有事件 API 的能力。

### 5.3 RAM 预算

已知目标固件曾测得 RAM 为 `260478 / 262144` bytes，余量 1666 bytes；此值只作为历史基线，阶段 1 必须重新构建确认。

默认静态文本上限为 64，而动态上限固定为 256。朴素实现至少增加：

- committed buffer：约 256 bytes；
- upload staging：约 256 bytes；
- executor 从 64 扩到 256 的差额：约 192 bytes；
- 另有 mutex、work、长度、deadline、状态和对齐开销。

也就是在不计算控制结构前，增量已约 704 bytes。设计必须：

- 禁止再复制出长期存在的第四个 256-byte buffer；
- 避免在系统 workqueue 或小栈路径上放置不必要的 256-byte local array；
- 用 map 文件分别报告 `.bss`、`.data`、stack 相关变化；
- 若剩余 RAM 不足，由用户决定缩小其他模块内存或停止动态宏实现，不能静默牺牲稳定性。

### 5.4 产物

- 新建 `docs/DYNAMIC_PROTOCOL.md`；
- 在该文档中列出精确 frame 表、opcode、status、validation order、事务状态机和示例帧；
- 确认 Kconfig、DTS 和 lifecycle 命名；
- 记录 RAM 预算与拒绝条件；
- 更新本文中仍标记为“推荐”的项目为最终决定。

### 5.5 完成标准

- wire 字节无“待定”“可自行选择”项；
- 固件与 Python client 能根据同一份文档独立实现；
- 认证边界获得明确批准；
- capability 探测无副作用；
- 用户批准进入阶段 1。

---

## 6. 阶段 1：编译门控、文件骨架和 RAM 基线

### 6.1 目标

建立 feature on/off 的可靠构建边界，先量化内存成本，不实现协议或 behavior 功能。

### 6.2 预计修改

- `Kconfig`
- `CMakeLists.txt`
- 动态模块私有 header/source 骨架
- 必要的 host test build 列表

### 6.3 工作项

1. 增加阶段 0 确认的 dynamic Kconfig；
2. feature off 时不编译动态 source、不分配动态 buffer；
3. feature on 时只加入最小状态骨架和编译期常量；
4. 对 256-byte 上限添加 compile-time assertion；
5. 生成 feature off/on 的 map 对比；
6. 检查 unibody、split central、split peripheral 和 USB transport-off 配置行为；
7. 不在本阶段增加 opcode、DTS behavior 或客户端命令。

### 6.4 验证

- 现有 host tests 全部通过；
- feature off 的 RAM/Flash 与基线无非预期增长；
- feature on 构建成功并记录增量；
- split peripheral 不包含 central-only 状态；
- 在 devcontainer 中构建目标 central 固件并保留 map 摘要。

### 6.5 停止条件

- 目标固件链接失败；
- 剩余 RAM 不能覆盖运行期安全余量；
- 仅引用动态 behavior 的 Kconfig/DTS 关系无法形成确定行为。

### 6.6 Phase 1 实际完成记录

Phase 1 已完成。实现和验证结果如下；本阶段没有进入 RAM store、TTL、协议、DTS behavior、executor 或 client 实现。

#### Gate 与骨架

- 新增 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC`，默认 `n`，并依赖
  `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`；dynamic 不反向 `select` 或启用 USB。
- USB transport 关闭时，显式请求 dynamic `y` 会收到 Kconfig unmet-dependency
  warning，最终解析为 `n`；不编译 dynamic source，也不分配 dynamic buffer。
- CMake 仅在 central-side runtime macro library 中加入
  `src/runtime_macro_dynamic.c`；feature off 时没有 dynamic object。
- feature on 只保留两个 256-byte RAM layout skeleton buffer 和 compile-time
  assertions。当前没有状态转换、锁、TTL 或数据路径。
- 由于本阶段不增加 dynamic DTS behavior，骨架仍位于现有 central runtime
  macro library 的 gate 内；dynamic-only keymap 的 behavior 选择与 DTS/Kconfig
  衔接留到后续 behavior 阶段处理，不在本阶段静默扩展范围。

#### 验证命令

- 在 `zmk-dev` devcontainer 内运行 `CLANG=gcc ./tests/host/run.sh`：GCC、
  sanitizer 及替代 Clang 阶段全部通过。
- split central、Totem dongle off/on、split peripheral USB-off 和
  `native_sim` unibody 构建均通过；各 build 使用独立目录。
- `git diff --check`：通过。

#### 当前构建与 RAM/Flash 记录

| 构建场景 | Dynamic 最终值 | Flash | RAM | 剩余 RAM | Dynamic map |
|---|---:|---:|---:|---:|---|
| `leen_temper_dongle` split central off | `n` | 344316 B | 92602 B | 169542 B | absent |
| `leen_temper_dongle` split central on | `y` | 344332 B | 93114 B | 169030 B | `.bss.runtime_macro_dynamic_state = 0x200` |
| `leen_totem_dongle` off | `n` | 429568 B | 193278 B | 68866 B | absent |
| `leen_totem_dongle` on | `y` | 429584 B | 193790 B | 68354 B | `.bss.runtime_macro_dynamic_state = 0x200` |
| split peripheral，USB off | `n` | 190692 B | 36516 B | 225628 B | absent |
| `native_sim` unibody，USB off | `n` | — | — | — | absent |

当前 Totem dongle 实测增量为 Flash `+16 B`、RAM `+512 B`，与两个 256-byte
骨架 buffer 一致。历史 `260478/262144` RAM 数值仅作为旧配置参考，不作为当前
门禁；当前容器和 display 配置的实测值是 `193278/262144`（off）和
`193790/262144`（on）。两次 Totem build 均链接成功，未触发 Phase 1 停止条件。

#### 阶段边界与后续衔接

本阶段提交的文件为：`Kconfig`、`CMakeLists.txt`、
`src/runtime_macro_dynamic.c`、`src/runtime_macro_dynamic_internal.h`、
`tests/host/run.sh`、`tests/host/runtime_macro_dynamic_gate_test.c` 和
`tests/host/README.md`。下一阶段只可在用户确认后实现 RAM store、staging、
原子 commit、clear 和 TTL；不得把本阶段的 layout skeleton 当作可用动态宏数据路径。

---

## 7. 阶段 2：RAM store、上传 staging 与 TTL

### 7.1 目标

实现完全独立于 Settings/NVS 的动态状态机，不连接 USB 协议和物理 behavior。

### 7.2 建议内部状态

- committed byte buffer，容量 256；
- committed length 和 valid flag；
- staging byte buffer，容量 256；
- staging active、expected length、received length；
- TTL deadline 或 generation；
- delayable TTL work；
- 一个 mutex，串行化 commit、clear、expire、execute handoff 和上传操作。

request ID 属于 wire transaction，放在 protocol context 或 store 中必须在阶段 0 固定；不得在两个位置各保留一份可分叉的事务真值。

### 7.3 内部 API 原则

API 只供本模块的 protocol、behavior、executor 和 lifecycle 使用，放在模块私有 header。禁止增加公开动态 read/get API。

所需能力：

- 初始化/重置 volatile state；
- begin staging；
- append contiguous chunk；
- final atomic commit；
- cancel staging without touching committed text；
- clear staging + committed + TTL；
- 在锁内检查 TTL；
- 把 committed text 交给执行器并仅在执行器接受时消费。

不得调用：

- `zmk_runtime_macro_slot_set()`；
- `zmk_runtime_macro_slot_clear()`；
- `settings_save_one()`；
- `settings_delete()`。

### 7.4 关键并发语义

- 新 BEGIN 清除旧 staging，但保留旧 committed text；
- final DATA 在同一临界区完成 staging → committed 替换、旧值 zeroize 和 TTL 启动；
- clear 与 final commit 的先后顺序由 mutex 决定，最后完成者生效；
- TTL work 必须使用 generation/deadline 复核，旧 work 不得清除较新的 commit；
- execute、clear、TTL 和 commit 不得产生 torn snapshot；
- incomplete transaction 被取消时只 zeroize staging；
- reboot 后依赖 RAM 初始化为空，同时执行显式 init invariant 检查。

### 7.5 Host tests

新增独立 dynamic store test，至少覆盖：

- 1、22、23、255、256 bytes；
- 0 和 257 bytes；
- 所有合法字符与非法控制字节/高位字节；
- 多 chunk commit 前 committed 值不变；
- final commit 原子替换；
- 新 BEGIN 替换旧 staging；
- bad offset、bad length、invalid text 取消 staging但保留 committed；
- clear 幂等；
- TTL 默认值、显式值、边界值、过期和 recommit generation race；
- clear/expire 后 buffer 确实归零；
- Settings save/delete 调用次数始终为 0；
- static slot 与 credential stub 状态不受影响。

### 7.6 完成标准

- store 测试通过 sanitizer；
- 无 Settings/NVS symbol 调用路径；
- 并发和 TTL 状态转换有注释且与 contract 一致；
- feature on/off build 和 RAM 记录更新。

### 7.7 Phase 2 实际完成记录

Phase 2 已完成。动态 store 仍完全独立于 Settings/NVS、USB protocol、DTS
behavior 和 executor；staging inactivity timeout 留给后续 protocol transaction
层，不在本阶段伪造第二套生命周期状态。

#### 私有 API 与状态语义

`src/runtime_macro_dynamic_internal.h` 新增了后续 protocol 可直接使用的私有
API：

- `zmk_runtime_macro_dynamic_reset()`：清空所有 volatile state 并取消 TTL work；
- `zmk_runtime_macro_dynamic_begin()`：校验 `1..256` 长度和 `1..86400` TTL，合法
  BEGIN 替换旧 staging 但保留 committed；
- `zmk_runtime_macro_dynamic_append()`：只接受 contiguous offset 和合法 ASCII/control
  字节，最终 chunk 在同一 mutex 临界区内完成 committed 原子替换并启动 TTL；
- `zmk_runtime_macro_dynamic_cancel_staging()`：只清 staging；
- `zmk_runtime_macro_dynamic_clear()`：幂等清 committed、staging 和 TTL；
- `zmk_runtime_macro_dynamic_check_expiry()`：供后续 lifecycle/consumer 在观察状态
  前执行 deadline 检查。

状态包含两个 256-byte buffer、长度/active 标志、staging TTL、TTL deadline、
current/work generation、一个 mutex 和一个 delayable work。错误的 BEGIN/DATA
输入会 zeroize/cancel staging，同时保留尚未过期的旧 committed。TTL 到期会清除
committed、staging、transaction metadata 和 TTL；recommit 使用 deadline 与
`generation` 保护，旧 work 不会清除较新的 commit。数据和日志不包含文本内容。

本阶段没有增加动态 read/get API，也没有实现 executor handoff/consume/snapshot；
下一阶段必须在同一 executor 中补充这些语义，并继续保持 TTL deadline/generation
保护。staging inactivity timeout 仍由后续 protocol transaction 层负责，不能在
两个模块中产生相互独立的 transaction 真值。

#### 测试与构建记录

- 新增独立 `runtime_macro_dynamic_store_test.c`，覆盖长度边界、完整合法字符集、
  非法字节、chunk/offset 错误、旧 committed 保留、BEGIN 替换 staging、原子 final
  commit、clear/zeroize、默认/显式 TTL、generation/deadline race，以及 clear 与
  final append 的 pthread 锁竞态。
- 在 `zmk-dev` devcontainer 内运行 `CLANG=gcc ./tests/host/run.sh`：GCC、sanitizer、
  替代 Clang 阶段和既有 static/protocol/auth/executor tests 全部通过。
- dynamic object 未解析 `settings_*` 或 static slot API；feature-off map 无 dynamic
  state；`git diff --check` 通过。

当前 Totem dongle（`leen_display raw_hid_adapter leen_totem_dongle`）实测：

| 构建 | Flash | RAM | Dynamic state map |
|---|---:|---:|---|
| Phase 2 off | 429568 B | 193278 B / 262144 B（73.73%） | absent |
| Phase 2 on | 430116 B | 193894 B / 262144 B（73.96%） | `.bss.runtime_macro_dynamic_state = 0x228` |

相对 Phase 2 off，当前 dynamic on 总增量为 Flash `+548 B`、RAM `+616 B`，剩余
RAM 为 `68250 B`。其中 Phase 1 已有两个 256-byte buffer；Phase 2 主要增加状态、
mutex/work 和 store 代码。两次 build 均成功链接，仅有既有 `leen-display`
`ZMK_TRANSPORT_NONE` warning，未触发阶段停止条件。

#### 阶段边界与后续衔接

本阶段修改文件为：`src/runtime_macro_dynamic.c`、
`src/runtime_macro_dynamic_internal.h`、`tests/host/stubs/zephyr/kernel.h`、
`tests/host/run.sh` 和 `tests/host/runtime_macro_dynamic_store_test.c`。下一阶段
只可在用户确认后改造现有 executor，加入共享 busy、256-byte snapshot、consume-on-
accept 和失败保留语义；不得新增 dynamic 专用 worker 或第二个长期文本 buffer。

---

## 8. 阶段 3：共用执行器与消费语义

### 8.1 目标

让静态和动态文本通过同一个 delayable-work executor 执行，同时保留现有静态 API 行为。

### 8.2 工作项

1. 从 `zmk_runtime_macro_execute(slot)` 中提取模块内部的“从已验证 bytes 启动执行”入口；
2. 该入口统一执行 busy CAS、snapshot copy、work schedule 和错误恢复；
3. executor 容量能容纳 256-byte dynamic text；
4. 静态 `zmk_runtime_macro_execute(slot)` 继续先获得 slot snapshot，再调用共用入口；
5. 动态执行在 store mutex 保护下完成 handoff：
   - empty/expired：不输出；
   - busy：返回 `-EBUSY`，保留 committed text；
   - schedule/start error：返回错误，保留 committed text；
   - accepted：executor 拥有独立 snapshot，store 立即消费并 zeroize committed text；
6. executor 完成或任何错误退出时 zeroize snapshot；
7. 不新增执行队列、动态专用 worker 或 raw HID report 路径。

### 8.3 必须验证的竞态

- static running → dynamic press：busy，dynamic 保留；
- dynamic running → static press：busy，static 不受影响；
- dynamic accepted 后立刻新 upload：旧 snapshot 继续执行，新 committed text 保留待下次按键；
- final upload 与 key press 并发：只允许执行完整旧值或完整新值；
- TTL 到期与 key press 并发：由锁顺序决定，不输出部分或已清除数据；
- executor schedule 失败不能消费 dynamic text；
- event raise 失败仍按现有逻辑释放按键并安全结束。

### 8.4 Tests

扩展 executor test，覆盖：

- 256-byte dynamic snapshot；
- static/dynamic 全局 busy；
- consume-on-accept；
- preserve-on-busy 和 preserve-on-start-error；
- 执行期间 replacement 不改变 executor snapshot；
- executor 完成/错误后 snapshot zeroize；
- 现有 ASCII、control、timing 和 static snapshot 测试不变。

### 8.5 完成标准

- 所有静态 executor tests 无回归；
- 动态执行只有一个 worker；
- 行为返回值与消费结果一一对应；
- map 中不存在意外的额外 256-byte 长期缓冲区。

### 8.6 Phase 3 实际完成记录

Phase 3 已完成。静态和 dynamic 文本现在共用同一个 executor busy 状态、同一个
`k_work_delayable` 和同一个 executor-owned snapshot；本阶段未修改 behavior、
protocol、USB 或 lifecycle。

#### 共享入口与消费语义

- 新增私有 `zmk_runtime_macro_executor_start()`，统一处理输入容量检查、全局
  busy CAS、snapshot copy、初始 work schedule 和失败恢复。
- 静态 `zmk_runtime_macro_execute(slot)` 保持先做现有 slot snapshot，再调用共享
  入口；static slot API 和现有返回行为不变。
- 新增私有 `zmk_runtime_macro_dynamic_execute()`：先持 dynamic store mutex 检查
  TTL/empty，再把 committed bytes 单向交给共享 executor。只有 start 返回 accepted
  后才清 committed 和 TTL；`-EBUSY` 或 schedule/start error 保留 committed。
- executor snapshot 在 feature on 时扩展到 256 bytes + terminator；feature off
  仍使用 static `CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN` 容量。
- executor 完成、invalid byte、event/schedule error 和 start failure 路径均先
  zeroize 全部 snapshot，再释放 busy。executor 不回调 dynamic store，避免锁反转。
- accepted 后 dynamic store 可立即被新 upload/clear；executor 使用独立 snapshot，
  不受后续 store 生命周期影响。

#### 测试与构建记录

- executor host test 新增 256-byte snapshot、static/dynamic 双向 busy、consume-on-
  accept、busy/start-error 保留、accepted 后 replacement、TTL/empty、event error
  和 snapshot zeroize 覆盖；既有 static ASCII/control/timing/schedule tests 全部
  保持通过。
- 在 `zmk-dev` devcontainer 内运行 `CLANG=gcc ./tests/host/run.sh`：GCC、sanitizer、
  替代 Clang 及既有 store/protocol/auth/USB tests 全部通过；`git diff --check`
  通过。
- dynamic/executor object 没有新增 Settings/NVS/static-slot 未解析依赖。

当前 Totem dongle 实测：

| 构建 | Flash | RAM | map 关键状态 |
|---|---:|---:|---|
| Phase 3 off | 429632 B | 193278 B / 262144 B（73.73%） | dynamic state absent |
| Phase 3 on | 430212 B | 194086 B / 262144 B（74.04%） | dynamic state `0x228`；executor state `0x114` |

相对 Phase 3 off，dynamic on 总增量为 Flash `+580 B`、RAM `+808 B`，剩余 RAM
为 `68058 B`。相对 Phase 2 on，executor 改造新增约 `+96 B Flash`、`+192 B RAM`，
正好对应 executor metadata/容量扩展和共享入口代码。两次 build 均成功链接，只有
既有 `leen-display` `ZMK_TRANSPORT_NONE` warning，未触发阶段停止条件。

#### 阶段边界与后续衔接

本阶段修改文件为：`src/runtime_macro_executor.c`、
`src/runtime_macro_executor_internal.h`、`src/runtime_macro_dynamic.c`、
`src/runtime_macro_dynamic_internal.h`、`tests/host/runtime_macro_executor_test.c`
和 `tests/host/runtime_macro_dynamic_store_test.c`。下一阶段只可在用户确认后新增
`&runtime_macro_dynamic` zero-parameter、central-only behavior；不得在 behavior 中
硬编码 USB/BLE 输出，也不得新增 executor 或 protocol execute opcode。

---

## 9. 阶段 4：`&runtime_macro_dynamic` keymap behavior

### 9.1 目标

提供 zero-parameter、central-only 的物理触发入口。

### 9.2 预计修改

- `dts/behaviors/runtime_macro.dtsi`
- 新增 zero-parameter binding YAML
- 新增或调整 behavior driver source
- `CMakeLists.txt`
- behavior host test/stubs

### 9.3 Behavior 语义

- press 调用动态 execute/handoff API；
- empty/expired 不产生 key event，并按最终 contract 返回 harmless result；
- busy 返回 `-EBUSY` 且不消费；
- accepted 返回 `ZMK_BEHAVIOR_OPAQUE`；
- release 不执行第二次宏，只返回 opaque；
- 不接受 slot 参数；
- locality 保持 central，split peripheral 只上报 position；
- 日志不得包含文本。

### 9.4 验证

- devicetree 能解析 `&runtime_macro_dynamic`；
- 错误地携带参数时构建失败；
- 仅引用 static、仅引用 dynamic、两者同时引用的 keymap 组合均按阶段 0 决策工作；
- split central 执行，peripheral 不分配 store/executor；
- empty、busy、accepted、schedule error 的返回和消费状态正确；
- selected endpoint 决定最终 USB/BLE 输出，不在 behavior 中硬编码 transport。

### 9.5 完成标准

- 物理 behavior 可通过测试注入的 dynamic committed text 驱动现有 keycode event pipeline；
- 无协议执行 opcode；
- 静态 `&runtime_macro <slot>` metadata 和参数范围无变化。

### 9.6 Phase 4 实际完成记录

Phase 4 已完成。新增了 zero-parameter、central-only 的
`&runtime_macro_dynamic` behavior；press 只调用现有
`zmk_runtime_macro_dynamic_execute()`，成功（包括 empty/expired 的 harmless
结果）返回 `ZMK_BEHAVIOR_OPAQUE`，busy 或其他启动错误原样返回；release 不会再次
执行，只返回 opaque。driver 不读取、记录或输出动态文本，也不硬编码 USB/BLE
endpoint。

本阶段的门控决策为显式 opt-in：`CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC` 继续
`default n`，仅在应用显式设为 `y` 且 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y` 时
提供 dynamic store/executor/behavior。它不反向启用 USB。新增的
`CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO_DYNAMIC` 仅在 dynamic DT node 被引用、dynamic
feature 已启用且构建位于 central/unibody 时启用。common runtime macro gate
现在接受 static 或 dynamic compatible；未启用 dynamic feature 却引用 dynamic
behavior 时，`runtime_macro_dynamic_guard.c` 在 central/unibody 和 split
peripheral 都确定性地以编译错误停止，不能生成缺失 driver 的固件。

Totem 的 `studio-rpc` 会设置 `ZMK_BEHAVIORS_KEEP_ALL`。为避免 static-only
keymap 因 keep-all 无条件保留 dynamic node，`runtime_macro.dtsi` 在未显式指定
`ZMK_BEHAVIORS_KEEP_RMD` 或 `ZMK_BEHAVIORS_OMIT_RMD` 时为 dynamic node 加入
`/omit-if-no-ref/`；实际引用 `&runtime_macro_dynamic` 仍会保留该 node。static
node 的 label、compatible、binding cell 数量和原有 omit 逻辑未改变。

实现文件为：`Kconfig`、`CMakeLists.txt`、`dts/behaviors/runtime_macro.dtsi`、
`dts/bindings/behaviors/zmk,behavior-runtime-macro-dynamic.yaml`、
`src/behaviors/behavior_runtime_macro_dynamic.c`、
`src/runtime_macro_dynamic_guard.c`，以及对应的 host behavior test/stubs。
本阶段未修改 protocol、USB lifecycle、client、static behavior metadata 或
ZMK 主仓库。

验证结果：

| 场景 | 结果 |
|---|---|
| Static-only Totem | 通过；dynamic 未设置；Flash `429632 B`，RAM `193278 B`（73.73%）；无 dynamic state/object |
| Dynamic feature on、无 dynamic 引用 | 通过；Flash `430212 B`，RAM `194086 B`（74.04%）；dynamic state `0x228` |
| True dynamic-only、显式 dynamic `y` | 通过；无 static `&runtime_macro` 引用；Flash `430432 B`，RAM `194094 B`（74.04%）；dynamic behavior/state 存在 |
| Static + dynamic、显式 dynamic `y` | 通过；dynamic behavior/state 存在 |
| True dynamic-only、未显式启用 dynamic | 按设计失败；guard 报错 |
| `&runtime_macro_dynamic 1` | 按设计在 devicetree 阶段失败 |
| USB-off central / split peripheral dynamic 引用 | Kconfig warning 后由 guard 确定失败；不编译 dynamic store/executor/behavior |
| Split peripheral static-only | 通过；无 dynamic store/executor/behavior object |
| Host suite | 容器内 `CLANG=gcc ./tests/host/run.sh` 全部通过，包括 dynamic behavior |

所有 off/on 和 behavior build 均成功链接（预期失败场景除外），仅有既有
`leen-display` 的 `ZMK_TRANSPORT_NONE` switch warning；`git diff --check`
通过。下一阶段只可在用户确认后进入 dynamic protocol 与 capability，不得把
behavior 直接扩展为协议执行入口。

---

## 10. 阶段 5：动态 protocol 与 capability

### 10.1 目标

在现有 v2 32-byte transport-independent protocol 中实现阶段 0 冻结的动态命令。

### 10.2 预计修改

- `include/zmk/runtime_macro_protocol.h`
- `src/runtime_macro_protocol.c`
- 模块私有 dynamic API
- `tests/host/runtime_macro_protocol_test.c` 或独立 dynamic protocol test
- `docs/DYNAMIC_PROTOCOL.md`（只做与最终代码一致的勘误）

### 10.3 Dispatch 边界

处理顺序必须明确且有测试固定：

1. 通用 frame/version/status/payload-tail 校验；
2. opcode 分类；
3. capability/dynamic 分支直接进入各自校验；
4. static management 命令才进入现有 auth gate；
5. password/auth 命令保持原有路径。

动态分支不得：

- 调用 static management access；
- refresh auth session；
- 读取或返回 committed/staging text；
- 调用 static slot set/clear；
- 把 dynamic object 加进 LIST。

### 10.4 Transaction 管理

- protocol context 只保存 wire 所需的 transaction identity；
- store 只保存实际 staging bytes 和单一真实状态；
- `protocol_init/discard` 对 incomplete dynamic transaction 的影响必须与 contract 一致；
- malformed dynamic frame 只取消 dynamic staging，不误清 static SET/PASSWORD_SET，除非 contract 明确要求整个 context discard；
- malformed static/auth frame 不得清除 dynamic committed text；
- USB generation reset 后旧 request 不得在新连接 commit。

### 10.5 Protocol tests

至少覆盖：

- wire constants 和 capability payload；
- BEGIN default TTL 与显式 TTL；
- 1/22/23/256-byte DATA；
- commit 前不可执行新 staging；
- request ID、total length、offset 连续性；
- duplicate、out-of-order、zero chunk、oversized、invalid byte；
- 新 BEGIN restart；
- final ACK 丢失后完整重传；
- CLEAR 同时清 staging/committed/TTL，且幂等；
- 所有响应 payload 不包含动态文本；
- static LIST/GET 永远看不到 dynamic；
- static SET/CLEAR 与 dynamic 状态互不影响；
- OPEN、PROTECTED 未登录、PROTECTED 已登录、ERROR_LOCKED 下的动态行为；
- 动态成功/失败不改变 auth session deadline；
- transport discard、generation rollover 和 stale queued request；
- 现有 static/auth/USB host tests 全量回归。

### 10.6 完成标准

- `DYNAMIC_PROTOCOL.md` 与 header constants 一致；
- 旧 Python v2 client 的现有测试全部通过；
- 新 opcode 不改变静态命令信息泄露/认证校验顺序；
- 不存在动态 readback 路径。

### 10.7 Phase 5 实际完成记录

Phase 5 已完成。现有 v2 32-byte protocol 已加入 `DYNAMIC_BEGIN (0x20)`、
`DYNAMIC_DATA (0x21)`、`DYNAMIC_CLEAR (0x22)` 和 `CAPABILITIES (0x23)`。
动态对象使用 `0xff` sentinel；protocol context 只保存 request ID、total、received
和 active/deadline metadata，实际 staging bytes 仍由 Phase 2 的 dynamic store
持有，没有新增第二套 256-byte protocol buffer。

#### Dispatch、事务和生命周期边界

- common frame/version/status/payload-length/tail validation 先执行；之后
  CAPABILITIES 和 dynamic branch 直接处理，static LIST/GET/SET/CLEAR 才进入
  static management authorization gate。
- dynamic branch 不调用 static management access，不刷新 auth session，不调用
  static slot/Settings API，不把 dynamic object 放入 LIST，也没有 dynamic GET 或
  protocol EXECUTE；所有成功/错误 response 都不包含 dynamic text。
- BEGIN 支持默认 TTL `300s` 和显式 `1..86400s` TTL；DATA 严格检查 slot、total、
  request ID、连续 offset、chunk 长度和 ASCII/control alphabet。合法最终 chunk
  才由 dynamic store 原子替换 committed；失败、重复、乱序、非法文本、timeout
  和 transport/auth discard 只清 staging 并保留旧 committed。
- dynamic transaction timeout 为 `30s`，与 committed TTL 分离；每个合法 BEGIN
  和非最终 DATA 刷新 timeout。合法 CLEAR 同时清 committed/staging/TTL 且幂等；
  malformed CLEAR/CAPABILITIES 保留 dynamic staging。版本错误等 common error 对
  BEGIN/DATA 取消 staging，但 CLEAR/CAPABILITIES 保持 staging 不变。
- auth 状态为 OPEN、PROTECTED（无论 session 是否有效）或 ERROR_LOCKED 时，
  dynamic 命令均按自身格式处理，不返回 `AUTH_REQUIRED` 或其他 static auth error。
  protocol 仍观察 auth state；实际 lazy expiry 只丢弃 incomplete staging，不清
  committed，且 dynamic operation 不延长 session deadline。static
  SET/PASSWORD_SET staging 与 dynamic staging 彼此独立。
- CAPABILITIES 返回固定 22-byte metadata：version `1`、object count `1`、
  max length `256`、TTL `300/1/86400`、transaction timeout `30`，当前 lifecycle
  flags 为 boot/TTL/execution-accept/USB-disconnect 四项；不返回动态内容。

#### 测试与构建记录

- protocol host tests 覆盖 capability、`1/22/23/256` bytes、默认/显式 TTL、
  request ID/total/offset、duplicate/out-of-order/zero/oversized/invalid text、
  restart、final ACK 重传、CLEAR 幂等、static staging isolation、auth bypass、
  lazy expiry、wrong-version staging 规则、discard 和 no-readback；既有
  static/auth/USB tests 全部回归。
- 在 `zmk-dev` 中运行 `CLANG=gcc ./tests/host/run.sh`：GCC、sanitizer、替代
  编译器四轮全部通过；`git diff --check` 通过。
- Totem 当前完整 `leen-display` 配置在 `zmk-dev` 中 off/on 均成功链接，只有既有
  `leen-display` `ZMK_TRANSPORT_NONE` switch warning：

| 构建 | Flash | RAM | map 关键状态 |
|---|---:|---:|---|
| Phase 5 off | 429632 B | 193294 B / 262144 B（73.74%） | dynamic/protocol dynamic symbols absent |
| Phase 5 on | 431784 B | 194174 B / 262144 B（74.07%） | dynamic state `0x228`；protocol timeout work/mutex/owner present |

相对 Phase 5 off，dynamic on 总增量为 Flash `+2152 B`、RAM `+880 B`，剩余 RAM
为 `67970 B`。dynamic store object 未发现 Settings/NVS/static-slot 未解析
依赖；feature-off ELF 未包含 dynamic symbols。未修改 ZMK 主仓库、USB lifecycle
或 Python client。

#### 阶段边界与后续衔接

本阶段修改 `include/zmk/runtime_macro_protocol.h`、
`src/runtime_macro_protocol.c` 和 `tests/host/runtime_macro_protocol_test.c`。
下一阶段只可在用户确认后接入实际 USB/BLE lifecycle clear policy；不得在
protocol 中加入 client、readback 或额外认证。

---

## 11. 阶段 6：生命周期和 clear policy

### 11.1 目标

把 committed/staging clear 接入真实生命周期边界，同时避免复用认证的“保守 reset”策略造成误清。

### 11.2 Kconfig 最终定义

Phase 0 的生命周期决策已落实为三个独立选项：

- `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_USB_DISCONNECT`：默认 `y`，依赖
  `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC`；
- `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_BLE_PROFILE_CHANGE`：默认 `n`，依赖
  dynamic 和 `CONFIG_ZMK_BLE`；
- `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_ENDPOINT_CHANGE`：默认 `n`，依赖
  dynamic。

dynamic 仍是显式 opt-in 且依赖 management USB HID；这些 policy 不会反向启用
USB。CAPABILITIES 的 lifecycle flags 与实际 Kconfig 条件一致，BLE flag 额外受
`CONFIG_ZMK_BLE` 门控。BLE/profile 与 endpoint listener 源文件仅在相应可选 policy
启用时编译。

### 11.3 USB 规则

- 每个 USB connection notification 仍重置 auth/protocol/queue；这与 committed
  dynamic clear 是两条独立路径；
- 只有 raw USB status 在处理前后稳定且为 `USB_DC_DISCONNECTED`，并且 event state
  与 raw-status mapping 一致（`ZMK_USB_CONN_NONE`）时，默认 policy 才清 committed
  和 staging；
- `RESET/CONFIGURED/SUSPEND/RESUME/UNKNOWN/ERROR` 不因 transport/auth reset
  自动清 committed text；
- reset 时先在 transport mutex 下置 offline、增加 generation、重置 auth、discard
  protocol、purge queue，再执行 dynamic clear；因此并发旧 generation 的 final DATA
  不得在真实 disconnect 后 commit；
- stale event/raw status 不重新发布 HID，endpoint ownership recovery 与 committed
  clear 也保持在同一 transport boundary 内；
- host OS shutdown 不承诺一定被识别为 disconnect。

### 11.4 Bluetooth/output 规则

- 默认不订阅或不执行 clear，保证 USB host A 上传后切到 BLE host B 时仍可执行；
- profile policy 使用 `zmk_ble_active_profile_changed`，只在显式启用且 `ZMK_BLE`
  可用时订阅；
- endpoint policy 使用 `zmk_endpoint_changed`，文档语义是 selected endpoint change，
  不是 preferred output 写入；
- 自动 fallback、BLE connect/disconnect 导致 selected endpoint 变化时，启用该 policy
  即按实际 event clear；
- 不修改 ZMK endpoint/output behavior 来制造新事件。

### 11.5 Tests

已覆盖：

- actual disconnect policy on/off；
- `RESET/CONFIGURED/SUSPEND/RESUME/UNKNOWN/ERROR` 不误清；
- auth/transport reset 与 committed preservation；
- disconnect 与 queued final DATA 的 generation race；
- profile/selected endpoint policy 的 on/off 行为及默认保留；
- USB host A → BLE/output 切换时保留 dynamic data；
- lifecycle clear 不影响 static slots 或 credential；
- committed/staging clear 路径 zeroize；
- 无 BLE 时 profile policy 被 Kconfig 阻止，capability header 不错误宣称 BLE flag；
- 既有 static/auth/protocol/USB/dynamic host tests 全量回归。

### 11.6 完成标准

- 认证 reset 和动态 clear 是两套可独立解释、独立测试的策略；
- 默认配置支持跨设备转发用例；
- 文档不把 suspend、UNKNOWN、error 或 host shutdown 描述成可靠 disconnect；
- off/on 目标固件和可选 policy 构建均通过。

### 11.7 Phase 6 实际完成记录

Phase 6 已完成。新增 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_USB_DISCONNECT`、
`..._ON_BLE_PROFILE_CHANGE` 和 `..._ON_ENDPOINT_CHANGE`，并新增
`src/runtime_macro_dynamic_lifecycle.c`。默认行为是实际 management USB disconnect
清除 committed/staging；BLE profile 和 selected endpoint 变化默认保留。

`src/runtime_macro_usb_hid.c` 现在在同一 transport mutex 下完成 offline、generation
递增、auth/protocol reset、queue purge、稳定 raw USB status 与 event mapping 判断、
endpoint recovery 和按 policy 的 dynamic clear。`src/runtime_macro_dynamic_lifecycle.c` 只在可选
policy 开启时订阅 ZMK 的 `zmk_ble_active_profile_changed` 或 `zmk_endpoint_changed`。
未修改 ZMK 主仓库、静态 slot、认证数据或 Python client；CAPABILITIES lifecycle
flags 会按实际 policy 动态反映。

#### Phase 6 验证记录

- 容器内 `CLANG=gcc ./tests/host/run.sh`：GCC、sanitizer、替代编译器五轮全部通过，
  包含 USB policy on/off、profile/endpoint 默认保留和无 BLE policy gate；
- 当前完整 `leen-display` Totem 配置使用 `nice_nano//zmk`、`ZMK_EXTRA_MODULES`
  完整路径和全新 pristine build，在 `zmk-dev` 中 off/on 均成功链接；仅有既有
  `leen-display` 的 `ZMK_TRANSPORT_NONE` switch warning；

| 构建 | Flash | RAM | map / config 关键状态 |
|---|---:|---:|---|
| Phase 6 off | 429632 B | 193294 B / 262144 B（73.74%） | dynamic config/source/state absent |
| Phase 6 on，默认 policy | 431880 B | 194174 B / 262144 B（74.07%） | USB disconnect=y；profile/endpoint=n；dynamic state `0x228` |
| profile+endpoint policy on | 431936 B | 194174 B / 262144 B（74.07%） | optional lifecycle source compiled successfully |

相对 off，默认 dynamic on 增量为 Flash `+2248 B`、RAM `+880 B`，剩余 RAM 为
`67970 B`。off map 没有 dynamic state；on map 有 `.bss.runtime_macro_dynamic_state`
且 dynamic object 没有 Settings/NVS/static-slot 未解析依赖。所有改动未修改 ZMK 主仓库，
当前工作区仍待主 agent 提交。

---

## 12. 阶段 7：Python client 和 CLI

### 12.1 目标

扩展现有参考客户端，使后台服务可探测、上传和 clear 动态宏，但不能 read back。

### 12.2 API 计划

在 `RuntimeMacroClient` 增加名称经阶段 0 确认的 API，例如：

- `get_capabilities()`；
- `upload_dynamic(data: bytes, ttl_seconds: int | None = None)`；
- `clear_dynamic()`。

不得增加 `get_dynamic()`。

### 12.3 CLI 计划

建议命令：

- `capabilities`；
- `dynamic-set`：复用 `--text`、`--stdin`、`--file` 三种互斥输入，并增加可选 `--ttl`；
- `dynamic-clear`。

输入在发送前完成：

- byte length 1..256；
- ASCII/control 校验；
- TTL 范围校验；
- feature capability 检查。

### 12.4 Retry 语义

- 一次 upload 的 BEGIN 和 DATA 共用 request ID；
- transport timeout/error 或 `BAD_REQUEST/BAD_OFFSET` 时，以新 request ID 从 BEGIN 重传；
- final ACK 丢失时允许完整重传，最终 committed 内容相同且 TTL 从新 commit 重新开始；
- 非恢复类错误立即报告；
- capability 不支持时给出明确“firmware does not support dynamic macro”，不能降级为 static SET；
- 不自动登录，不读取或保存密码；
- dynamic 操作不改变现有客户端认证状态机。

### 12.5 Fake-HID tests

- capability supported/unsupported/malformed；
- 1/22/23/256 bytes chunking；
- default/explicit TTL wire bytes；
- restart with new request ID；
- final ACK loss；
- invalid text/length/TTL 在本地拒绝且无 HID write；
- response opcode/request ID/slot/offset/total/tail 严格校验；
- response 夹带 payload 时拒绝；
- dynamic-clear retry；
- PROTECTED 未登录时 dynamic upload 不触发 login；
- 所有现有 auth/static CLI tests 回归。

### 12.6 完成标准

- Python library 和 CLI 使用同一组 wire constants；
- 无 readback API；
- background service 可直接调用 library API；
- `python -m unittest`、`py_compile` 和 Ruff 检查通过。

### 12.7 Phase 7 完成记录

Phase 7 已实现，范围严格限定为参考 Python client/CLI，没有修改桌面应用代码：

- `RuntimeMacroClient.get_capabilities()` 严格解析固定 22-byte CAPABILITIES payload，拒绝未知版本、
  保留 lifecycle flags、错误对象数量、长度或 TTL 元数据；`BAD_OPCODE` 转换为明确的
  `DynamicUnsupportedError`，不回退到 static SET；
- `upload_dynamic(data, ttl_seconds=None)` 在首次 HID write 前完成长度、ASCII/control 和 TTL 校验，
  先探测能力，再以最多 22 bytes 分块发送 BEGIN/DATA；同一上传使用同一 request ID，超时、
  `BAD_REQUEST`、`BAD_OFFSET` 以新 request ID 从 BEGIN 完整重启；
- `clear_dynamic()` 先探测能力，再对 DYNAMIC_CLEAR 做可恢复重试；没有 `get_dynamic()` 或其他
  readback API；dynamic 操作不触发 login、不改变 static/auth client state；
- CLI 增加 `capabilities`、`dynamic-set`、`dynamic-clear`，输入接口复用 `--text`、`--stdin`、
  `--file`，支持 `--ttl`；
- fake-HID 覆盖 capability supported/unsupported/malformed、1/22/23/256-byte chunking、默认和
  显式 TTL、transport timeout、`BAD_OFFSET` restart、clear retry、输入零写入保证、response metadata
  校验和 PROTECTED 未登录场景；既有 static/auth 测试保持通过；
- 更新 `docs/CLI.md`，记录 library/CLI 使用方式和 dynamic 安全边界。

独立验证结果：`python3 -m unittest discover -s tests/python` 共 59 tests 通过，
`python3 -m py_compile tools/runtime_macro_cli.py tests/python/test_runtime_macro_cli.py` 通过，
`ruff check tools/runtime_macro_cli.py tests/python/test_runtime_macro_cli.py` 通过，
`git diff --check` 通过。修改文件为 `tools/runtime_macro_cli.py`、`tests/python/test_runtime_macro_cli.py`、
`docs/CLI.md`、`docs/DYNAMIC_DESKTOP_APP_SPEC.md` 和本计划文档。桌面应用实施规范已作为
Phase 7 交接产物输出；Phase 8 继续负责桌面应用跨组件集成、实物验证和最终文档回归。

---

## 13. 阶段 8：集成、硬件验证和文档

### 13.1 自动化回归

在 devcontainer 中执行：

1. host C tests，环境无 Clang 时使用 `CLANG=gcc`；
2. GCC sanitizer host tests；
3. Python unit tests、`py_compile`、Ruff；
4. static-only feature-off build；
5. dynamic-enabled central USB build；
6. split peripheral build；
7. USB transport-off compatibility build；
8. Studio/CDC ACM 共存 build；
9. 目标 dongle 完整 build 和 map 分析。

必须记录：

- feature off/on Flash 差值；
- `.bss/.data` 差值；
- 最终 RAM 使用和剩余量；
- 新增 stack/workqueue 风险；
- 已知未覆盖项。

### 13.2 实物测试脚本

#### A. 基础上传与执行

1. USB host A 枚举 management HID；
2. 上传固定测试串，含字母、数字、标点、LF、Tab、Backspace；
3. 按一次 `&runtime_macro_dynamic`；
4. 核对普通键盘输出；
5. 再按一次，确认已消费且无输出。

#### B. Busy

1. 启动长 static macro；
2. 在执行中按 dynamic behavior；
3. 确认返回 busy/无动态输出且 dynamic 保留；
4. static 完成后再次按键，确认 dynamic 完整输出并消费；
5. 反向测试 dynamic running 时触发 static。

#### C. USB A 管理、BLE B 输出

1. A 通过 USB 保持 management HID 连接；
2. 键盘选择 BLE profile B 和 `OUT_BLE`；
3. A 上传 dynamic text；
4. 按 dynamic behavior；
5. B 收到完整文本，A 不收到普通键盘文本；
6. A 的 management channel 仍可继续上传/clear。

#### D. TTL

1. 分别上传默认 TTL 和短显式 TTL 测试值；
2. 到期前执行成功；
3. 到期后执行无输出；
4. 到期边界 recommit 不被旧 TTL work 清除。

#### E. Lifecycle

1. output/profile 切换默认保留；
2. 拔掉实际 management USB 默认清除；
3. suspend/resume 不被当成可靠 disconnect；
4. reboot/power-cycle 清除；
5. 打开可选 output/profile clear 后重复验证；
6. 验证 host shutdown 结果只作为观察记录，不形成跨平台保证。

#### F. Static/auth regression

1. static LIST/GET/SET/CLEAR 正常；
2. OPEN/PROTECTED login/password replacement/LOCK 正常；
3. dynamic 不出现在 LIST/GET；
4. dynamic 操作不延长 authenticated session；
5. credential 和 static NVS 数据在 dynamic clear/reboot 测试后保持正确。

### 13.3 文档更新

- `README.md`
- `README.zh-CN.md`
- `docs/CLI.md`
- `docs/PLAN.md` 状态
- `docs/DYNAMIC_PROTOCOL.md`
- host test README

文档必须明确：

- RAM-only、默认单次消费（可选执行后保留）、TTL、无 readback；
- 动态通道第一版不受 static password gate 保护；
- USB 流量不加密；
- 不能可靠识别 host OS shutdown；
- `&runtime_macro_dynamic` keymap 示例；
- USB A → BLE B 的正确操作顺序；
- 支持的平台与尚未完成的硬件验证。

### 13.4 完成标准

- 自动化矩阵全部通过；
- 目标固件 RAM 余量获用户接受；
- 实物 USB 与 BLE 工作流通过；
- 文档与最终 wire 完全一致；
- 独立 release/readiness review 无阻塞问题。

---

## 14. 需求—阶段—测试追踪表

| 需求 | 实现阶段 | 主要验证阶段 |
|---|---|---|
| 单一 256-byte RAM object | 1–2 | 2、8 |
| 不写 Settings/NVS | 2 | 2、5、8 |
| 原子 replacement | 2 | 2、5 |
| 默认 5 分钟 TTL | 0、2 | 2、8 |
| 无 GET/LIST/readback | 0、5、7 | 5、7、8 |
| 物理 behavior 执行 | 3–4 | 4、8 |
| 共用单执行器 | 3 | 3、8 |
| accepted 后默认消费/可选保留 | 2–4、增量 | 3、4、8、增量测试 |
| busy/start error 保留 | 3–4 | 3、4、8 |
| actual USB disconnect clear | 6 | 6、8 |
| output/profile 默认保留 | 6 | 6、8 |
| 动态命令绕过 static auth gate | 0、5 | 5、7、8 |
| 动态命令不刷新 auth session | 5 | 5、7、8 |
| Python upload/clear | 7 | 7、8 |
| USB A 管理 → BLE B 输出 | 3、6、7 | 8 |

## 15. 明确不做

本计划不包含：

- 多个动态槽；
- 动态 GET/LIST；
- protocol-triggered execute；
- Unicode、中文或 Emoji；
- execution queue 或并发 executor；
- 动态文本写入 Settings/NVS；
- 动态内容加密；
- 进程身份认证或敏感动态模式；
- ZMK Studio protobuf；
- 修改 ZMK 主仓库；
- 保证检测所有 host shutdown；
- 桌面 GUI 实现。

如果后续需要其中任何一项，应单独设计并重新评估 wire compatibility、RAM 和安全边界，不顺带加入第一版。

## 16. 最终交付清单

- [ ] Wire contract 已冻结并批准
- [ ] Feature gate 与 DTS 行为已批准
- [ ] RAM 预算和 feature-on map 已批准
- [ ] Dynamic store/TTL 测试通过
- [ ] Shared executor/consume 测试通过
- [ ] `&runtime_macro_dynamic` 构建与行为测试通过
- [x] Dynamic protocol/capability 测试通过
- [ ] USB/profile/output lifecycle 测试通过
- [ ] Python API/CLI 测试通过
- [ ] 现有 static/auth/USB tests 无回归
- [ ] 所有要求的 devcontainer builds 通过
- [ ] USB A → BLE B 实物流程通过
- [ ] RAM/Flash 最终数据已记录
- [ ] 用户文档与安全边界已更新
- [ ] 每个阶段均已独立 commit、push 并获得进入下一阶段的确认

## 17. 执行后保留选项增量（实现记录）

在保持默认行为不变的前提下，dynamic upload 增加可选的执行后策略：

- 默认 `DYNAMIC_BEGIN` 不带 flags，executor 接受后消费 committed text；
- `DYNAMIC_BEGIN` payload length `1` 或 `5` 时，flags bit 0
  `KEEP_AFTER_EXECUTE=1` 表示执行成功后保留 committed text；
- payload length `0/4` 仍分别表示默认/显式 TTL 的旧编码，保证默认 client
  与旧调用方式兼容；
- `CAPABILITIES.lifecycle_flags` bit 6
  `SUPPORTS_KEEP_AFTER_EXECUTE` 表示固件支持该可选参数；
- 保留策略不改变 TTL、`DYNAMIC_CLEAR`、USB disconnect、可选 lifecycle clear
  或重新上传的清除语义；executor busy/start failure 仍保留 committed text；
- Python API 增加 `keep_after_execute=False`，CLI 增加
  `--keep-after-execute`；桌面应用规范同步更新；
- 仍不提供 dynamic readback，也不改变默认的 RAM-only、非 secret 产品约束。

### 17.1 验证记录

- Host C suite（`CLANG=gcc ./tests/host/run.sh`）：全部通过，覆盖默认消费、保留
  策略、未知 flags、TTL、busy/start failure 和 capability bit 6；
- Python client/CLI：62 tests、`py_compile` 和 Ruff 全部通过；
- Totem 正式 `just totem-dongle`：FLASH `432196 / 811008 B`，RAM
  `194190 / 262144 B`，`zmk.uf2` 成功生成；
- 默认行为保持消费；`--keep-after-execute` 发送 BEGIN flags bit 0，实物验证待
  新固件刷入后进行。
