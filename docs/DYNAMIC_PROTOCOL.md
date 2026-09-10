# Runtime Macro Dynamic Protocol v2

本文是 RAM-only dynamic macro 多槽位协议的冻结 wire contract。它定义现有
Runtime Macro v2 32-byte management frame 上的 capability、上传和逐槽 clear；
不改变静态 `LIST`/`GET`/`SET`/`CLEAR`、`AUTH_*`、`PASSWORD_SET` 或 `LOCK` 的
语义。固件、Python client、CLI 和桌面应用必须逐字节遵循本文。

本文的 **MUST**/“必须”是互操作要求。所有整数均为 unsigned little-endian。
动态协议没有动态文本 readback，也没有 protocol execute。dynamic management HID
通道未加密、未认证，不得用于秘密数据。

> 本文描述 D3 已实现的 firmware wire contract。Python/CLI 和桌面 client 的
> v2 同步属于后续 D5；在 client 同步前，不得把旧的单槽 client 当作 v2 client。

## 1. 冻结值和安全边界

| 项目 | 固定值 |
| --- | --- |
| 动态槽数 | `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT`，默认 `8`，范围 `1..8` |
| 动态 slot 字段 | `0..dynamic_object_count-1`；`0xff`（static `LIST_SLOT`）无效 |
| committed/staging 存储 | 仅 RAM；不得调用 Settings/NVS |
| 最大文本长度 | `512` bytes |
| 合法文本字节 | `0x20..0x7e`，以及 `0x0a` (LF)、`0x09` (Tab)、`0x08` (Backspace) |
| 空文本上传 | 不允许；使用对应 slot 的 `DYNAMIC_CLEAR` |
| 默认 TTL | `300` seconds |
| 显式 TTL | `1..86400` seconds，包含两端 |
| staging inactivity timeout | `30` seconds |
| 执行后策略 | 默认接受后消费；可选上传后保留 |
| TTL 起点 | 最后一个合法 `DYNAMIC_DATA` 完成 committed 替换之后 |
| 执行入口 | 物理 `&runtime_macro_dynamic <slot>` behavior |
| dynamic 命令认证 | 不经过静态 management authorization gate；不刷新 auth session |
| dynamic readback | 不提供 |
| wire clear-all | 不提供；客户端必须逐槽发送 CLEAR |

每个 slot 有独立 committed text、TTL 和执行后消费/保留策略；staging buffer
只有一个，因此任一时刻只能有一个 active upload transaction。完整上传才替换
目标 slot 的 committed text。`BEGIN`、不完整上传、超时、非法 chunk、重复/乱序
chunk 和失败上传都不得破坏任何已 committed 的其他 slot。executor 忙或启动失败
时，目标 slot 必须保留。

所有 dynamic text 都必须被视为非秘密临时文本。禁止传输、保存、注入或依赖密码、
PIN、OTP/验证码、token、密钥或其他凭据；产品需要秘密用途时必须另行设计加密且
认证的通道。

## 2. Opcode、frame 和 slot 规则

### 2.1 Opcode

| 名称 | 值 | 作用 |
| --- | ---: | --- |
| `DYNAMIC_BEGIN` | `0x20` | 开始或重新开始指定 slot 的上传 |
| `DYNAMIC_DATA` | `0x21` | 追加指定 slot 的连续文本 chunk |
| `DYNAMIC_CLEAR` | `0x22` | 清除指定 slot 的 committed、TTL，并取消该 slot 的 staging |
| `CAPABILITIES` | `0x23` | 返回 dynamic v2 capability metadata |

协议版本仍为 `2`。本文是破坏式 dynamic wire 升级：`CAPABILITIES` 的
`capability_version`、对象数和最大长度已经改变，旧的单槽 client 不兼容；不提供
`0xff` legacy alias，也不提供自动降级。

### 2.2 32-byte v2 frame

每个 request 和 response 必须严格为 32 bytes。frame 不含 CRC；transport 负责
report 边界。字段与 `include/zmk/runtime_macro_protocol.h` 相同：

| Byte | Size | 字段 | 动态协议要求 |
| ---: | ---: | --- | --- |
| `0` | 1 | `version` | 固定为 `2`；错误 response echo 收到值 |
| `1` | 1 | `opcode` | 上表或现有静态/认证 opcode |
| `2` | 1 | `request_id` | response 原样 echo；同一上传的 BEGIN/DATA 必须相同 |
| `3` | 1 | `status` | request 必须为 `0` (`OK`) |
| `4` | 1 | `slot` | dynamic 必须为 `0..N-1`；`0xff` 只属于 static `LIST_SLOT` |
| `5` | 1 | `payload_length` | `0..22` |
| `6..7` | 2 | `offset` | 文本 byte offset；固定对象使用 `0` |
| `8..9` | 2 | `total_length` | 文本总长度，或 capability response 长度 |
| `10..31` | 22 | `payload` | 声明长度之后必须全为零 |

request 的 `status` 非零返回 `BAD_REQUEST`。`payload_length > 22` 返回
`BAD_LENGTH`，并且在读取尾部前不得越界。声明长度之后的 payload 尾部任何非零
字节返回 `BAD_REQUEST`。request 不能把 TTL、文本或其他数据放在声明长度之外。

response 生成时先全零填充，然后 echo request 的 `version`、`opcode`、`request_id`
和 `slot`。所有 dynamic 错误 response 必须满足：

```text
status          = 对应错误状态
payload_length  = 0
offset          = 0
total_length    = 0
payload[0..21]  = 0
```

除 `CAPABILITIES` 外，dynamic 成功 response 没有 payload，只返回状态和进度
metadata；任何 response 都不返回 dynamic text。

USB HID report descriptor 和 report 方向不变：逻辑 frame 是 32 bytes；现有
hidapi helper 写入时可以在 frame 前加 report-ID `0`，因此 host write 可以是
33 bytes，读取时可以是 32 或带前导零的 33 bytes。前导零不是协议 frame 字段。

### 2.3 Status codes

dynamic 协议复用现有 status 值：

| 名称 | 值 | dynamic 含义 |
| --- | ---: | --- |
| `OK` | `0` | 请求成功 |
| `BAD_VERSION` | `1` | `version` 不是 `2` |
| `BAD_OPCODE` | `2` | firmware 不认识 opcode |
| `BAD_REQUEST` | `3` | status、payload tail、事务 metadata 或固定字段非法；也用于无 active transaction 的 DATA |
| `BAD_SLOT` | `4` | slot 不是有效 dynamic slot，包括 `0xff` |
| `BAD_OFFSET` | `5` | offset 超出对象，或不是下一个连续 offset |
| `BAD_LENGTH` | `6` | total/payload/TTL 长度或范围非法 |
| `INVALID_TEXT` | `7` | DATA payload 含不支持的 byte |
| `STORAGE_ERROR` | `8` | dynamic RAM 路径不返回；仅静态 Settings 路径使用 |
| `INTERNAL` | `9` | 不可预期的 RAM/module failure；不得返回文本 |
| `AUTH_REQUIRED` | `10` | dynamic 分支不返回 |
| `AUTH_FAILED` | `11` | dynamic 分支不返回 |
| `AUTH_NOT_CONFIGURED` | `12` | dynamic 分支不返回 |
| `RATE_LIMITED` | `13` | dynamic 分支不返回 |
| `AUTH_NO_CHALLENGE` | `14` | dynamic 分支不返回 |
| `CREDENTIAL_INVALID` | `15` | dynamic 分支不返回，即使 auth state 为 `ERROR_LOCKED` |

## 3. Capability discovery

### 3.1 Request

`CAPABILITIES (0x23)` request 必须使用有效 dynamic slot；推荐 client 使用
slot `0`，firmware 对任意 `0..N-1` 都返回相同 capability metadata：

```text
version=2
opcode=0x23
status=0
slot=0..N-1
offset=0
total_length=0
payload_length=0
payload[0..21]=0
```

`slot=0xff` 或其他越界值返回 `BAD_SLOT`。offset、total_length 或 payload
不符合 empty object 时返回 `BAD_REQUEST`。CAPABILITIES 的 opcode handling 不
改变 dynamic committed/staging/TTL、queued request 或 auth session，也不主动调用
或刷新 auth session；外围 auth state observation 的规则见 §7。

### 3.2 Response payload

成功 response 固定为 `status=OK`、`offset=0`、`total_length=22`、
`payload_length=22`。payload 布局：

| Payload byte | Size | 字段 | v2 值 |
| ---: | ---: | --- | --- |
| `0` | 1 | `capability_version` | `2` |
| `1` | 1 | `dynamic_object_count` | `N`（默认 `8`） |
| `2..3` | 2 | `lifecycle_flags` | little-endian，见下表 |
| `4..5` | 2 | `max_dynamic_length` | `512` |
| `6..9` | 4 | `default_ttl_seconds` | `300` |
| `10..13` | 4 | `min_ttl_seconds` | `1` |
| `14..17` | 4 | `max_ttl_seconds` | `86400` |
| `18..21` | 4 | `transaction_timeout_seconds` | `30` |

`lifecycle_flags`：

| Bit | 名称 | v2 含义 |
| ---: | --- | --- |
| `0` | `CLEAR_ON_BOOT` | `1`；RAM 初始化清除 |
| `1` | `CLEAR_ON_TTL_EXPIRY` | `1`；TTL 到期清除 |
| `2` | `CLEAR_ON_EXECUTION_ACCEPT` | `1`；默认接受时消费，可被上传覆盖为保留 |
| `3` | `CLEAR_ON_USB_DISCONNECT` | 按 Kconfig 声明 |
| `4` | `CLEAR_ON_BLE_PROFILE_CHANGE` | 按 Kconfig 声明 |
| `5` | `CLEAR_ON_SELECTED_ENDPOINT_CHANGE` | 按 Kconfig 声明 |
| `6` | `SUPPORTS_KEEP_AFTER_EXECUTE` | `1` |
| `7..15` | 保留 | 必须为 `0` |

client 遇到未知 capability version、错误 object count、非零 reserved bits 或
超出约定范围的数值，必须报告 malformed capability，不得发送 dynamic 写入。
只有 `SUPPORTS_KEEP_AFTER_EXECUTE=1` 时，client 才可以发送保留选项。

## 4. Dynamic store、TTL 和行为边界

### 4.1 RAM state

实现维护 `N` 个独立 slot，每个 slot 有 `512`-byte committed buffer、length、
valid、consume-on-accept、TTL deadline 和 generation；所有 slot 共享一个
`512`-byte staging buffer。mutex 串行化 begin/append/commit/clear/expire/execute。
只有一个 staging transaction，且记录目标 slot。

动态路径不得调用以下 static/storage API：

```c
zmk_runtime_macro_slot_set()
zmk_runtime_macro_slot_clear()
settings_save_one()
settings_delete()
```

dynamic 状态不进入 static `LIST`，static `GET` 不可读取它，static `SET`/`CLEAR`
不影响它，dynamic `DYNAMIC_CLEAR` 也不影响 static slots 或 credentials。dynamic
protocol staging 与 static `SET/PASSWORD_SET` staging 必须保持独立。

### 4.2 Commit、clear 和 TTL

- 合法 BEGIN 只替换共享 staging transaction；目标 slot 旧 committed 和所有其他
  slot 保持不变。
- 合法非最终 DATA 只追加到目标 slot staging；所有 committed 保持不变。
- 最终 DATA 在临界区内把完整 staging 原子替换到目标 slot，启动该 slot TTL，
  然后 zeroize staging。
- TTL 从 commit 完成时刻开始；到期只清对应 slot。单 TTL work 扫描所有 slot
  的最早 deadline，旧 work 不得清理新 commit。
- `DYNAMIC_CLEAR(slot)` 只清目标 slot 的 committed/TTL；如果 staging 目标正是
  该 slot，则取消 staging 和对应 transaction。如果 staging 属于其他 slot，CLEAR
  不取消它。
- CLEAR 是幂等的。没有 wire clear-all；需要全清时 client 必须逐 slot CLEAR。
- staging inactivity timeout 只清 staging transaction，保留所有 committed。
- reboot/reset 的 volatile initialization 清空全部 slot、staging 和 TTL。
- clear、替换、到期和取消必须 zeroize 相应 RAM；日志不得包含 dynamic text。

### 4.3 Physical behavior

协议没有 execute opcode。物理 behavior 使用参数化 binding：

```dts
&runtime_macro_dynamic <slot>
```

slot 必须在 `0..N-1`；越界 binding 返回错误，不执行、不消费、不修改状态。零
参数 dynamic binding 已移除。单一 executor 全局互斥：

- empty 或已到期 slot：无输出；
- executor 忙：返回 `-EBUSY`，不消费；
- start/schedule 失败：保留 committed；
- executor 接受 snapshot：默认消费该 slot；KEEP flag 上传则保留该 slot；
- executor 使用独立 `512`-byte snapshot，CLEAR 或 lifecycle 不中断 snapshot；
- static 和 dynamic 不并发、不排队、不创建第二个 worker；snapshot 最终 zeroize。

## 5. Command wire contract

### 5.1 `DYNAMIC_BEGIN (0x20)`

Request 必须满足：

| 字段 | 要求 |
| --- | --- |
| `version` | `2` |
| `opcode` | `0x20` |
| `status` | `0` |
| `slot` | `0..N-1` |
| `offset` | `0` |
| `total_length` | `1..512` |
| `payload_length` | 只能是 `0`、`1`、`4` 或 `5` |
| payload `0` | 默认 TTL `300`，执行后消费 |
| payload `1` | `payload[0]` 为 BEGIN flags，使用默认 TTL |
| payload `4` | `payload[0..3]` 为 TTL seconds uint32 LE |
| payload `5` | 前 4 bytes 为 TTL，`payload[4]` 为 flags |

TTL 必须在 `1..86400`。BEGIN flags 当前只有 bit `0`：
`KEEP_AFTER_EXECUTE`。其他 flags 返回 `BAD_REQUEST`；TTL/长度错误返回
`BAD_LENGTH`；offset 错误返回 `BAD_OFFSET`；slot 错误返回 `BAD_SLOT`。这些
错误会取消当前 staging，但保留所有旧 committed。

每个合法 BEGIN 都是明确 restart：它可以替换共享 staging，不要求新的 request ID
与旧 transaction 不同。server 记录 slot、request ID、total、TTL、策略和
`received=0`，并启动/刷新 30-second inactivity timeout。

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
| `slot` | 等于 active BEGIN 的 slot |
| `request_id` | 等于 active BEGIN 的 request ID |
| `total_length` | 等于 active BEGIN 的 total，且 `1..512` |
| `offset` | 等于 active transaction 当前 received |
| `payload_length` | `1..22`，且不超过 `total_length-offset` |
| payload | 每个 byte 必须为允许字符 |
| payload 尾部 | 声明长度之后全零 |

实现按以下边界处理：common frame validation 后，slot 越界或与 active slot 不同
返回 `BAD_SLOT` 并取消 staging；total 错误返回 `BAD_LENGTH`；offset 错误返回
`BAD_OFFSET`；无 active transaction、request ID/total 不匹配返回 `BAD_REQUEST`；
非法字符返回 `INVALID_TEXT`。所有失败都保留旧 committed，不允许隐式 BEGIN。

每个 DATA payload 最多 `22` bytes；`512` bytes 最多需要 `24` 个 chunk
（`23*22 + 6`）。成功 response 不返回文本：

```text
status=OK
payload_length=0
offset=offset + payload_length
total_length=active total
payload 全零
```

最终 DATA 的 response offset 等于 total，且此时目标 committed 已替换、TTL 已启动。

### 5.3 `DYNAMIC_CLEAR (0x22)`

Request 必须为：

```text
version=2
opcode=0x22
status=0
slot=0..N-1
offset=0
total_length=0
payload_length=0
payload[0..21]=0
```

`slot=0xff` 或越界返回 `BAD_SLOT`；其他 empty-object 错误返回 `BAD_REQUEST`，
错误 request 不改变 dynamic 或 static staging。合法 CLEAR 只作用于指定 slot，
成功 response 为 `status=OK`、`payload_length=0`、`offset=0`、`total_length=0`。

## 6. Transaction、timeout 和恢复

- 一个 shared staging transaction 的 BEGIN/DATA 使用同一 request ID；response echo
  version/opcode/request ID/slot。
- 每次合法 BEGIN 都是 restart；client 发生 transport error、ACK 丢失或连接重置
  时，必须使用新的 request ID 从 BEGIN 完整上传，不重发旧 DATA 作为恢复手段。
- DATA duplicate 或 out-of-order 返回 `BAD_OFFSET` 并取消 staging；request ID 或
  total mismatch 返回 `BAD_REQUEST` 并取消 staging。
- final ACK 丢失后重发 final DATA 会因无 active transaction 返回 `BAD_REQUEST`，
  不会再次读取或返回文本，也不会清除已经 committed 的 slot。
- timeout 固定 `30` 秒，仅清 staging；slot committed/TTL 不受影响。
- CLEAR 幂等；transport timeout 可以用新的 request ID 重发同一 slot CLEAR。
- capability timeout/transport error 可以重试；合法 capability response 必须严格
  校验，不能静默接受未知版本或 reserved bits。

## 7. Authentication boundary

dynamic branch 必须在 static management authorization gate 之外。common frame
validation 仍先执行，但 dynamic request 不得调用 auth refresh，不得因为成功或
失败刷新 static session。

`runtime_macro_protocol_process()` 在 request 前会观察 auth state；如果该观察
实际触发 lazy session expiry，按认证契约丢弃 incomplete dynamic staging 和 static
`SET/PASSWORD_SET` staging，但不清任何 committed dynamic text。CAPABILITIES 本身
不主动 refresh session。

| Auth state | `CAPABILITIES` | `DYNAMIC_BEGIN/DATA/CLEAR` |
| --- | --- | --- |
| `OPEN` | 按格式处理 | 按格式处理 |
| `PROTECTED`、session 无效 | 按格式处理 | 按格式处理，不返回 `AUTH_REQUIRED` |
| `PROTECTED`、session 有效 | 按格式处理 | 按格式处理，不刷新 session |
| `ERROR_LOCKED` | 按格式处理 | 按格式处理，不返回 `CREDENTIAL_INVALID` |

**安全警告：dynamic management HID 通道未加密且不要求 static password 认证，
不得用于 OTP/验证码、密码、PIN、token、密钥或其他秘密。** 拥有 HID 访问权限的
本机程序可以写入或清除 dynamic buffer；firmware 不能识别来源进程。

`LOCK`、成功 `AUTH_PROVE`、成功 `PASSWORD_SET`、实际发生的 session lazy expiry、
transport reset 和其他明确的 auth/transport lifecycle transition 可以丢弃未完成
staging，但不清 committed dynamic text；static staging 按其独立认证契约处理。

## 8. Transport、feature gate 和实现约束

- `CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC` 仍依赖 `CONFIG_ZMK_RUNTIME_MACRO_USB_HID`。
  USB management 关闭时不得编译 dynamic store、protocol dynamic 分支或分配
  dynamic buffer；引用 dynamic behavior 必须由 fail-closed guard 拒绝。
- dynamic protocol commands 不刷新静态 auth session，也不进入 Settings/NVS。
- USB disconnect、BLE profile/endpoint 等启用的 lifecycle policy 使用内部
  `clear_all` 清除全部 dynamic slots；它们不是 `DYNAMIC_CLEAR` 的 clear-all wire
  语义。
- dynamic slot 数、512-byte store 和 512-byte executor 必须由全新 build 目录的
  map/size 数据验证；host protocol tests 必须覆盖 slot 0..7、`0xff`/越界、
  512 bytes/24 chunks、slot isolation、逐槽 clear 和无 readback。
