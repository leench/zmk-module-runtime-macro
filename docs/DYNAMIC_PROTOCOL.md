# Runtime Macro Dynamic Protocol v1

本文是 RAM-only dynamic macro 的最终 wire contract。它只定义已有
Runtime Macro v2 32-byte management frame 上的 capability、上传和 clear；不改变
静态 `LIST`/`GET`/`SET`/`CLEAR`、`AUTH_*`、`PASSWORD_SET` 或 `LOCK` 的语义。
固件、Python client 和桌面应用必须逐字节遵循本文。

文中的 **MUST**/“必须”是互操作要求。所有整数均为 unsigned little-endian。
动态协议没有动态文本 readback，也没有 protocol execute。

## 1. 范围和冻结值

第一版只有一个 dynamic object：

| 项目 | 固定值 |
| --- | --- |
| 动态对象数 | `1` |
| committed/staging 存储 | 仅 RAM；不得调用 Settings/NVS |
| 最大文本长度 | `256` bytes |
| 合法文本字节 | `0x20..0x7e`，以及 `0x0a` (LF)、`0x09` (Tab)、`0x08` (Backspace) |
| 空文本上传 | 不允许；使用 `DYNAMIC_CLEAR` |
| 默认 TTL | `300` seconds（5 分钟） |
| 显式 TTL | `1..86400` seconds，包含两端 |
| staging inactivity timeout | `30` seconds |
| 执行后策略 | 默认接受后消费；可选上传后保留 |
| TTL 起点 | 最后一个合法 `DYNAMIC_DATA` 完成 committed 替换之后 |
| 执行入口 | 只能是物理 `&runtime_macro_dynamic` behavior |
| 动态命令认证 | 不经过静态管理 authorization gate；这是第一版明确的安全边界 |

完整上传才替换 committed text。`BEGIN`、不完整上传、超时、非法 chunk、
重复/乱序 chunk 和失败上传都不得破坏旧 committed text。默认情况下成功执行被
executor 接受时消费 committed text；上传可选择执行后保留。executor 忙或启动
失败时无论策略如何都必须保留它。

## 2. Opcode、frame 和兼容性

### 2.1 Opcode

新 opcode 使用现有未占用的 `0x20..0x23`：

| 名称 | 值 | 作用 |
| --- | ---: | --- |
| `DYNAMIC_BEGIN` | `0x20` | 开始或重新开始一次动态上传 |
| `DYNAMIC_DATA` | `0x21` | 追加一个连续文本 chunk |
| `DYNAMIC_CLEAR` | `0x22` | 清除 committed、staging 和 TTL |
| `CAPABILITIES` | `0x23` | 只读；opcode handling 不修改 dynamic state（含 committed/TTL）、queued request 或 auth session 的 capability discovery（外围 auth observation 例外见 §3.1、§7） |

旧 v2 固件没有这些 opcode，会按现有实现返回 `BAD_OPCODE`。旧 v1 固件收到
`version=2` 会按现有实现返回 `BAD_VERSION`。因此 client 先发送 v2
`CAPABILITIES` request，可以区分：

- `BAD_VERSION`：不是当前 v2 管理协议（通常是 v1），不能发送动态命令；
- `BAD_OPCODE`：是 v2 frame 处理器，但没有动态 capability；
- 合法 `OK` capability response：支持本文定义的动态协议；
- transport timeout/error：不能推断固件版本或能力，按 transport retry 规则处理。

client 不得用 `DYNAMIC_CLEAR` 探测能力，也不得因为 `BAD_OPCODE` 自动降级到
静态 `SET` 或 v1。

### 2.2 32-byte v2 frame 对应关系

每个 request 和 response 必须严格为 32 bytes。frame 不含 CRC；transport 负责
report 边界。字段与现有 `PROTOCOL.md`、
`include/zmk/runtime_macro_protocol.h` 完全相同：

| Byte | Size | 字段 | 动态协议要求 |
| ---: | ---: | --- | --- |
| `0` | 1 | `version` | 固定为 `2`；错误 response 仍 echo 收到值 |
| `1` | 1 | `opcode` | 上表或现有静态/认证 opcode |
| `2` | 1 | `request_id` | response 原样 echo；同一上传的 BEGIN/DATA 必须相同 |
| `3` | 1 | `status` | request 必须为 `0` (`OK`)，response 为 status |
| `4` | 1 | `slot` | 动态和 capability 固定为 `0xff` (`LIST_SLOT`) |
| `5` | 1 | `payload_length` | `0..22` |
| `6..7` | 2 | `offset` | 文本 byte offset；BEGIN/CLEAR/CAPABILITY 有固定值 |
| `8..9` | 2 | `total_length` | 动态文本总长度，或 capability response 长度 |
| `10..31` | 22 | `payload` | 未使用尾部必须全为零 |

request 的 `status` 非零返回 `BAD_REQUEST`。`payload_length > 22` 返回
`BAD_LENGTH`，并且在读取尾部前不得越界。声明长度之后的 payload 尾部任何非零
字节返回 `BAD_REQUEST`。request 不能把 TTL、文本或任何其他数据放在声明长度
之外。

response 在生成时先全零填充，然后 echo request 的 `version`、`opcode`、
`request_id` 和 `slot`。所有动态错误 response 都必须满足：

```text
status          = 对应错误状态
payload_length  = 0
offset          = 0
total_length    = 0
payload[0..21]  = 0
```

所有动态成功 response（`CAPABILITIES` 除外）都没有 payload。它们只返回状态和
进度 metadata，绝不返回任何动态文本。`CAPABILITIES` 的 22-byte payload 只有
能力 metadata，也不包含动态文本。

USB HID 的 report descriptor 和 report 方向不变：firmware 接收/发送的逻辑
frame 是 32 bytes；现有 Python `hidapi` helper 写入时在 frame 前加一个
report-ID `0`，因此 host write 常为 33 bytes，读取时可为 32 或带前导零的
33 bytes。该前导零不是协议 frame 字段。

### 2.3 Status codes

动态协议复用现有 status 值，不增加动态专用 status：

| 名称 | 值 | 动态含义 |
| --- | ---: | --- |
| `OK` | `0` | 请求成功 |
| `BAD_VERSION` | `1` | `version` 不是 `2` |
| `BAD_OPCODE` | `2` | firmware 不认识 opcode |
| `BAD_REQUEST` | `3` | status、payload tail、事务 metadata 或固定字段非法；也用于没有 active transaction 的 DATA |
| `BAD_SLOT` | `4` | `slot` 不是 `0xff` |
| `BAD_OFFSET` | `5` | offset 超出对象，或不是下一个连续 offset |
| `BAD_LENGTH` | `6` | total/payload/TTL 长度或范围非法 |
| `INVALID_TEXT` | `7` | DATA payload 含不支持的 byte |
| `STORAGE_ERROR` | `8` | 动态 RAM 路径永不返回此值；它仍只属于静态 Settings 操作 |
| `INTERNAL` | `9` | 不可预期的 RAM/module failure；不得返回文本 |
| `AUTH_REQUIRED` | `10` | 动态分支永不返回；静态命令语义不变 |
| `AUTH_FAILED` | `11` | 动态分支永不返回；认证语义不变 |
| `AUTH_NOT_CONFIGURED` | `12` | 动态分支永不返回 |
| `RATE_LIMITED` | `13` | 动态分支永不返回 |
| `AUTH_NO_CHALLENGE` | `14` | 动态分支永不返回 |
| `CREDENTIAL_INVALID` | `15` | 动态分支永不返回；即使 auth state 为 `ERROR_LOCKED` 也允许合法动态请求 |

## 3. Capability discovery

### 3.1 Request

`CAPABILITIES (0x23)` request 必须是以下 canonical empty object：

```text
version=2
opcode=0x23
status=0
slot=0xff
offset=0                  # frame byte 6..7
total_length=0
payload_length=0
payload[0..21]=0
```

`offset` 对应 frame byte `6..7`。在 common frame validation 通过后，固定 object
字段不匹配返回 `BAD_SLOT`（slot 错误）或 `BAD_REQUEST`（其余错误）。这里的
“无副作用”仅指 `CAPABILITIES` opcode handling：它不改变 dynamic state、staging、
committed、TTL、queued request 或 auth session，也不主动调用/刷新 auth session。
如果现有 protocol process 在本次 request 前的 auth observation 实际触发 session
lazy expiry，则按第 7 节 session-expiry lifecycle 只清 incomplete dynamic staging
（不清 committed）；这是外围 auth lifecycle side effect，不是 `CAPABILITIES`
opcode handling 的副作用。

### 3.2 Response payload

支持动态协议的 firmware 返回 `status=OK`、`offset=0`、`total_length=22`、
`payload_length=22`。payload 的布局固定如下：

| Payload byte | Size | 字段 | 固定/含义 |
| ---: | ---: | --- | --- |
| `0` | 1 | `capability_version` | 固定 `1` |
| `1` | 1 | `dynamic_object_count` | 固定 `1` |
| `2..3` | 2 | `lifecycle_flags` | little-endian，见下表 |
| `4..5` | 2 | `max_dynamic_length` | 固定 `256` |
| `6..9` | 4 | `default_ttl_seconds` | 固定 `300` |
| `10..13` | 4 | `min_ttl_seconds` | 固定 `1` |
| `14..17` | 4 | `max_ttl_seconds` | 固定 `86400` |
| `18..21` | 4 | `transaction_timeout_seconds` | 固定 `30` |

`lifecycle_flags`：

| Bit | 名称 | 第一版默认/含义 |
| ---: | --- | --- |
| `0` | `CLEAR_ON_BOOT` | `1`；RAM 初始化清除 |
| `1` | `CLEAR_ON_TTL_EXPIRY` | `1`；TTL 到期清除 |
| `2` | `CLEAR_ON_EXECUTION_ACCEPT` | `1`；默认 behavior 被 executor 接受时消费；单次上传可覆盖为保留 |
| `3` | `CLEAR_ON_USB_DISCONNECT` | `1`，实际管理 USB disconnect 默认清除 |
| `4` | `CLEAR_ON_BLE_PROFILE_CHANGE` | `0`；默认保留，可配置为 `1` |
| `5` | `CLEAR_ON_SELECTED_ENDPOINT_CHANGE` | `0`；默认保留，可配置为 `1` |
| `6` | `SUPPORTS_KEEP_AFTER_EXECUTE` | `1`；支持 BEGIN 的保留选项 |
| `7..15` | 保留 | 必须为 `0` |

响应中的所有字段和 reserved bits 必须符合上述值；client 遇到未知
`capability_version`、错误 `dynamic_object_count`、非零 reserved bits 或
超出约定范围的数值，必须报告 malformed capability，不得发送动态写入。只有
`SUPPORTS_KEEP_AFTER_EXECUTE` 为 `1` 时，client 才可以发送保留选项；不支持时
普通默认消费上传仍可用。capability response 的 payload 不是动态文本。

## 4. Dynamic state、TTL 和执行边界

### 4.1 两个 RAM 状态

实现维护一个动态对象的两个 bounded buffer：

1. `committed`：当前可由 `&runtime_macro_dynamic` 执行的文本，长度 `1..256`
   或 empty，并带有执行后消费/保留策略；
2. `staging`：当前一次上传的临时文本，长度由 `total_length` 声明，最多
   `256`，并带有待提交的执行策略。

另外保存 `committed_length`、`staging_received`、TTL deadline、transaction
request ID、transaction total 和 staging deadline。staging 与 committed 必须
由同一串行状态保护；任何时刻只有一个 active dynamic upload transaction。

动态路径不得调用以下任何 static/storage API：

```c
zmk_runtime_macro_slot_set()
zmk_runtime_macro_slot_clear()
settings_save_one()
settings_delete()
```

动态状态不进入 static `LIST`，static `GET` 不可读取它，static `SET`/`CLEAR`
不影响它，dynamic `DYNAMIC_CLEAR` 也不影响 static slots 或 credentials。

dynamic transaction 与当前 protocol context 中 static `SET` 和 `PASSWORD_SET`
transaction 是两套相互隔离的状态，即使实现把它们放在同一个 context struct
中也必须保持独立字段和独立清除函数。合法或非法的 `CAPABILITIES`、
`DYNAMIC_BEGIN`、`DYNAMIC_DATA`、`DYNAMIC_CLEAR` 的 opcode handling 都不得清除、
覆盖、推进或 zeroize static `SET/PASSWORD_SET` staging；dynamic branch 不得调用
完整的 `zmk_runtime_macro_protocol_discard()`。只有本文第 7 节的 auth transition、
第 8 节的 USB transport discard 以及其他明确列出的独立 lifecycle 动作，才可按
各自规则处理 static 和 dynamic staging；若一次 request 的外围处理实际观察到
这样的 lifecycle transition，按该独立规则处理，不把它算作 dynamic branch 的
清除副作用。

### 4.2 Commit 和 TTL

- 合法 `DYNAMIC_BEGIN` 只清除旧 dynamic staging，然后建立新的 active
  transaction；旧 committed text 和其 TTL 保持不变，static `SET/PASSWORD_SET`
  staging 不变。
- 合法非最终 `DYNAMIC_DATA` 只追加到 staging；旧 committed text 保持不变。
- 最终 DATA 使 `staging_received == total_length` 时，在一个不可观察到半成品
  的临界区中把完整 staging 原子替换为 committed，启动新的 TTL，并清空 staging
  transaction。旧 committed 只在新值完整复制后才 zeroize。
- 新 commit 使用 BEGIN 中的 TTL 和执行后策略；TTL deadline 从 commit 完成时刻
  开始，不从 BEGIN 或第一 chunk 开始。
- TTL 到期是一次动态状态清除：zeroize committed、staging，取消 transaction
  和 TTL。它不发送 response；下一个 behavior press 不产生输出。
- staging inactivity timeout 只清除 staging transaction，保留旧 committed
  text；它不改变 TTL。
- 显式 `DYNAMIC_CLEAR` 只清除 dynamic committed、dynamic staging 和 TTL，且
  重复执行仍返回 `OK`；它不清除 static `SET/PASSWORD_SET` staging，也不得调用
  完整的 protocol discard。
- reboot/reset 的 volatile initialization 使 committed、staging 和 TTL 全部为空。
- clear、替换和到期必须 zeroize store 中相应 RAM；executor snapshot 无论动态
  策略如何都必须在完成/失败/取消后 zeroize。日志不得包含动态文本或 chunk。

### 4.3 Physical behavior

协议没有 execute opcode。物理 `&runtime_macro_dynamic` press 使用与 static
macro 相同的单一 executor：

- empty 或已到期：无输出并保持 harmless；
- executor 忙：返回 `-EBUSY`，不消费 committed；
- executor 无法 schedule/start：返回错误，不消费 committed；
- executor 接受 snapshot：默认立即从 dynamic store 消费并清除 committed/TTL；若
  本次上传选择保留，则 committed 和 TTL 继续存在。无论哪种策略，executor 都用
  独立 snapshot 继续完成 keycode 事件；后续 `DYNAMIC_CLEAR` 或 lifecycle clear
  不会中断或回读这个 executor-owned snapshot，snapshot 仍在 executor 完成、失败
  或取消时 zeroize；
- static 和 dynamic 不排队、不并发，不能创建第二个 worker；
- executor 结束、event failure 或取消后 zeroize snapshot。

## 5. Command wire contract

### 5.1 `DYNAMIC_BEGIN (0x20)`

Request 固定字段：

| 字段 | 要求 |
| --- | --- |
| `version` | `2` |
| `opcode` | `0x20` |
| `status` | `0` |
| `slot` | `0xff` |
| `offset` | `0` |
| `total_length` | `1..256`；只表示文本长度，不包含 TTL |
| `payload_length` | 只能是 `0`、`1`、`4` 或 `5` |
| payload 长度 `0` | 使用默认 TTL `300`，执行后消费 |
| payload 长度 `1` | `payload[0]` 是 BEGIN flags；使用默认 TTL |
| payload 长度 `4` | `payload[0..3]` 是 TTL seconds 的 uint32 LE，范围 `1..86400` |
| payload 长度 `5` | `payload[0..3]` 是 TTL；`payload[4]` 是 BEGIN flags |
| payload 尾部 | payload 长度之后必须全零 |

BEGIN flags 当前只有 bit `0`：`KEEP_AFTER_EXECUTE`。bit `0=0` 表示执行
被 executor 接受后消费；bit `0=1` 表示执行后保留。其他 flags bit 返回
`BAD_REQUEST`。显式 TTL `0`、大于 `86400` 或 payload 长度不是 `0/1/4/5`
返回 `BAD_LENGTH`。`offset` 非零返回 `BAD_OFFSET`。slot 错误返回 `BAD_SLOT`。
这些 BEGIN semantic errors 都 zeroize/cancel 当前 staging，但保留旧 committed
text。

合法 BEGIN 的处理是无条件开始一个新 transaction，即使已有 staging，也即使
request ID 与旧 transaction 相同；它不会清 committed。服务器记录
`request_id`、`total_length`、TTL、执行后策略和 `received=0`，并启动/重置
30-second inactivity timeout。

成功 response：

```text
status=OK
payload_length=0
offset=0
total_length=BEGIN.total_length
payload 全零
```

### 5.2 `DYNAMIC_DATA (0x21)`

每个 DATA 必须满足：

| 字段 | 要求 |
| --- | --- |
| `version` | `2` |
| `opcode` | `0x21` |
| `status` | `0` |
| `slot` | `0xff` |
| `request_id` | 必须等于 active BEGIN 的 request ID |
| `total_length` | 必须等于 active BEGIN 的 total，且为 `1..256` |
| `offset` | 必须等于 active transaction 当前 `received` |
| `payload_length` | `1..22`，且不超过 `total_length - offset` |
| payload | 每个 byte 必须是允许的 ASCII/control byte |
| payload 尾部 | payload 长度之后必须全零 |

DATA 的确切 server validation 顺序如下；失败时按表中 status 处理，并
zeroize/cancel staging，旧 committed 不变：

1. common frame validation（version、known opcode、request status、payload
   长度、payload tail）；
2. `slot` 检查，错误为 `BAD_SLOT`；
3. `total_length` 检查，`0` 或 `>256` 为 `BAD_LENGTH`；
4. `offset > total_length` 为 `BAD_OFFSET`；
5. `payload_length == 0` 或大于 `total_length - offset` 为 `BAD_LENGTH`；
6. 没有 active transaction、request ID 不同或 total 不同为 `BAD_REQUEST`；
7. `offset != received` 为 `BAD_OFFSET`；这包括重复 chunk 和向前跳过 chunk；
8. 逐 byte 验证 payload；遇到不支持 byte 为 `INVALID_TEXT`；
9. 通过全部检查后才 memcpy 到 staging 并推进 `received`。

`DYNAMIC_DATA` 不允许隐式 BEGIN。没有 active transaction 的 offset `0` DATA
也返回 `BAD_REQUEST`。因此 client 必须先成功完成 BEGIN。

非最终 DATA 的成功 response：

```text
status=OK
payload_length=0
offset=offset + payload_length   # 下一个期望 offset
total_length=BEGIN.total_length
payload 全零
```

最终 DATA 的成功 response 使用相同格式，其中 `offset == total_length`，并且
此时 committed 已经原子替换、TTL 已启动。任何 DATA response 都不包含已写入
的文本，也不返回 staging 内容。

### 5.3 `DYNAMIC_CLEAR (0x22)`

Request 必须是：

```text
version=2
opcode=0x22
status=0
slot=0xff
offset=0
total_length=0
payload_length=0
payload[0..21]=0
```

slot 错误返回 `BAD_SLOT`；其余 empty-object 错误返回 `BAD_REQUEST`，错误
request 不产生任何 dynamic 或 static staging 变化。合法 CLEAR 原子清除 dynamic
committed、dynamic staging、TTL 和 transaction；它不改变 static
`SET/PASSWORD_SET` staging、static slots、Settings、credentials、auth session 或
static executor 状态，也不调用完整的 protocol discard，也不打断已经被 executor
接受的 dynamic snapshot。无论之前是否为空，成功 response 都是：

```text
status=OK
payload_length=0
offset=0
total_length=0
payload 全零
```

这使 CLEAR 幂等，且不会通过 response 区分“原来有文本”和“原来为空”。

## 6. 单事务状态机和恢复

### 6.1 状态转换

```text
IDLE (无 staging)
  -- valid BEGIN --> ACTIVE(received=0, request_id, total, ttl)
ACTIVE
  -- valid non-final DATA --> ACTIVE(received += payload_length)
ACTIVE
  -- valid final DATA --> IDLE + atomic committed replace + new TTL
ACTIVE
  -- valid new BEGIN --> ACTIVE(new request_id, received=0, new total, new ttl)
ACTIVE/IDLE
  -- valid CLEAR --> IDLE + committed clear + TTL clear
ACTIVE
  -- timeout/dynamic-branch invalid DATA or BEGIN/transport discard/auth transition
     --> IDLE, dynamic committed 保持
```

状态机中的每个 transition 只改变 dynamic state。除明确标记为 auth transition
或 USB transport discard 的 lifecycle 动作外，dynamic transition 不得清除或
修改同一 protocol context 的 static `SET/PASSWORD_SET` staging。

### 6.2 request ID、duplicate 和 out-of-order

- request ID 是 8-bit opaque correlation value，不提供认证、去重或 replay
  protection；同一串行 transport 不允许并行 request。
- 一个 transaction 的 BEGIN 和全部 DATA 使用同一个 request ID。响应 echo
  request 的 version/opcode/request ID/slot。
- 每次合法 BEGIN 都是明确的 restart，允许替换同 ID 的旧 staging；恢复时
  client 必须改用新的 request ID，避免旧 ACK 与新事务混淆。
- DATA 的 offset 小于 `received` 是 duplicate，返回 `BAD_OFFSET` 并丢弃整个
  staging；offset 大于 `received` 是 out-of-order，同样返回 `BAD_OFFSET` 并
  丢弃 staging。
- DATA 的 request ID 或 total 与 active BEGIN 不同返回 `BAD_REQUEST` 并丢弃
  staging；不会尝试把 chunk 拼到别的 transaction。
- final DATA 已经 commit 而 final ACK 丢失时，服务器没有 active staging。把
  相同 final DATA 重发会得到 `BAD_REQUEST`，不会再次读取或返回文本，也不会
  清除已 committed text。
- 任一 transaction failure 都只影响 staging；旧 committed text 仍可被物理
  behavior 执行。

### 6.3 Timeout 和丢 ACK

服务器 transaction timeout 固定为 30 秒：BEGIN 和每个成功的非最终 DATA
都从处理完成时刷新 deadline。到期无 response；staging 被 zeroize。此 timeout
与 committed TTL 完全独立。

client 规则：

1. 每个 upload 先完成 capability check、本地长度/ASCII/TTL 校验，再发送
   BEGIN 和 DATA；最大 22 bytes/chunk，offset 连续从 `0` 开始。
2. BEGIN、任一 DATA 发生 HID write/read timeout、transport error、连接重置或
   ACK 丢失时，不重发原 frame 作为恢复手段；用新的 request ID 从 BEGIN
   完整重传。新的 BEGIN 会替换可能残留的 staging。
3. 收到 DATA 的 `BAD_REQUEST` 或 `BAD_OFFSET` 时，用新的 request ID 从 BEGIN
   完整重传；这覆盖 ACK 丢失后重发 final DATA 的情况。
4. `BAD_LENGTH`、`BAD_SLOT`、`INVALID_TEXT` 是本地 bug 或输入错误，client
   不得自动重试同一 upload；应先修正输入/frame。`INTERNAL` 应报告失败，
   不把未确认的 commit 当作成功；用户/上层显式重试时必须从新 BEGIN 开始。
5. CLEAR 是幂等的：其 timeout/transport error 可以用新的 request ID 重发同一
   empty frame；不得把 CLEAR 降级为上传空文本。
6. `CAPABILITIES` 的 opcode handling 不改变 dynamic state、committed、TTL 或
   queued request，不主动调用/刷新 auth session；若 request 前的外围 auth
   observation 实际触发 session lazy expiry，按第 7 节只清 incomplete dynamic
   staging、不清 committed，这不是 CAPABILITIES opcode 的副作用。CAPABILITIES
   timeout/transport error 可以用新的 request ID 重试；它的合法 response 仍必须
   严格校验。
7. 收到与当前 request 的 version、opcode、request ID 或 slot 不匹配的旧
   response 时丢弃；旧 response 不延长当前 monotonic deadline。字段匹配但
   payload/offset/total malformed 的 response 必须立即报告 protocol error，
   不静默重试。
8. 动态 client 不因为设备处于 `PROTECTED` 或 `ERROR_LOCKED` 而先自动登录；
   动态操作不改变 client 的静态 auth state machine。

## 7. Authentication boundary

动态 branch 必须在 static management authorization gate 之外。common frame
validation 仍先执行，但动态请求不得调用
`zmk_runtime_macro_auth_refresh_session()`，不得因为成功或失败刷新静态
session deadline。现有 protocol process 会在处理 request 前同步观察 auth state；
`zmk_runtime_macro_auth_get_state()`、`get_info()` 和 `is_authenticated()` 可能
在观察时触发 session lazy expiry。若本次 request 的 auth observation 确实把
session 从 active 变为 expired，则按 session-expiry lifecycle 处理：只丢弃
incomplete dynamic staging（并按现有认证契约丢弃 static `SET/PASSWORD_SET`
staging），不清 dynamic committed text。`CAPABILITIES` 本身不主动调用 auth
refresh，也不延长 session。

| Auth state | `CAPABILITIES` | `DYNAMIC_BEGIN/DATA/CLEAR` |
| --- | --- | --- |
| `OPEN` | 按格式处理 | 按格式处理 |
| `PROTECTED`，session 无效 | 按格式处理 | 按格式处理，不返回 `AUTH_REQUIRED` |
| `PROTECTED`，session 有效 | 按格式处理 | 按格式处理，仍不刷新 session |
| `ERROR_LOCKED` | 按格式处理 | 按格式处理，不返回 `CREDENTIAL_INVALID` |

**安全警告：dynamic management HID 通道未加密且不要求 static password
认证。它不得用于传输、保存、注入或依赖任何秘密，包括 OTP/验证码、密码、密钥、
token 或其他凭据。** 产品计划中即使列出 OTP/verification-code use case，也
不能把它解释为本协议的安全承诺；调用方必须把 dynamic text 当作可能被本机
程序、USB 抓包者和目标 host 观察或修改的非秘密数据。OTP 等秘密场景必须使用
另行设计的加密且认证通道，不能依赖本文 dynamic upload 的完整性或保密性。

这是一项有意的第一版安全边界：拥有 management HID 访问权限的本机程序可以
尝试写入或清除 dynamic buffer。firmware 不能识别来源进程，dynamic 通道也不
提供进程身份认证。

认证变化与 dynamic committed 是两个边界：

- `LOCK`、成功 `AUTH_PROVE`、成功 `PASSWORD_SET`、实际发生的 session lazy
  expiry、`zmk_runtime_macro_auth_transport_reset()` 和其他 auth state
  transition 都会丢弃未完成 dynamic staging/transaction，但不清 committed
  text；其中 static `SET/PASSWORD_SET` staging 按现有认证契约独立丢弃。
- `AUTH_INFO`、`AUTH_CHALLENGE` 和 `CAPABILITIES` 如果本次 auth observation
  没有实际触发 lazy expiry，则不改变 dynamic state；它们都不会因为查询或
  challenge 生成而刷新 session。若其中一次观察实际触发 expiry，则只按上一段
  的 session-expiry 规则丢 dynamic incomplete staging，不清 committed。
- static 命令的现有 OPEN/PROTECTED/ERROR_LOCKED gate、session refresh、
  `SET/PASSWORD_SET` staging 和 error status 完全不变；dynamic request 不会
  代替这些 static lifecycle 规则。
- `LOCK` 不等价于 `DYNAMIC_CLEAR`，不会清动态 committed text；需要清动态内容
  必须显式发送 `DYNAMIC_CLEAR` 或等待已启用且条件满足的 lifecycle clear。

## 8. USB、Bluetooth 和 endpoint lifecycle

### 8.1 三种清除概念必须分开

1. **USB generation/queued-request discard**：每个 USB connection-state
   notification 都把 transport 置 offline、递增 generation、清 auth transient
   state、调用 protocol discard（包括 dynamic staging）并 purge queued
   request。采样旧 generation 的 callback 即使晚到 queue，也由 worker 丢弃。
2. **protocol/lifecycle discard**：只 zeroize 未完成 transaction/staging，不
   触碰 committed text。它可以由 timeout、auth transition、USB notification
   或非法 transaction 触发。
3. **committed dynamic clear**：zeroize committed、staging 和 TTL。它只在
   boot、TTL、executor accept、合法 `DYNAMIC_CLEAR` 和已启用的 lifecycle policy
   发生；它不是每次 auth reset、queue purge 或 USB generation 变化的自动结果。

USB generation discard 和 committed clear 必须由独立代码路径表达。一个 stale
queued DATA 不能在新 generation commit；但 RESET/SUSPEND 等没有 committed-clear
policy 的事件也不能误清已经 committed 的文本。`protocol_discard` 作为 USB/auth
lifecycle 操作时可以同时清 static `SET/PASSWORD_SET` 和 dynamic incomplete
staging；它不得被 `CAPABILITIES`、`DYNAMIC_BEGIN`、`DYNAMIC_DATA` 或
`DYNAMIC_CLEAR` 的正常/错误处理路径复用。合法 `DYNAMIC_CLEAR` 只调用 dynamic
clear，不能借 USB queue purge 或 generic protocol discard 清除 static staging。

### 8.2 USB raw status 规则

现有 USB listener 的 raw-status/event mapping 和 IN endpoint ownership 规则
固定如下。每个通知都先执行 8.1 的 generation/queue/protocol discard。

| raw USB status | 期待的 `zmk_usb` event state | dynamic committed | protocol/auth/queue | IN permit |
| --- | --- | --- | --- | --- |
| `USB_DC_DISCONNECTED` | `ZMK_USB_CONN_NONE` | 若 `CLEAR_ON_USB_DISCONNECT=1` 则清；默认是 `1` | discard/purge/generation | recycle |
| `USB_DC_RESET` | `ZMK_USB_CONN_POWERED` | 保留 | discard/purge/generation | recycle |
| `USB_DC_CONFIGURED` | `ZMK_USB_CONN_HID` | 保留 | discard/purge/generation | recycle |
| `USB_DC_SUSPEND` | `ZMK_USB_CONN_HID` | 保留 | discard/purge/generation | 保留到正常 `DATA_IN` |
| `USB_DC_RESUME` | `ZMK_USB_CONN_HID` | 保留 | discard/purge/generation | 保留到正常 `DATA_IN` |
| `USB_DC_UNKNOWN` | `ZMK_USB_CONN_NONE` | 保留 | discard/purge/generation | 保留到正常 `DATA_IN` |
| `USB_DC_ERROR` | `ZMK_USB_CONN_POWERED` | 保留 | discard/purge/generation | 保留到正常 `DATA_IN` |

`USB_DC_CLEAR_HALT`、`USB_DC_SOF` 也映射为 HID，清除 staging/queue/auth 但不
清 committed，且不提前 recycle IN permit。`USB_DC_CONNECTED`、`INTERFACE`、
`SET_HALT` 等映射为 POWERED，同样不清 committed，也不提前 recycle permit。

“实际 management USB disconnect”只有在 listener 处理前后的 raw status 都是
`USB_DC_DISCONNECTED`，且 event state 与上述 mapping 相符时才成立。状态在
异步 listener 期间变化时，transport 必须保持 offline、丢弃 queue/staging，
但不得执行 committed clear 或提前回收 endpoint-owned response buffer。
只有 RESET、DISCONNECTED、CONFIGURED 明确证明旧 endpoint ownership 结束，
才可以 recycle IN permit；这与 dynamic committed clear 是不同判断。

USB `SUSPEND` 不是可靠 disconnect。host OS shutdown、host 关闭数据功能但继续
提供 VBUS、controller 的 reset/coalescing 行为可能只产生 suspend、reset、
configured 或没有可观察变化；firmware 不承诺把它们解释为 disconnect。即使
硬件最终确实拔出，若没有稳定的 `USB_DC_DISCONNECTED` notification，也不承诺
触发 disconnect clear。

### 8.3 BLE profile 和 selected endpoint

默认 policy 是 preserve，保证 USB host A 保持 management HID 时，可以切换
Bluetooth host B 并让 dynamic text 继续可用：

- `zmk_ble_active_profile_changed`：默认不清；若
  `CLEAR_ON_BLE_PROFILE_CHANGE=1`，在该独立 event 被处理时清 committed、
  staging 和 TTL；它不等价于 USB disconnect，不清 auth session、static
  `SET/PASSWORD_SET` staging 或 USB generation。
- `zmk_endpoint_changed`：默认不清；若
  `CLEAR_ON_SELECTED_ENDPOINT_CHANGE=1`，只有 selected endpoint 实际变化时
  清 committed、staging 和 TTL。USB、BLE、fallback 引起的 selected endpoint
  change 都属于此事件；该 optional clear 不清 auth session、static
  `SET/PASSWORD_SET` staging 或 USB generation。
- `OUT_USB`/`OUT_BLE` preferred-output 值变化但没有独立 endpoint event 时，
  不清除；本文不把 preferred output change 描述为可可靠观察的 lifecycle
  boundary。

可选 lifecycle clear event 会按动态 mutex 顺序清除当时的 state；它不是
transport generation reset。event 之后到达的新管理 request 可以正常建立新的
BEGIN，这表示一次新的写入而不是绕过 clear。若实现同时能在同一 transport
mutex 下丢弃尚未处理的 dynamic queue，则该 queue discard 只属于该 event 的
lifecycle integration，不能改变 USB generation 的定义。

第一版 policy defaults 与 capability bits 一致：实际 management USB
`DISCONNECTED` clear 开启；BLE profile change 和 selected endpoint change 关闭。

## 9. Server validation 和 client 独立实现检查表

### 9.1 Server 的全局顺序

对任何收到的完整 frame，server 必须按以下顺序生成 response：

1. zero-fill response，并先写入 request 的 version/opcode/request ID/slot；
2. 检查 `version==2`，否则 `BAD_VERSION`；
3. 检查 opcode 是否已知，否则 `BAD_OPCODE`；
4. 检查 request `status==0`，否则 `BAD_REQUEST`；
5. 检查 `payload_length<=22`，否则 `BAD_LENGTH`；
6. 检查 declared payload 后的所有尾部 byte 是否为零，否则 `BAD_REQUEST`；
7. 进入 capability 或 dynamic-specific validation；
8. 只有 static `LIST/GET/SET/CLEAR/PASSWORD_SET` 才进入现有 static auth gate。

动态和 static staging 的清除规则必须按 opcode 分开：

- `version` 错误和未知 opcode 在进入 opcode-specific branch 之前返回；它们不
  改变任何 dynamic state，也不改变 static `SET/PASSWORD_SET` staging。
- 一旦 `version==2` 且 opcode 已识别为 `DYNAMIC_BEGIN` 或 `DYNAMIC_DATA`，该
  branch 中发现的任何 request/事务/文本错误都必须取消当前 dynamic staging、
  保留 dynamic committed text，然后返回对应的 `BAD_REQUEST`、`BAD_SLOT`、
  `BAD_OFFSET`、`BAD_LENGTH` 或 `INVALID_TEXT`。这包括 common status/length/
  tail validation failure，以及 BEGIN 的 slot/offset/total/TTL 错误和 DATA 的
  request ID/total/offset/payload/text 错误。它们绝不能清除、覆盖或 zeroize
  static `SET/PASSWORD_SET` staging。
- `CAPABILITIES` 的合法或非法 opcode handling 都不改变 dynamic state、committed、
  TTL、queued request 或 static staging，也不主动调用/刷新 auth session；若外围
  auth observation 实际触发 lazy expiry，按第 7 节只清 incomplete dynamic staging，
  不清 committed。这是独立 auth lifecycle side effect，不是 CAPABILITIES 的
  opcode 副作用。`DYNAMIC_CLEAR` 的 malformed request（`BAD_SLOT` 或 `BAD_REQUEST`）
  同样不改变任何 state。
- 只有合法 `DYNAMIC_CLEAR` 才清 dynamic committed/dynamic staging/TTL；它仍然
  不清 static `SET/PASSWORD_SET` staging。USB transport discard 和 auth
  transition 是独立 lifecycle 路径，按第 7、8 节规则处理两类 staging。

上述 per-opcode “不改变”只描述 opcode handling 本身；若同一次 request 的外围
处理已经实际观察到 auth lazy expiry 或 USB lifecycle notification，该独立事件
仍按第 7、8 节规则 discard 相应 staging，不能被 malformed/valid opcode 规则
屏蔽。所有错误 response 都使用 2.2 的零 offset/total/payload 形式。

### 9.2 Client 的严格 response 检查

client 必须先按现有 `tools/runtime_macro_cli.py` 的
`HidTransport.exchange()` 规则过滤 stale response，再验证：

- response version、opcode、request ID、slot 与当前 request 一致；
- status 是已知值；
- error response 的 payload_length、offset、total 和 payload 全为零；
- BEGIN response payload_length 为零、offset 为零、total 等于请求 total；
- DATA response payload_length 为零、total 等于 transaction total、offset
  等于发送 chunk 末尾；
- CLEAR response 三个长度/范围字段均为零；
- CAPABILITIES response 恰为 22-byte metadata，并严格检查所有字段。

匹配 response 但 metadata 不正确时立即停止并报告 protocol error；不能因为
response 看起来像 timeout 而重发同一 DATA。每次重试都必须使用新的 request
ID；动态 upload 的重试必须从 BEGIN 重新开始。

## 10. 完整 frame examples

以下每行都是 firmware 看到的 32-byte frame，按 byte `0..31` 书写。hex 中的
`00` 包括所有未使用 payload 尾部。

### 10.1 Default TTL、两个 DATA chunk

文本为 23 bytes：`0123456789ABCDEFGHIJKL?`，第一 chunk 为 22 bytes，第二
chunk 为 1 byte。BEGIN 不携带 TTL，所以使用默认 300 seconds。

```text
# DYNAMIC_BEGIN request, request_id=0x10, total_length=23
02 20 10 00 ff 00 00 00 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_BEGIN response: OK, next offset=0, total=23
02 20 10 00 ff 00 00 00 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_DATA request, offset=0, payload_length=22
02 21 10 00 ff 16 00 00 17 00 30 31 32 33 34 35 36 37 38 39 41 42 43 44 45 46 47 48 49 4a 4b 4c

# DYNAMIC_DATA response: OK, next offset=22, total=23
02 21 10 00 ff 00 16 00 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_DATA request, offset=22, final payload "?" (0x3f)
02 21 10 00 ff 01 16 00 17 00 3f 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_DATA response: final OK, next offset=23, total=23
02 21 10 00 ff 00 17 00 17 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### 10.2 Explicit TTL

下面上传 1-byte text `Z`，TTL 为 600 seconds (`0x00000258`，LE
bytes `58 02 00 00`)。TTL bytes 只出现在 BEGIN payload，不进入 text total。

```text
# DYNAMIC_BEGIN request, request_id=0x20, total=1, explicit TTL=600
02 20 20 00 ff 04 00 00 01 00 58 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_BEGIN response
02 20 20 00 ff 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# Corresponding final DATA request and response
02 21 20 00 ff 01 00 00 01 00 5a 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02 21 20 00 ff 00 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### 10.3 Keep after execute

下面上传 1-byte text `K`，使用默认 TTL 并设置 `KEEP_AFTER_EXECUTE`。BEGIN
payload 只有一个 flags byte `01`；成功执行后 committed text 和 TTL 仍然存在，
直到 TTL、CLEAR、lifecycle clear 或新的上传清除/替换它。

```text
# DYNAMIC_BEGIN request, request_id=0x25, total=1, default TTL, keep=1
02 20 25 00 ff 01 00 00 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# DYNAMIC_DATA request and final response
02 21 25 00 ff 01 00 00 01 00 4b 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
02 21 25 00 ff 00 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### 10.4 `DYNAMIC_CLEAR`

```text
# request_id=0x30
02 22 30 00 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# idempotent OK response
02 22 30 00 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### 10.5 `CAPABILITIES`

```text
# request_id=0x01
02 23 01 00 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

# response: version=1, object_count=1, flags=0x004f,
# max=256, default=300, min=1, max_ttl=86400, transaction_timeout=30
02 23 01 00 ff 16 00 00 16 00 01 01 4f 00 00 01 2c 01 00 00 01 00 00 00 80 51 01 00 1e 00 00 00
```

旧 v2 firmware 对同一 request 的可识别 response 是：

```text
02 23 02 02 ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

其中 status `0x02` 为 `BAD_OPCODE`；旧 v1 firmware 则 echo version `0x02`
并返回 status `0x01` (`BAD_VERSION`)。两种错误 response 都不改变设备状态。

## 11. 与现有实现的对应依据

本文选择不是新 transport：

- `include/zmk/runtime_macro_protocol.h` 已固定 frame size `32`、header size
  `10`、payload size `22`、version `2`，字段 offset 为 `0/1/2/3/4/5/6/8/10`，
  并以 `0xff` 定义 `LIST_SLOT`；dynamic 使用同一 sentinel，避免增加 frame
  字段。
- `src/runtime_macro_protocol.c` 的 `runtime_macro_protocol_init_response()`、
  `set_error()`、`set_success()`、little-endian helpers 和 payload-tail check
  决定了本文的 echo、错误零填充、offset/total ACK 及 canonical frame 规则。
- 现有 static `SET` 已以 22-byte chunk、request ID/slot/total、连续 offset、
  duplicate/out-of-order discard 和最终 atomic slot update 工作；本文保留这些
  wire 习惯，但 dynamic 额外要求先 BEGIN，且不调用 static slot API。
- `tools/runtime_macro_cli.py` 的 `build_frame()`、`HidTransport.exchange()`、
  `validate_response()` 已规定 zero-filled frame、report ID 0、stale response
  过滤、absolute timeout 和错误 response 校验；dynamic client 只新增本文
  opcode/field checks。
- `src/runtime_macro_usb_hid.c` 已固定 vendor HID report 为 32 bytes，并以
  generation-tagged queue、transport mutex、protocol discard 和 endpoint-owned
  IN permit 处理 lifecycle；本文把 committed clear 从这些 discard 操作中明确
  分离。
- `src/runtime_macro_ascii.c`/`include/zmk/runtime_macro.h` 的 ASCII mapping 是
  dynamic DATA 的唯一文本合法性来源；不引入 Unicode、中文或 raw HID output。
- `src/runtime_macro_executor.c` 当前是单一 delayable-work executor，busy 返回
  `-EBUSY` 且不排队；dynamic 必须接入同一 executor，不改变 static behavior。

因此该协议不需要修改现有静态 opcode、status、认证 frame 或 ZMK 主仓库；实现
动态 feature 时只需在 v2 dispatch 中增加本文定义的 capability/dynamic branch，
并为 dynamic state 增加独立 RAM/API 边界。
