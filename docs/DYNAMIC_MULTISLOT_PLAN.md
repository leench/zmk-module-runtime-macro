# RAM-only Dynamic Macro 多槽位扩展设计（D0）

> **状态：D0–D5 已实现并完成相应 host/container 验证；D5 的 Python CLI 子范围
> 已实现（见第 17 节）。桌面端由用户单独处理，不在本仓库实现。D6 集成回归和最终
> readiness review 尚未完成。**
>
> 多槽 store、512-byte executor、参数化 behavior、v2 dynamic wire、多槽 lifecycle
> clear 和 Python CLI 已落地。dongle 已完成 v2 capability、slot 上传/清除和边界 CLI
> 实机验证；物理按键执行、TTL/lifecycle 全流程、桌面端和完整 D6 矩阵仍未完成。
> [`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md) 描述实际 v2 contract：
> `CAPABILITIES (0x23)` 返回 capability v2、配置槽数和最大 `512` bytes；dynamic
> opcode 只接受有效 `0..N-1` slot，`0xff` 返回 `BAD_SLOT`。Python CLI 已按该 v2
> contract 实现；桌面 client 未在本仓库同步，不能把当前桌面版本当作 v2 client。
>
> 本文确认的决策属于**破坏式升级**：不保留旧客户端、旧固件或旧 keymap 兼容。

## 1. 文档关系

- [`PLAN.md`](PLAN.md)：产品范围、架构和安全边界的总览；
- [`DYNAMIC_MACRO_PLAN.md`](DYNAMIC_MACRO_PLAN.md)：初版单槽位历史记录和 Phase 8
  收尾记录；当前多槽位阶段状态以本文为准；
- [`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md)：**当前已交付固件**的 dynamic v2
  wire contract；Python CLI 已据此同步，桌面端仍由用户处理；
- [`DYNAMIC_DESKTOP_APP_SPEC.md`](DYNAMIC_DESKTOP_APP_SPEC.md)：桌面应用实施规范；
  桌面端由用户单独处理，本仓库不实现，也不在本轮修改该规范；
- 本文：多槽位扩展的目标设计、阶段划分和验收矩阵。

若本文与实现文档冲突，以 `DYNAMIC_PROTOCOL.md` 的实际 v2 wire contract 和本记录的
阶段状态为准；未完成的 client/lifecycle/hardware 不得写成已交付。

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

实现时需要同步修改的常量（D1/D2 已完成的部分标注在“状态”列）：

| 位置 | 现在 | 目标 | 状态 |
| --- | --- | --- | --- |
| `include/zmk/runtime_macro_protocol.h` `..._CAPABILITY_VERSION` | `1` | `2` | D3 已完成 |
| 同上 `..._CAPABILITY_OBJECT_COUNT` | `1` | 配置槽数（默认 `8`） | D3 已完成 |
| 同上 `..._DYNAMIC_MAX_LENGTH` | `256` | `512` | D3 已完成 |
| 同上 dynamic slot sentinel | `0xff` | 删除；`0..object_count-1` 范围校验 | D3 已完成 |
| `src/runtime_macro_dynamic_internal.h` | 单槽 `256` | `ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN = 512`（多槽 store/executor）；`ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN = 256` 保留为 v1 legacy 上限 | D1/D2 已完成 |
| `src/runtime_macro_executor.c` `RUNTIME_MACRO_EXECUTOR_MAX_TEXT_LEN` | dynamic 下 `256` | `512` | D2 已完成 |
| `dts/bindings/behaviors/zmk,behavior-runtime-macro-dynamic.yaml` | `zero_param.yaml` | 单 cell binding | D2 已完成 |
| `Kconfig` | 无槽位选项 | 新增 `ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT` | D1 已完成 |
| `tools/runtime_macro_cli.py` dynamic 常量 | version `1`、count `1`、max `256` | version `2`、count `1..8`、max `512` | D5 CLI 已完成 |

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
- binding 从 zero-parameter 改为 **1 cell**：`&runtime_macro_dynamic <slot>`（D2 已完成）；
- 旧零参数引用必须迁移为 `&runtime_macro_dynamic 0`，本模块不保留兼容 alias，也不接受
  `#binding-cells = <0>` 的旧写法；当前 `zmk-config-leen` 已按用户要求恢复为单文件
  `boards/shields/leen_totem/leen_totem.keymap`，内容包含 slot `0` 和 Macro layer
  的 dynamic slot `1..5`；
- fail-closed guard（`runtime_macro_dynamic_guard.c`）和 `/omit-if-no-ref/` 规则不变。
  角色专用 wrapper 仅属于此前的 split peripheral 验证方案；当前单文件配置只声明并
  验证 dongle 构建，不把 left/right 构建写成当前已通过；
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

D1/D2 实测（见 13.5、14.5）：store 相对 Phase 8 dynamic-on 基线 `+4208 B` RAM，
executor snapshot `+256 B`，合计 `+4464 B`；最新 dongle dynamic-on 构建余量
`63490 B`。所有阶段均使用全新 build 目录并记录映射。

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

以下 8.1 的 CLI 部分已于 D5 实现（见第 17 节）；本仓库不实现 8.2 桌面端。

### 8.2 桌面应用（由用户单独处理）

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
| Lifecycle（`tests/host/runtime_macro_dynamic_lifecycle_test.c`） | 每槽独立 setup：policy on 清全部 slot + staging + TTL work；部分槽/已到期槽/空槽混合 boundary；boot reset；policy off 保留全部；NULL event 拒绝；Settings/static 调用为 0 |
| Python（`tests/python/test_runtime_macro_cli.py`） | capability v1/v2 校验、malformed 拒绝；slot 参数与越界拒绝；chunking；默认/显式 TTL；restart 与 final ACK loss；`clear_all_dynamic()` 逐槽循环与部分失败；本地校验零 HID write；PROTECTED 未登录不触发 login |
| 构建矩阵 | static-only、dynamic off/on、USB transport-off、Studio/CDC 共存、split central、Totem dongle 完整 build + map 对比；split peripheral 仅在显式采用角色 wrapper 时验证，当前单文件配置不宣称已通过 |
| 实物 | 已完成 dongle v2 capability、slot 上传/清除和 CLI 边界验证；物理按键执行、TTL、busy、USB/BLE lifecycle 和 static/auth 全流程仍待安排 |

## 11. 阶段划分（D0–D6）

| 阶段 | 内容 | 主要产物 | 门禁 |
| --- | --- | --- | --- |
| D0 | 设计冻结 | 本文档；`DYNAMIC_MACRO_PLAN.md` 18.2 链接 | 用户确认设计决策（已完成），不进入实现 |
| D1 | Store 与 RAM 基线（已完成） | Kconfig `SLOT_COUNT`、多槽 store、TTL work、map 实测 | RAM 增量实测；Settings 零调用；store 测试通过 |
| D2 | Executor 与 behavior（已完成） | 512-byte snapshot、参数化 behavior、keymap/wrapper 迁移说明 | 单 executor/全局 busy；现有静态测试无回归 |
| D3 | Protocol 与 capability v2（已完成） | `0x23` v2、per-slot BEGIN/DATA/CLEAR、`0xff` → `BAD_SLOT`；改写 `DYNAMIC_PROTOCOL.md` | wire 表与 header 常量一致；512-byte/slot isolation/逐槽 clear 测试通过 |
| D4 | Lifecycle 多槽 clear（已完成） | `clear_all` 接入 USB/BLE/endpoint policy | policy on/off；多槽 positive/boundary 测试；不误清 static/auth |
| D5 | Python/CLI 与桌面边界 | CLI：破坏式 API、`--slot`/`--all`（已完成）；桌面端：用户单独处理 | Python 70 tests、py_compile、Ruff、CLI 实机基本流程通过 |
| D6 | 集成与文档回归（进行中） | 完整回归、当前配置文档、最终 RAM/Flash 和 readiness review | host/Python/dongle 已有结果；完整矩阵、物理按键流程和桌面端仍未完成 |

每个阶段继续遵守项目流程：用户确认开始 → 只实现该阶段范围 → 测试与审查 →
`zmk-dev` devcontainer 构建并记录 RAM/Flash → 独立 commit、push → 用户确认后进入
下一阶段。worker 不直接 commit；冲突文档先改文档再改代码。

## 12. 风险与未决项

- **RAM**：预期约 +4.5 KB，D1 必须实测；executor snapshot 和 store 都不得放在栈或
  workqueue 小栈路径上；
- **wire 破坏性**：`DYNAMIC_PROTOCOL.md` 已完成 v2 改写，Python CLI 已按 v2 实现；
  桌面应用的对接与测试由用户单独处理，在其完成前不得声称桌面端已支持多槽位；
- **keymap 结构**：配置仓库当前按用户要求使用单文件 `leen_totem.keymap`；本仓库不
  擅自恢复角色拆分。当前只记录 dongle 构建和 dongle 实机 CLI 结果；
- **桌面状态表达**：无 readback 决定了 UI 只能显示本地观察状态，需要产品确认文案；
  该工作属用户侧桌面实现范围，本仓库只提供 wire 契约；
- **实物验证**：已完成 CLI 管理面基本验证，但物理按键执行和 A–F 全流程仍未完成，
  D6 完成标准在验证前不得标记完成；
- **上传级 lifecycle**：仍为 backlog，多槽位实现不得顺带引入。

---

## 13. D1 实施记录

### 13.1 状态

D1 已完成并已在 `zmk-dev` devcontainer 内验证。以下是 D1 完成时的状态；behavior 与
executor 已在 D2 更新（见第 14 节）：

- wire contract 仍是 v1 单对象（`0xff` sentinel、capability v1、最大 `256`）；
- D1 时物理 behavior 仍是零参数，只作用于旧 slot 0（D2 已改为参数化槽位）；
- D1 时 executor snapshot 仍是 `CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN` 与旧
  `256`-byte 上限（D2 已提升到 `512`）；
- D1 没有修改 `DYNAMIC_PROTOCOL.md`、protocol opcode、Python client/CLI、
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

### 13.3 D1 明确限制（D2 已处理，见第 14 节）

- D1 时 store 已能保存每槽 `512` bytes，但执行仍受 executor snapshot 限制：超过
  `256` 的 committed text 执行时返回 `-EINVAL`，**绝不截断**，并完整保留在槽内；
- 旧单对象入口（begin/append/execute）继续把长度限制在 `256`，与冻结的 v1 wire
  一致；
- D2 已把 executor snapshot 提升到 `512`、删除 `..._EXECUTABLE_MAX_TEXT_LEN`，并把
  behavior 迁移到参数化槽位绑定；旧单对象 `256` 上限作为 v1 legacy 上限保留（见 14.2）。

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

### 13.7 D2 待办（已在第 14 节完成）

- executor snapshot 提升到 `512 + 1`（并移除 `256` 旧常量）；
- 参数化 `&runtime_macro_dynamic <slot>` behavior 与 DTS binding 迁移；
- keymap 迁移说明与配置仓库同步（含 split peripheral wrapper）；
- 越界槽位在 behavior 层的安全拒绝与不消费。

---

## 14. D2 实施记录

### 14.1 状态

D2 已完成并已在 `zmk-dev` devcontainer 内验证；D3 随后已完成 protocol v2 实现和
wire 文档更新。当前仍未完成的部分是 Python client/CLI、桌面应用同步、D4 上传级
lifecycle，以及实物验证；readback 和 clear-all wire 仍明确不提供。

### 14.2 已实现内容

- executor（`src/runtime_macro_executor.c`）：dynamic feature on 时
  `RUNTIME_MACRO_EXECUTOR_MAX_TEXT_LEN` 从 `256` 提升到
  `ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_MAX_TEXT_LEN`（`512`），snapshot 为 `512 + 1`
  terminator；static 容量、static/动态共享的单 executor、全局 busy、
  consume-on-accept/keep 语义不变；`_Static_assert` 改为覆盖完整槽位容量；
- store（`src/runtime_macro_dynamic.c`）：删除 D1 的 `-EINVAL` 超长拒绝，512-byte
  committed text 直接交给 executor，不再可能截断；
- behavior（`src/behaviors/behavior_runtime_macro_dynamic.c`）：读取
  `binding->param1` 作为槽位，先按 32-bit 值做范围校验（越界返回 `-EINVAL`，不执行、
  不消费、不修改任何状态），再调用 `zmk_runtime_macro_dynamic_execute_slot()`；
  release 不重复执行，busy/启动失败保留 committed；
- DTS：`dts/behaviors/runtime_macro.dtsi` 的 dynamic 节点 `#binding-cells` 由 `<0>`
  改为 `<1>`；binding YAML 由 `zero_param.yaml` 改为 `one_param.yaml`；
- legacy 单对象入口（`begin`/`begin_with_options`/`append`/`execute`）与
  `ZMK_RUNTIME_MACRO_DYNAMIC_MAX_TEXT_LEN`（`256`）仅作为内部兼容测试入口保留；
  D3 protocol 已改用 slot-aware API，不保留旧零参数 behavior alias。

### 14.3 配置仓库 keymap 迁移（D2 历史记录）

D2 当时先使用角色专用 keymap；随后配置仓库按用户要求恢复为单文件结构。当前状态
记录在第 17.6 节：

- 当前文件为 `boards/shields/leen_totem/leen_totem.keymap`；
- base dynamic binding 为 `&runtime_macro_dynamic 0`，Macro layer 左手第 3 排为
  dynamic slot `1..5`；
- dongle 生成的 devicetree 中 dynamic binding 为 `&runtime_macro_dynamic 0x0`。

### 14.4 测试记录

容器内 `CLANG=gcc ./tests/host/run.sh`：四轮（GCC、GCC sanitizer、替代编译器、
替代编译器 sanitizer）全部通过。新增/更新覆盖：

- `tests/host/runtime_macro_dynamic_behavior_test.c`：slot `0..7` 逐个转发并记录
  实际槽位；越界（`8`、`0xff`、`0x100`、`0xffffffff`）返回 `-EINVAL`、不执行、
  不消费（其中 `0x100` 专门覆盖 32-bit cell 截断回绕风险）；busy/启动失败保留；
  release 不重复执行；empty 无副作用；
- `tests/host/runtime_macro_executor_test.c`：新增 512-byte 槽位快照测试（24 chunk
  上传、`length == 512`、terminator、执行完成后 zeroize/不 busy）；
- `tests/host/runtime_macro_dynamic_store_test.c`：原“超长拒绝”测试改为 512-byte
  槽位与 legacy slot 0 都能完整交给 executor 并在默认策略下消费；
- 既有 static、protocol（v1）、USB HID、auth、lifecycle host 测试保持通过。

Python（D2 当时尚未修改 client）：`python3 -m unittest discover -s tests/python` 62 tests
通过，`py_compile` 通过，`ruff check tools tests/python` 通过；D5 后的当前 CLI 结果见第 17.4 节。

### 14.5 构建与 RAM/Flash 实测

全新 build 目录，配置仓库为当前工作区（dynamic on）。

| 构建 | Flash | RAM | 余量 |
| --- | ---: | ---: | ---: |
| D1 dongle dynamic on（对照） | `432356 B` | `198398 B` | `63746 B` |
| D2 dongle dynamic on（`just totem-dongle`） | `432372 B` | `198654 B` | `63490 B` |
| D2 dongle dynamic off（临时配置副本：`&none` + feature off） | `429632 B` | `193294 B` | `68850 B` |
| D2 `just totem-left` | `197892 B` | `41856 B` | `220288 B` |
| D2 `just totem-right` | `197892 B` | `42084 B` | `220060 B` |

D2 相对 D1：Flash `+16 B`，RAM `+256 B`（executor snapshot `256 → 512`，映射中
`.bss.runtime_macro_executor` 由 `0x114`（276 B）变为 `0x214`（532 B））；
`.bss.runtime_macro_dynamic_state` 仍为 `0x12a0`（4768 B），store 未变。dynamic off
构建尺寸与 D1 的 dynamic-off 构建完全一致（`429632 B` / `193294 B`）。

门控与边界（均为全新 build 目录）：

- dongle `.config`：`CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC=y`、
  `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT=8`、
  `CONFIG_ZMK_BEHAVIOR_RUNTIME_MACRO_DYNAMIC=y`；
- peripheral（left/right）：`.config` 无 dynamic，生成的 devicetree 中 dynamic
  引用数为 0，`compile_commands.json` 只包含 `runtime_macro_dynamic_guard.c`
  （store/behavior 均未编译），映射中只有该 guard 的零长度对象；
- feature off（dongle keymap 使用 `&none` + `CONFIG_..._DYNAMIC is not set`）：
  `compile_commands.json` 不含 `runtime_macro_dynamic.c` /
  `behavior_runtime_macro_dynamic.c`，映射中无 dynamic store/behavior 符号；
- fail-closed：请求 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID=n` 且 keymap 仍引用
  `&runtime_macro_dynamic 0` 时，构建按设计失败于
  `runtime_macro_dynamic_guard.c` 的
  `#error "runtime_macro_dynamic is referenced but its feature/driver is disabled"`。

### 14.6 D2 变更文件

模块仓库（D2 已提交到本地提交 `4630c76`；D3 追加修改尚未提交）：`src/runtime_macro_dynamic_internal.h`、
`src/runtime_macro_executor.c`、`src/runtime_macro_dynamic.c`、
`src/behaviors/behavior_runtime_macro_dynamic.c`、
`dts/behaviors/runtime_macro.dtsi`、
`dts/bindings/behaviors/zmk,behavior-runtime-macro-dynamic.yaml`、
`tests/host/runtime_macro_dynamic_behavior_test.c`、
`tests/host/runtime_macro_executor_test.c`、
`tests/host/runtime_macro_dynamic_store_test.c`、本文档。

配置仓库（D2 历史记录）：当时使用 `boards/shields/leen_totem/leen_totem_dongle.keymap`；当前已恢复为
`boards/shields/leen_totem/leen_totem.keymap`，并已提交到配置仓库。

### 14.7 D3 交接状态

D3 已完成 protocol/capability v2 实现，并通过 host suite 与 zmk-dev 固件构建验证。
D3 不引入 readback、clear-all wire 或上传级 lifecycle。

## 15. D3 实施记录

### 15.1 实现

- `CAPABILITIES (0x23)` 返回 `capability_version=2`、实际配置槽数和最大
  `512` bytes；请求必须携带有效 dynamic slot，`0xff` 返回 `BAD_SLOT`；
- `DYNAMIC_BEGIN`、`DYNAMIC_DATA`、`DYNAMIC_CLEAR` 已接到 slot-aware store API，
  slot 只接受 `0..N-1`；DYNAMIC_CLEAR 只清单个槽位，客户端逐槽实现全清；
- protocol transaction 保存目标 slot，DATA 的 slot mismatch 会返回 `BAD_SLOT` 并
  取消共享 staging；清除其他槽位不会取消当前 staging；
- v2 dynamic branch 不使用旧 `0xff` sentinel，不刷新 auth session；static
  management staging 和 dynamic staging 仍独立；
- `DYNAMIC_PROTOCOL.md` 已改写为实际 v2 contract；Python CLI 已在 D5 同步并通过测试，
  桌面 client 仍由用户单独处理，不能把旧单槽 client 当作 v2 client。

### 15.2 测试与构建

- `CLANG=gcc ./tests/host/run.sh`：四轮 host 编译/运行全部通过；
- protocol 测试覆盖 capability v2、slot 0/1/2/3、`0xff`/越界、512-byte 上传的
  24 chunks、slot isolation、逐槽 CLEAR、slot mismatch、无 clear-all 和无 readback；
- `just totem-left`、`just totem-right`、`just totem-dongle` 在 zmk-dev 中通过；
  dongle 本次 Flash `432468 B`、RAM `198654 B`，相对 D2 executor 变更前
  `432372 B` / `198654 B` 为 Flash `+96 B`、RAM `+0 B`；
- dongle 仍生成 `&runtime_macro_dynamic 0`，peripheral 仍不保留 dynamic node，
  fail-closed guard 未放宽；
- host C、Python/CLI 和 desktop client 未因 D3 修改；Python/CLI 与桌面 v2 同步
  属于 D5。

### 15.3 D3 变更文件

- `include/zmk/runtime_macro_protocol.h`
- `src/runtime_macro_protocol.c`
- `src/runtime_macro_dynamic_internal.h`（注释更新）
- `tests/host/runtime_macro_protocol_test.c`
- `tests/host/runtime_macro_usb_hid_test.c`
- `docs/DYNAMIC_PROTOCOL.md`
- `docs/DYNAMIC_MULTISLOT_PLAN.md`

D3 提交为 `cda9d49`，D2 模块/配置提交和 D3 均已按当前阶段要求推送；当前配置结构恢复
提交为 `fb0eaaf`。

---

## 16. D4 实施记录

### 16.1 验证结论（实现本已满足多槽语义）

D4 审查了全部四个 lifecycle 入口，行为已经是“全量清除”：

| 入口 | 位置 | 多槽行为 |
| --- | --- | --- |
| boot/reset | `src/runtime_macro_dynamic.c` 的 `SYS_INIT` → `zmk_runtime_macro_dynamic_reset()` | 清全部 slot、staging、TTL work |
| confirmed USB disconnect | `src/runtime_macro_usb_hid.c` 的 `actual_management_usb_disconnect` 分支 | `clear_all`：全部 slot + staging + TTL |
| BLE profile policy | `src/runtime_macro_dynamic_lifecycle.c` `runtime_macro_dynamic_profile_listener()` | `clear_all`，Kconfig 默认关 |
| endpoint policy | `runtime_macro_dynamic_lifecycle.c` `runtime_macro_dynamic_endpoint_listener()` | `clear_all`，Kconfig 默认关 |

`zmk_runtime_macro_dynamic_clear_all()` 本身只循环清 slot、取消共享 staging 并
重排/取消单 TTL work；它不引用 auth、不引用 Settings/static slot API（本次审查用
grep 确认 lifecycle 源文件对 `auth`、`settings_`、`slot_set`、`slot_clear` 均为零引用）。

唯一代码改动是把 USB/BLE/endpoint 三个调用点从 legacy 别名
`zmk_runtime_macro_dynamic_clear()` 改为显式的
`zmk_runtime_macro_dynamic_clear_all()`，并补充说明注释；行为完全不变，只是把
目标语义写清楚。legacy 别名仍保留并被 store 测试覆盖。

### 16.2 测试补强（独立 setup，未复用同一 mock state）

`tests/host/runtime_macro_dynamic_lifecycle_test.c`（policy on）：

- 8 槽全部 committed + 共享 staging 进行中 → BLE profile listener 清全部 slot、
  staging 和 TTL work；
- 独立 boundary state：部分槽为空、slot 1 带 `1s` TTL 且已过期、slot 4 为
  keep-after-execute、staging 指向空槽 → endpoint listener 清全部并在已空状态幂等；
- boot reset 清全部 slot/staging/TTL work；
- `NULL` event 返回 `-EINVAL` 且不改动任何 slot 或 staging；
- Settings/static slot stub 计数器断言为 0。

`tests/host/runtime_macro_dynamic_lifecycle_policy_off_test.c`（两 policy 均关）：

- 独立 setup（slot 1/3/7 committed + 空槽 5 上的 staging）→ 两个 listener 都保留
  全部 slot、TTL 与 staging，并保持 TTL work 已调度；
- `NULL` event 拒绝且保留；
- Settings/static 调用为 0。

`tests/host/runtime_macro_usb_hid_test.c`：

- 新增 `test_dynamic_disconnect_clears_every_slot()`：8 槽全 committed + staging；
  非 disconnect 通知（suspend/resume/unknown）保留全部 committed slot（staging 被
  USB 逻辑 protocol discard 取消，属既有安全边界）；confirmed
  `USB_DC_DISCONNECTED` + `ZMK_USB_CONN_NONE` 清全部 slot、staging 和 TTL work，
  且 static slot 文本与 settings 写入次数不变；
- policy-off variant 改为逐槽验证全部 slot 在所有 USB 状态下都保留。

policy on/off 由同一 fixture 编译两次（`runtime_macro_usb_hid_test`、
`runtime_macro_usb_hid_policy_off_test`）分别覆盖。

### 16.3 测试与构建结果

- `CLANG=gcc ./tests/host/run.sh`：四轮（gcc、gcc sanitizer、替代编译器、替代
  编译器 sanitizer）全部 PASS；
- zmk-dev 全新 build：`just totem-left` `197892 B` Flash / `41856 B` RAM，
  `just totem-right` `197892 B` / `42084 B`，`just totem-dongle`
  `432468 B` / `198654 B`——与 D3 完全一致（D4 无行为改动）；
- dongle `.config`：`CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC=y`、slot count `8`、
  `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_CLEAR_ON_USB_DISCONNECT=y`、BLE/endpoint policy
  默认关；map：`.bss.runtime_macro_dynamic_state` `0x12a0`（4768 B）、
  `.bss.runtime_macro_executor` `0x214`（532 B）、单一 `.data.runtime_macro_dynamic_ttl_work`；
- peripheral（left/right）：`compile_commands.json` 不含 dynamic store 或 dynamic
  behavior，dynamic 引用为 0，fail-closed guard 未放宽。

### 16.4 D4 范围边界

- 未修改 wire、protocol opcode、Python/CLI、桌面应用、DTS/binding、配置仓库；
- upload 级 lifecycle 策略仍为 backlog，未随多槽位引入；
- lifecycle clear 仍与认证 transport reset 是两条独立路径，互不替代。

### 16.5 D4 变更文件

- `src/runtime_macro_dynamic_lifecycle.c`（显式 `clear_all` + 注释）
- `src/runtime_macro_usb_hid.c`（显式 `clear_all` + 注释）
- `tests/host/runtime_macro_dynamic_lifecycle_test.c`（多槽 positive/boundary）
- `tests/host/runtime_macro_dynamic_lifecycle_policy_off_test.c`（多槽保留）
- `tests/host/runtime_macro_usb_hid_test.c`（多槽 disconnect + policy-off）
- `docs/DYNAMIC_MULTISLOT_PLAN.md`、`docs/DYNAMIC_MACRO_PLAN.md`

D4 已提交为 `32f0596` 并已推送；D2/D3 也已推送。

## 17. D5 实施记录（CLI 子范围）

### 17.1 范围与边界

本轮只实现 D5 的 Python CLI/客户端子范围。用户明确将**桌面端单独处理**，因此：

- 未修改桌面应用仓库；
- 未修改 [`DYNAMIC_DESKTOP_APP_SPEC.md`](DYNAMIC_DESKTOP_APP_SPEC.md)；桌面规范仍描述
  旧行为，必须在用户自行实现桌面端时按 `DYNAMIC_PROTOCOL.md` 的 v2 contract 同步；
- 未修改固件 wire、protocol 实现、opcode、DTS/binding 或配置仓库。

### 17.2 已实现内容（`tools/runtime_macro_cli.py`）

- capability v2：`DYNAMIC_CAPABILITY_VERSION = 2`，`CAPABILITIES` 请求固定携带 slot `0`，
  校验 `capability_version == 2`、`dynamic_object_count` 在 `1..8`、
  `max_dynamic_length == 512`，以及原有 TTL/timeout/lifecycle flags 规则；
- 拒绝 v1/畸形 capability：固件返回 `capability_version = 1` 或对 v2 capability slot 返回
  `BAD_SLOT` 时抛出 `DynamicV1Error`（`ProtocolError` 子类，不重试、不降级）；其他未知版本、
  越界对象数、错误最大长度均报 malformed capability，且不发送任何 dynamic 写入；
- 破坏式 slot API：`upload_dynamic(slot, data, ttl_seconds=None, *, keep_after_execute=False)`、
  `clear_dynamic(slot)`；`validate_dynamic_slot()` 在任何 HID write 前拒绝 `0..7` 之外的
  slot（含 `0xff`），capability 发现后再拒绝 `>= dynamic_object_count` 的 slot；
- `DYNAMIC_BEGIN`/`DYNAMIC_DATA`/`DYNAMIC_CLEAR` 均携带目标 slot；`0xff` dynamic sentinel 已
  完全移除（`LIST_SLOT` 仅用于 static `LIST` 和 auth 命令）；
- 每槽 1..512 bytes、22-byte chunking（512 bytes = 23×22 + 6）、默认/显式 TTL、
  `KEEP_AFTER_EXECUTE` flags 与原有重试规则（`BAD_REQUEST`/`BAD_OFFSET`/传输错误使用新的
  request ID 从 BEGIN 重启）保持一致；
- `clear_all_dynamic()`：按 capability 报告的 `dynamic_object_count` 对 `0..N-1` 逐槽发送
  `DYNAMIC_CLEAR`（wire 无 clear-all opcode），每槽独立重试并独立记录结果；任一槽位失败时
  仍尝试其余槽位，然后抛出 `DynamicClearAllError`（`.failed_slots` 列出失败槽位），绝不把部分
  失败报告为成功；
- 无 `get_dynamic()`/readback API，也没有任何通过 static `list`/`get` 推断 dynamic 内容的路径。

### 17.3 CLI 命令

- `capabilities`：输出 v2 字段（version、`dynamic_object_count`、flags、max length、TTL 边界、
  transaction timeout）；
- `dynamic-set --slot N`：`--slot` 必填，三种输入方式互斥，输出目标槽位、字节数、TTL 和执行后策略；
- `dynamic-clear --slot N` 与 `dynamic-clear --all`：互斥且必选其一；`--all` 输出已清槽位列表，
  部分失败时向 stderr 报告失败槽位并返回退出码 `1`；
- 本地参数/文本/slot 校验全部发生在任何 HID write 之前（slot 硬边界甚至在打开设备前）。

### 17.4 测试记录

`tests/python/test_runtime_macro_cli.py`（70 tests，全部通过）：

- capability v2：请求字节（slot `0` + 22-byte payload）、`object_count` 取 `1/3/8` 均接受；
  拒绝 v1、未知 version、`object_count` `0`/`9`、`max_length=511`、reserved lifecycle flags、
  `BAD_OPCODE` 和 `BAD_SLOT`（v1）；
- slot：`upload_dynamic`/`clear_dynamic` 对 `-1`/`8`/`12`/`0xff` 在零 HID write 下拒绝；
  slot `>= dynamic_object_count` 在 capability 之后、BEGIN/CLEAR 之前拒绝；BEGIN/DATA/CLEAR
  帧均带正确 slot；
- 分块：`1/22/23/256/511/512`，512 bytes 为 `[22]×23 + [6]`；同一上传 BEGIN/DATA 共用同一
  request ID；默认 TTL、显式 TTL、keep flags 与原有断言保持；
- clear-all：按 `0..2` 升序逐槽 CLEAR、每槽独立 request ID；slot 1 失败时仍尝试 slot 0/2/3
  并报 `failed_slots=(1,)`；逐槽超时重试使用新的 request ID；
- CLI：`--slot`/`--all` 必填与互斥、`dynamic-set` 输出目标槽位、`--all` 成功与部分失败退出码、
  单槽 clear、以及所有无效输入（TTL、非 ASCII、513 bytes、slot `8`/`-1`）零 HID write。

`py_compile` 在 host 与 `zmk-dev` 中通过，Ruff 在 host 中通过；`zmk-dev` 镜像未安装 Ruff，
因此容器内 Ruff 未运行。本轮未修改固件源码，host suite 与固件构建结果沿用 D4/D3 记录，
随后由主 agent 复核 dongle 构建。

### 17.5 变更文件

- `tools/runtime_macro_cli.py`
- `tests/python/test_runtime_macro_cli.py`
- `docs/CLI.md`
- `docs/DYNAMIC_MULTISLOT_PLAN.md`、`docs/DYNAMIC_MACRO_PLAN.md`

### 17.6 当前未完成 / 交接

- 桌面端（含 `DYNAMIC_DESKTOP_APP_SPEC.md` 同步、槽位 UI、逐槽清空与部分失败展示）由用户
  单独处理；本仓库不修改桌面仓库或桌面规范；
- 配置仓库已恢复单文件 `boards/shields/leen_totem/leen_totem.keymap`，保留最新 dongle
  内容：base slot `0`，Macro layer 左手第 3 排 slot `1..5`；本次只编译/验证 dongle；
- dongle 实机 CLI 已验证：capability `v2/8 slots/512 bytes`，slot `0..5` 上传、slot `7`
  的 512-byte 边界上传、slot `7` clear、`--all` 逐槽清除和 slot `8` 本地拒绝；slot `1`
  的 keep flag 上传也已被接受。测试文本已清空；这不等同于物理按键执行后的保留行为验证；
- 主 agent 已复跑 host/Python/py_compile/Ruff 和 `just totem-dongle`：均通过；当前 dongle
  RAM/Flash 已记录为 Flash `432468 B`、RAM `198654 / 262144 B`。D6 仍需按用户安排决定
  是否继续物理按键/TTL/lifecycle/static-auth 流程，并由用户单独完成桌面端。
