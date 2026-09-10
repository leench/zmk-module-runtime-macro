# RAM-only Dynamic Macro 多槽位扩展设计（D0）

> **状态：设计已确认，尚未实现。**
>
> 本文冻结的是多槽位扩展的目标设计，不是当前固件行为。当前仓库中的实现仍是
> **1 个 dynamic 对象、最大 256 bytes、`slot=0xff` sentinel、零参数
> `&runtime_macro_dynamic` behavior**，由 [`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md)
> 描述。开始实现前不得把本文的目标 wire、Kconfig、store 或 behavior 当作已交付行为。
>
> 本文确认的决策属于**破坏式升级**：不保留旧客户端、旧固件或旧 keymap 兼容。

## 1. 文档关系

- [`PLAN.md`](PLAN.md)：产品范围、架构和安全边界的总览；
- [`DYNAMIC_MACRO_PLAN.md`](DYNAMIC_MACRO_PLAN.md)：单槽位交付的实施与阶段记录，
  多槽位 backlog 在其 18.1、18.2；
- [`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md)：**当前已交付固件**的 dynamic v1
  wire contract；多槽位实现（D3）落地时必须同步改写为多槽位契约；
- [`DYNAMIC_DESKTOP_APP_SPEC.md`](DYNAMIC_DESKTOP_APP_SPEC.md)：桌面应用实施规范，
  需在 D5 同步；
- 本文：多槽位扩展的目标设计、阶段划分和验收矩阵。

若本文与当前实现文档冲突，以“本文描述目标状态、`DYNAMIC_PROTOCOL.md` 描述已交付
状态”为界；实现开始前先冻结设计，不在代码中静默选择一种解释。

## 2. 已确认决策（冻结）

| 决策项 | 冻结值 |
| --- | --- |
| 兼容策略 | 破坏式升级，直接修改现有 `CAPABILITIES (0x23)`；不保留旧客户端/旧固件兼容 |
| 槽位数量 | 默认最多 `8`，Kconfig 可降低；`object_count` 反映实际配置 |
| 槽位编号 | dynamic opcode 只接受 `0..object_count-1`；`0xff` 及其他值返回 `BAD_SLOT` |
| 每槽最大文本 | `512` bytes（固定，不提供 per-build 长度配置） |
| 清空全部 | **没有** clear-all opcode 或保留 slot 值；客户端逐槽 `DYNAMIC_CLEAR`（默认 8 次） |
| 物理触发 | 只保留参数化 `&runtime_macro_dynamic <slot>`；旧零参数绑定移除并迁移 |
| Readback | 继续没有任何 dynamic text readback（无 GET/LIST/协议内读取） |
| 上传级 lifecycle 策略 | 暂缓（继续使用编译期 USB/BLE/endpoint policy） |
| 并发 | 单 active upload transaction、单 executor、全局互斥、不排队 |
| 每槽状态 | 独立 text、长度、TTL、执行后消费/保留策略 |
| 用途边界 | 仍只允许非秘密临时文本；不因槽位或长度增加而放宽 |

## 3. 范围与非目标

### 3.1 范围内

- 8 个彼此独立的 dynamic 槽位，每槽最大 512 bytes；
- 共享单个 staging buffer 和单 active upload；
- `CAPABILITIES` v2：`capability_version=2`、`object_count=配置槽数`、`max=512`；
- 参数化 dynamic behavior 与 keymap 迁移；
- 单 executor 扩展到 512-byte snapshot，保留 consume-on-accept/busy 语义；
- 单 TTL work item + 每槽 deadline/generation；
- lifecycle clear 作用于全部槽位；
- Python client/CLI 与桌面应用的最小破坏式变更。

### 3.2 明确不做

- 不提供 dynamic `GET`、`LIST`、readback 或协议执行命令；
- 不提供 clear-all opcode/sentinel；清空全部由客户端逐槽完成；
- 不提供上传级 BLE profile/endpoint lifecycle 策略（`DYNAMIC_MACRO_PLAN.md` 18.1）；
- 不支持并发执行、执行队列或第二个 worker；
- 不兼容旧 `0xff` sentinel、旧 capability v1 或旧零参数 behavior；
- 不写入 Settings/NVS，不新增持久化路径；
- 不修改 ZMK 主仓库；
- 不提供加密、认证或秘密用途支持。

## 4. Wire contract（目标状态）

### 4.1 Opcode 保持不变

| 名称 | 值 | 目标语义 |
| --- | ---: | --- |
| `DYNAMIC_BEGIN` | `0x20` | 开始一次上传，目标槽位由 `slot` 指定 |
| `DYNAMIC_DATA` | `0x21` | 向当前 active staging 追加连续 chunk |
| `DYNAMIC_CLEAR` | `0x22` | 只清除 `slot` 指定的单个槽位 |
| `CAPABILITIES` | `0x23` | 只读 capability discovery，返回 v2 元数据 |

32-byte frame、字段偏移、`BAD_*` status 编号、response 全零尾部规则、USB HID report
边界规则保持不变。

实现时需要同步修改的常量（给 D1/D3 的交接清单）：

| 位置 | 现在 | 目标 |
| --- | --- | --- |
| `include/zmk/runtime_macro_protocol.h` `..._CAPABILITY_VERSION` | `1` | `2` |
| 同上 `..._CAPABILITY_OBJECT_COUNT` | `1` | 配置槽数（默认 `8`） |
| 同上 `..._DYNAMIC_MAX_LENGTH` | `256` | `512` |
| 同上 `..._DYNAMIC_SLOT`（= `LIST_SLOT` `0xff`） | dynamic sentinel | 删除，改为 `0..object_count-1` 范围校验 |
| `src/runtime_macro_dynamic_internal.h` `ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN` | `256` | `512`（同步 `_Static_assert`） |
| `src/runtime_macro_executor.c` `RUNTIME_MACRO_EXECUTOR_MAX_TEXT_LEN` | dynamic 下 `256` | `512` |
| `dts/bindings/behaviors/zmk,behavior-runtime-macro-dynamic.yaml` | `zero_param.yaml` | 单 cell binding |
| `Kconfig` | 无槽位选项 | 新增 `ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT` |
| `tools/runtime_macro_cli.py` dynamic 常量 | version `1`、count `1`、max `256` | version `2`、count 配置值、max `512` |

不变：`DYNAMIC_BEGIN` flags 定义、TTL 边界 `300/1/86400`、transaction timeout `30s`、
三个 lifecycle policy Kconfig 名称与默认值、behavior compatible 和 guard。

### 4.2 slot 规则

- dynamic opcode（`0x20..0x23`）的 `slot` **必须**是 `0..object_count-1`；
- `0xff`（旧 dynamic sentinel 和 static `LIST` 的 slot 值）以及任何越界值返回
  `BAD_SLOT`；dynamic 分支不得再把它解释成“单对象”或“全部”；
- static `LIST` 继续使用 `0xff` 作为自己的 slot 语义，不受影响（opcode 不同）；
- `CAPABILITIES` 是全局查询，不与具体槽绑定，但请求仍必须携带合法 slot（客户端
  应固定发送 `0`）；response 原样 echo 请求的 `slot`。

### 4.3 `CAPABILITIES (0x23)` v2

Request 仍是 canonical empty object（`payload_length=0`、`offset=0`、
`total_length=0`、22-byte payload 全零）。Response 仍是 22-byte payload，字段布局
不变，只有值与语义升级：

| Payload byte | Size | 字段 | v2 固定值/语义 |
| ---: | ---: | --- | --- |
| `0` | 1 | `capability_version` | `2` |
| `1` | 1 | `dynamic_object_count` | 配置槽数，默认 `8` |
| `2..3` | 2 | `lifecycle_flags` | 不变的 flag 定义，见下 |
| `4..5` | 2 | `max_dynamic_length` | `512` |
| `6..9` | 4 | `default_ttl_seconds` | `300` |
| `10..13` | 4 | `min_ttl_seconds` | `1` |
| `14..17` | 4 | `max_ttl_seconds` | `86400` |
| `18..21` | 4 | `transaction_timeout_seconds` | `30` |

`lifecycle_flags` 语义保持不变：

| Bit | 名称 | 目标行为 |
| ---: | --- | --- |
| `0` | `CLEAR_ON_BOOT` | `1` |
| `1` | `CLEAR_ON_TTL_EXPIRY` | `1`；按槽到期 |
| `2` | `CLEAR_ON_EXECUTION_ACCEPT` | `1`；按槽消费策略，可被 BEGIN flags 覆盖 |
| `3` | `CLEAR_ON_USB_DISCONNECT` | 默认 `1`，按 Kconfig |
| `4` | `CLEAR_ON_BLE_PROFILE_CHANGE` | 默认 `0`，按 Kconfig |
| `5` | `CLEAR_ON_SELECTED_ENDPOINT_CHANGE` | 默认 `0`，按 Kconfig |
| `6` | `SUPPORTS_KEEP_AFTER_EXECUTE` | `1` |
| `7..15` | 保留 | 必须为 `0` |

`capability_version=2` 同时表示：slot 语义为 `0..object_count-1`、无 `0xff` 特殊值、
无 clear-all、单槽最大长度由 `max_dynamic_length` 给出。客户端必须拒绝非 `2` 的
version 或不符合上述约束的 metadata，不得降级到 v1 语义或 static `SET`。

### 4.4 `DYNAMIC_BEGIN (0x20)`

| 字段 | 目标要求 |
| --- | --- |
| `slot` | `0..object_count-1`，本次上传的目标槽位；否则 `BAD_SLOT` |
| `offset` | 必须为 `0`，否则 `BAD_OFFSET` |
| `total_length` | `1..512`（即 `1..max_dynamic_length`），否则 `BAD_LENGTH` |
| `payload_length` | `0`（默认 TTL）、`1`（flags）、`4`（显式 TTL）、`5`（flags+TTL） |
| `payload[0]` | flags bit 0 `KEEP_AFTER_EXECUTE`；未知 bit 置位返回 `BAD_REQUEST` |
| `payload[1..4]` | 显式 TTL seconds，`1..86400`，little-endian |

语义：

- 全局只有一个 active upload：任何**合法** BEGIN 先取消上一笔未完成 staging
  （无论它属于哪个槽），但不清除任何槽的 committed text 或 TTL；
- BEGIN 不修改目标槽的 committed text；旧值在新值完整提交前始终保留；
- 非法 BEGIN（slot/offset/total/payload/TTL/flags 错误）取消当前 staging，保留所有
  已 committed 槽位，并返回对应错误；
- staging 记录目标槽位，后续 DATA 必须匹配。

### 4.5 `DYNAMIC_DATA (0x21)`

- `slot` 必须等于当前 active staging 的槽位；不匹配返回 `BAD_SLOT` 并取消 staging；
- 没有 active transaction、request ID/total 不匹配、offset 不连续、chunk 长度为
  `0` 或大于 `22`、文本含非法字节、total 超过 `512`：返回对应 `BAD_REQUEST` /
  `BAD_OFFSET` / `BAD_LENGTH` / `INVALID_TEXT`，取消 staging，保留全部 committed；
- chunk 长度仍为 `1..22`；512 bytes 完整上传最多需要 **24** 个 chunk
  （`23 × 22 + 6 = 512`）；
- 最终 chunk 在不可观察到半成品的临界区内，仅把 staging 原子提交到目标槽：替换该
  槽 committed、zeroize 旧值、启动该槽 TTL；其他槽位在任何时刻都不受影响；
- transaction timeout 仍为 `30s`，每个合法 BEGIN 和非最终 DATA 刷新；
- duplicate/out-of-order/旧 request ID 继续按一致性错误处理：取消 staging，保留
  committed。

### 4.6 `DYNAMIC_CLEAR (0x22)`

- `slot` 必须是 `0..object_count-1`；否则 `BAD_SLOT`，且不修改任何状态；
- 只清除该槽的 committed text 和 TTL；若当前 staging 属于该槽，同时取消 staging；
- 其他槽位的 committed、staging、TTL 与 static `SET`/`PASSWORD_SET` staging 完全
  不受影响；
- 幂等：对已空的槽位重复发送仍返回 `OK`；
- **没有** clear-all：客户端要清空全部时必须对 `0..object_count-1` 逐槽发送 CLEAR，
  并把“部分成功”作为可重试状态处理（逐槽结果独立）。

### 4.7 响应规则

- 所有动态成功 response 的 payload 为空（`CAPABILITIES` 除外），只返回 status 和
  进度 metadata；
- 任何错误 response 都不包含动态文本、chunk 内容或其他槽位信息；
- dynamic 分支不返回 `AUTH_REQUIRED`/`AUTH_FAILED`/`CREDENTIAL_INVALID` 等 static
  auth status；auth state 为 OPEN/PROTECTED/ERROR_LOCKED 时动态命令仍按自身规则处理；
- dynamic 操作不刷新、不延长 static authenticated session；auth lazy expiry 只丢弃
  incomplete staging，不清除任何 committed 槽位。

## 5. 物理 behavior 与 keymap

- compatible 保持 `zmk,behavior-runtime-macro-dynamic`，locality 保持 central；
- binding 从 zero-parameter 改为 **1 cell**：`&runtime_macro_dynamic <slot>`；
- 旧零参数引用必须迁移（例如 `leen_totem_dongle.keymap` 的
  `&runtime_macro_dynamic` 变为 `&runtime_macro_dynamic 0`）；本模块不保留兼容
  alias，也不接受 `#binding-cells = <0>` 的旧写法；
- split peripheral 的角色专用 keymap 包装继续有效：`RM_DYN` 展开为
  `&runtime_macro_dynamic <slot>` 或 `&none`，两者都是单个 binding；fail-closed
  guard（`runtime_macro_dynamic_guard.c`）和 `/omit-if-no-ref/` 规则不变；
- 运行期 slot 越界（`slot >= CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT`）由驱动
  安全拒绝：不执行、不消费、不修改状态，返回错误；
- press 调用该槽的执行入口，成功（含 empty/expired 的无副作用结果）返回
  `ZMK_BEHAVIOR_OPAQUE`；release 不触发第二次执行；
- executor busy 返回 `-EBUSY` 且保留该槽 committed text。

## 6. Store、executor、TTL 与 RAM 预算

### 6.1 Store

- `committed`：`SLOT_COUNT × 512` bytes，每槽独立；
- `staging`：单个共享 `512`-byte buffer；
- 每槽 metadata：`length`（u16）、`valid`、`consume_on_accept`、`ttl_deadline_ms`
  （i64）、`ttl_generation`（u32）；
- 全局 metadata：active staging 槽位、staging 期望/已收长度、staging TTL 与 flags、
  TTL work generation；
- 单一 mutex 串行化 begin/append/commit/clear/expire/execute handoff；
- 所有 clear、替换、到期、取消和 executor 完成路径继续 zeroize 对应 RAM；
- 不调用 `zmk_runtime_macro_slot_set()`、`zmk_runtime_macro_slot_clear()`、
  `settings_save_one()`、`settings_delete()`。

目标私有 API（命名可在 D1 微调，语义冻结）：

```c
int  zmk_runtime_macro_dynamic_begin(uint8_t slot, size_t total_length,
                                     uint32_t ttl_seconds, bool consume_on_accept);
int  zmk_runtime_macro_dynamic_append(uint8_t slot, size_t offset,
                                      const uint8_t *data, size_t length);
void zmk_runtime_macro_dynamic_cancel_staging(void);
void zmk_runtime_macro_dynamic_clear_slot(uint8_t slot);
void zmk_runtime_macro_dynamic_clear_all(void);   /* lifecycle 专用 */
void zmk_runtime_macro_dynamic_check_expiry(void);
int  zmk_runtime_macro_dynamic_execute(uint8_t slot);
```

### 6.2 TTL

- 仍然只有**一个** delayable TTL work item；
- 每次 commit/clear/re-arm 后扫描所有有效槽位，把 work 重新排到最早的 deadline；
- 每个槽位有独立 `ttl_deadline_ms` 和 `ttl_generation`；handler 只在
  `now >= deadline` 且 generation 匹配时清除该槽，然后继续 re-arm 到下一个最早
  deadline；
- 旧 work 不得清除较新的 commit；某槽到期只清该槽，不影响其他槽。

### 6.3 Executor

- 仍是唯一 executor：单个 busy 状态、单个 `k_work_delayable`、单个 executor-owned
  snapshot；
- feature on 时 snapshot 容量从 `256` 扩到 `512 + 1` terminator；feature off 仍使用
  static `CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN`；
- static 与 dynamic 共享 executor，跨槽位同样全局互斥：任一槽位执行中，其他动态槽位
  或 static macro 都返回 `-EBUSY`，不排队、不并发；
- 只有 executor 接受 snapshot 后才按该槽 `consume_on_accept` 消费；busy 或
  schedule/start failure 一律保留；
- 不新增动态专用 worker、第二执行器或 raw HID report 路径。

### 6.4 RAM 预算

当前 Totem dongle 基线（`leen-display raw_hid_adapter leen_totem_dongle`，Phase 7/8
记录）：

- Flash `432196 / 811008 B`；
- RAM `194190 / 262144 B`（余量 `67954 B`）；
- map：`.bss.runtime_macro_dynamic_state = 0x230`（560 B），executor state `0x114`
  （276 B）。

目标结构的主要增量：

| 项目 | 现在 | 目标 | 增量 |
| --- | ---: | ---: | ---: |
| committed buffers | 256 B | `8 × 512 = 4096 B` | +3840 B |
| shared staging | 256 B | 512 B | +256 B |
| 每槽 metadata（8 槽） | 内联在对象中 | 约 8 × 20–24 B | 约 +150–190 B |
| 全局 metadata | 约 40 B | 约 40–64 B | 约 +0–24 B |
| executor snapshot | 276 B | 约 532 B | +256 B |

合计预期 **约 +4.5 KB RAM**（区间约 4.4–5.0 KB），完成后余量约 `63 KB`。
D1 必须用全新 build 目录和 map 实测确认，并记录 `.bss.runtime_macro_dynamic_state`
与 executor state 的实际大小；若链接失败或余量不足，先报告并等待用户决定，不静默
缩小其他模块内存或牺牲稳定性。

## 7. Kconfig 与 DTS

- 新增 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT`：`int`，默认 `8`，范围
  `1..8`，依赖 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC`；
- 每槽最大长度固定为 `512`，不作为 Kconfig 暴露（内部常量 +
  `_Static_assert`）；
- `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC` 继续保持显式 opt-in、依赖
  `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`，不反向启用 USB；
- 三个 lifecycle policy Kconfig 名称和默认值不变；`CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO_DYNAMIC`
  的门控条件不变（节点被引用、feature 已启用、central/unibody）；
- DTS binding 的 `#binding-cells` 由 `0` 改为 `1`，compatible、`display-name`、
  `/omit-if-no-ref/` 逻辑和 guard 不变；
- devicetree/Kconfig/CMake 的 central-only 与 split peripheral 边界不变。

## 8. Python client、CLI 与桌面应用最小变更

### 8.1 Python client/CLI

- 常量升级：`capability_version=2`、`object_count=默认 8`、`max=512`；
- `DynamicCapabilities` 校验 v2：version 必须 `2`、count 在 `1..8`、`max=512`，
  否则报告 malformed capability，不发送写入；
- 破坏式 API：`upload_dynamic(slot, data, ttl_seconds=None, *, keep_after_execute=False)`
  与 `clear_dynamic(slot)`；
- 新增客户端便利方法 `clear_all_dynamic()`：按 `0..object_count-1` 逐槽发送 CLEAR，
  逐槽独立记录成功/失败，不引入 clear-all wire 请求；
- CLI：`dynamic-set` 增加 `--slot`，`dynamic-clear` 增加 `--slot` 与 `--all`；
- 输入校验在首次 HID write 前完成（slot 范围、`1..512` 长度、ASCII/control、TTL）；
- 无 `get_dynamic()` 或任何 readback API；dynamic 操作不自动登录、不改变 static
  auth 状态机。

### 8.2 桌面应用

- 增加槽位选择（`0..object_count-1`），上传/清除都带槽位；
- “清空全部”实现为逐槽 CLEAR 循环，并显示部分失败；
- 无 readback 时 UI 只能显示**本地观察状态**：本次会话是否上传成功、是否清除、已知
  错误，以及 capability 元数据；
- UI 必须明确标注这些状态不是设备真值（无法读取设备上的占用、长度或剩余 TTL），
  不得通过 static `LIST`/`GET` 推断 dynamic 内容；
- 日志和诊断不得记录 dynamic text 或完整 frame；
- 桌面实现不属于本阶段（Phase 8 的桌面集成仍未完成）。

## 9. 生命周期与安全边界

### 9.1 生命周期

- USB disconnect（默认）、BLE profile change（可选）、selected endpoint change（可选）
  policy 继续按 Kconfig 生效，清除**全部**槽位的 committed/staging/TTL；
- 编译期 policy 保持不变；上传级 per-slot lifecycle 策略属于暂缓项；
- reboot/reset 仍由 RAM 初始化清空全部槽位；
- 上述 clear 与认证 transport reset 是两条独立路径，互不替代。

### 9.2 安全边界（不放宽）

- dynamic 通道仍未加密、未经 static 认证 gate 保护，也不刷新认证 session；
- 仍**只能**用于非秘密临时文本：不得传输、保存、注入或依赖 OTP/验证码、密码、
  PIN、token、API key 或任何凭据；
- 槽位数、长度和 readback 状态的变化都不改变该信任模型；需要秘密场景必须另行设计
  独立的认证加密通道；
- 固件日志只记录 slot、长度、状态和错误码，不记录文本或 chunk；
- 测试失败输出使用固定假数据，不打印真实秘密。

## 10. 测试矩阵

| 层次 | 必须覆盖 |
| --- | --- |
| Store（`tests/host/runtime_macro_dynamic_store_test.c`） | 每槽独立 committed 与 TTL；某槽到期只清该槽；跨槽 BEGIN 替换 staging 但保留所有 committed；跨槽 DATA slot 不匹配；clear_slot 幂等且不影响其他槽；`1/22/23/256/511/512` 长度；`0` 和 `513` 拒绝；全字符集与非法字节；generation/deadline race；zeroize；Settings/static-slot 调用为 0 |
| Executor（`tests/host/runtime_macro_executor_test.c`） | 512-byte snapshot；static 与不同槽位动态全局 busy；consume-on-accept 与 keep 策略；busy/start error 保留；执行中 replacement 不影响 snapshot；完成/失败 zeroize |
| Behavior（`tests/host/runtime_macro_dynamic_behavior_test.c`） | 参数化 slot 执行；越界 slot 安全拒绝且不消费；release 不二次执行；empty/expired 无副作用 |
| Protocol（`tests/host/runtime_macro_protocol_test.c`） | capability v2 字段；`0xff` 与越界 slot → `BAD_SLOT`；每槽 BEGIN/DATA/CLEAR 语义；完整 512-byte（24 chunk）上传；1/22/23/256/511/512 边界；duplicate/out-of-order/timeout/丢 ACK 重传；CLEAR 幂等与逐槽清空全部；static `SET`/`PASSWORD_SET`/auth session 隔离；OPEN/PROTECTED/ERROR_LOCKED；无 readback 与响应不含文本 |
| Lifecycle（`tests/host/runtime_macro_dynamic_lifecycle_test.c`） | policy on/off 清除全部槽位；不误清 static slots/credentials；USB disconnect 与 queued final DATA 的 generation race |
| Python（`tests/python/test_runtime_macro_cli.py`） | capability v1/v2 校验、malformed 拒绝；slot 参数与越界拒绝；chunking；默认/显式 TTL；restart 与 final ACK loss；`clear_all_dynamic()` 逐槽循环与部分失败；本地校验零 HID write；PROTECTED 未登录不触发 login |
| 构建矩阵 | static-only、dynamic off/on、USB transport-off、Studio/CDC 共存、split central、split peripheral（角色专用 wrapper）、Totem dongle 完整 build + map 对比 |
| 实物（用户暂缓） | 按 `DYNAMIC_MACRO_PLAN.md` 13.2 A–F 扩展为逐槽版本；本轮不执行 |

## 11. 阶段划分（D0–D6）

| 阶段 | 内容 | 主要产物 | 门禁 |
| --- | --- | --- | --- |
| D0 | 设计冻结 | 本文档；`DYNAMIC_MACRO_PLAN.md` 18.2 链接 | 用户确认设计决策（已完成），不进入实现 |
| D1 | Store 与 RAM 基线 | Kconfig `SLOT_COUNT`、多槽 store、TTL work、map 实测 | RAM 增量实测；Settings 零调用；store 测试通过 |
| D2 | Executor 与 behavior | 512-byte snapshot、参数化 behavior、keymap/wrapper 迁移说明 | 单 executor/全局 busy；现有静态测试无回归 |
| D3 | Protocol 与 capability v2 | `0x23` v2、per-slot BEGIN/DATA/CLEAR、`0xff` → `BAD_SLOT`；改写 `DYNAMIC_PROTOCOL.md` | wire 表与 header 常量一致；旧 dynamic 测试按新契约更新 |
| D4 | Lifecycle 多槽 clear | `clear_all` 接入 USB/BLE/endpoint policy | policy on/off；不误清 static/auth |
| D5 | Python/CLI 与桌面规范 | 破坏式 API、`--slot`/`--all`、桌面规范同步 | Python 测试/Ruff；无 readback |
| D6 | 集成与文档回归 | 完整回归、实物脚本、README/PLAN/桌面文档、最终 RAM/Flash 记录 | 自动化矩阵通过；实物验证按用户安排（当前暂缓） |

每个阶段继续遵守项目流程：用户确认开始 → 只实现该阶段范围 → 测试与审查 →
`zmk-dev` devcontainer 构建并记录 RAM/Flash → 独立 commit、push → 用户确认后进入
下一阶段。worker 不直接 commit；冲突文档先改文档再改代码。

## 12. 风险与未决项

- **RAM**：预期约 +4.5 KB，D1 必须实测；executor snapshot 和 store 都不得放在栈或
  workqueue 小栈路径上；
- **wire 破坏性**：`DYNAMIC_PROTOCOL.md`、Python client、桌面应用测试在新的契约下
  需要同步改写；过渡期间文档必须明确“已交付 v1 / 目标 v2”的区别；
- **keymap 迁移**：`leen_totem_dongle.keymap` 的零参数引用和配置仓库需要同步；本阶段
  不修改配置仓库；
- **桌面状态表达**：无 readback 决定了 UI 只能显示本地观察状态，需要产品确认文案；
- **实物验证**：用户已暂缓，D6 完成标准和 Phase 8 的实物项在验证前不得标记完成；
- **上传级 lifecycle**：仍为 backlog，多槽位实现不得顺带引入。

---

## 13. D1 实施记录

### 13.1 状态

D1 已完成并已在 `zmk-dev` devcontainer 内验证；**D2 尚未开始**，因此以下目标行为仍
未交付给用户：

- wire contract 仍是 v1 单对象（`0xff` sentinel、capability v1、最大 `256`）；
- 物理 behavior 仍是零参数，只作用于旧 slot 0；
- executor snapshot 仍是 `CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN` 与旧
  `256`-byte 上限；
- 本阶段没有修改 `DYNAMIC_PROTOCOL.md`、protocol opcode、Python client/CLI、
  桌面应用、DTS/binding、behavior 或 ZMK 主仓库。

### 13.2 已实现内容

- 新增 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT`（`int`，默认 `8`，范围
  `1..8`，`depends on ZMK_RUNTIME_MACRO_DYNAMIC`）；
- `src/runtime_macro_dynamic_internal.h`：每槽固定 `512`-byte committed
  buffer、单共享 `512`-byte staging buffer、每槽 `length/valid/consume`、
  `ttl_deadline_ms`、`ttl_generation`，以及全局 `ttl_generation`/`ttl_work_generation`；
- `src/runtime_macro_dynamic.c`：slot-aware 的 begin/append/clear/execute、
  `clear_all`、单一 delayable TTL work（总是排到最早 deadline，并在 handler 中重
  新扫描）、每槽到期只清该槽、stale generation 保护、commit/clear/expire/cancel
  全部 zeroize；
- 兼容入口：`begin`/`begin_with_options`/`append`/`execute` 固定作用于 slot 0，
  `clear` 保持 v1 的“整个 dynamic 状态清除”语义（即 `clear_all`），因此现有
  protocol、behavior、USB reset 与 lifecycle 调用点无需修改；
- store 全程不调用 `settings_save_one()`、`settings_delete()`、
  `zmk_runtime_macro_slot_set()`、`zmk_runtime_macro_slot_clear()`（测试用计数器断言为 0）。

### 13.3 D1 明确限制（D2 前置）

- store 已能保存每槽 `512` bytes，但执行仍受当前 executor snapshot 限制：超过
  `ZMK_RUNTIME_MACRO_DYNAMIC_EXECUTABLE_MAX_TEXT_LEN`（`256`）的 committed text
  执行时返回 `-EINVAL`，**绝不截断**，并完整保留在槽内；
- 旧单对象入口（begin/append/execute）继续把长度限制在 `256`，与冻结的 v1 wire
  和当前 executor 一致；
- D2 必须把 executor snapshot 提升到 `512`，然后删除/替换
  `ZMK_RUNTIME_MACRO_DYNAMIC_EXECUTABLE_MAX_TEXT_LEN` 与旧 `256` 常量，并迁移 behavior 到
  参数化槽位绑定。

### 13.4 测试记录

容器内 `CLANG=gcc ./tests/host/run.sh`：GCC、GCC sanitizer、替代编译器、替代编译器
sanitizer 四轮全部通过（含新增的 Kconfig slot-count 静态门禁检查）。

`tests/host/runtime_macro_dynamic_store_test.c` 覆盖：8 槽隔离与 `0..7` 寻址；越界
slot 拒绝（begin/append/execute/clear）且不改动任何 committed；`1/22/23/256/511/512`
长度与 `0/513` 拒绝；512 bytes = 24 chunks；共享 staging 的目标槽记录、跨槽 BEGIN
替换 staging 但保留所有 committed、DATA slot 不匹配取消 staging；同槽 clear 幂等、
不影响其他槽；`clear_all`；每槽独立 TTL 与最早 deadline 单 work；stale generation
（旧 work 不得清除较新的 commit）；commit/clear/expire/cancel 的 zeroize；
Settings/static slot 调用数为 0；clear_all 与 final append 的并发竞态。

既有 protocol、USB HID、executor、lifecycle（policy on/off）host 测试同步迁移到
`runtime_macro_dynamic_state.slots[0U]` 单槽视图，语义与断言未放宽。

### 13.5 构建与 RAM/Flash 实测

全新 build 目录（`west build -b nice_nano//zmk -s /workspaces/zmk/app`，snippet
`studio-rpc-usb-uart`，shield `leen_display raw_hid_adapter leen_totem_dongle`），
配置仓库为当前 commit 的副本（dynamic on），baseline 使用 `0c48c0a` 的模块快照：

| 构建 | Flash | RAM | 余量 |
| --- | ---: | ---: | ---: |
| baseline（D1 之前，dynamic on） | `432196 B` | `194190 B`（74.08%） | `67954 B` |
| D1 dynamic on | `432356 B` | `198398 B`（75.68%） | `63746 B` |
| D1 dynamic off | `429632 B` | `193294 B`（73.74%） | `68850 B` |

D1 相对 baseline：Flash `+160 B`，RAM `+4208 B`，与设计预期（约 +4.2 KB）一致。

map 结论：

- `.bss.runtime_macro_dynamic_state`：baseline `0x230`（560 B）→ D1 `0x12a0`
  （4768 B）；
- `.bss.runtime_macro_executor`：两者均为 `0x114`（276 B），本阶段未改变 executor
  容量；
- `.data.runtime_macro_dynamic_ttl_work`（`0x30`）为单一 TTL work item；
- dynamic off：`compile_commands.json` 与 map 均不含 `runtime_macro_dynamic.c`
  （只有 `runtime_macro_dynamic_guard.c` 的 guard 调试信息），即 feature-off 不编译
  dynamic store；
- USB-off fail-closed：同时请求 `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC=y` 与
  `CONFIG_ZMK_RUNTIME_MACRO_USB_HID=n` 时，Kconfig 将 dynamic 解析为未启用，
  被引用的 dynamic 节点触发 `runtime_macro_dynamic_guard.c` 的
  `#error "runtime_macro_dynamic is referenced but its feature/driver is disabled"`，
  构建按设计失败。

参考客户端回归：`python3 -m unittest discover -s tests/python` 62 tests 通过，
`py_compile` 通过（本阶段未修改 Python/client 代码）。

### 13.6 D1 变更文件

- `Kconfig`：新增 `ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT`；
- `src/runtime_macro_dynamic_internal.h`：多槽 state、常量、slot-aware 与兼容 API；
- `src/runtime_macro_dynamic.c`：多槽 store、单 TTL work、slot/全量 clear、执行边界；
- `tests/host/runtime_macro_dynamic_store_test.c`：重写为多槽覆盖；
- `tests/host/runtime_macro_dynamic_gate_test.c`：更新为 512/slot 常量断言；
- `tests/host/runtime_macro_protocol_test.c`、`tests/host/runtime_macro_usb_hid_test.c`、
  `tests/host/runtime_macro_executor_test.c`、
  `tests/host/runtime_macro_dynamic_lifecycle_test.c`、
  `tests/host/runtime_macro_dynamic_lifecycle_policy_off_test.c`：迁移到 slot 0 视图；
- `tests/host/run.sh`：新增 Kconfig slot-count 静态门禁；
- 本文档。

### 13.7 D2 待办（未开始）

- executor snapshot 提升到 `512 + 1`（并移除 `256` 旧常量）；
- 参数化 `&runtime_macro_dynamic <slot>` behavior 与 DTS binding 迁移；
- keymap 迁移说明与配置仓库同步（含 split peripheral wrapper）；
- 越界槽位在 behavior 层的安全拒绝与不消费。
