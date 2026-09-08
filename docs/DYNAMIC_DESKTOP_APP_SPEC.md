# Dynamic Macro 桌面应用实施规范

## 1. 文档目的

本文是给桌面应用、后台服务和桌面端测试人员的交接规范。桌面应用应依据本文实现
Runtime Macro dynamic macro 的发现、能力显示、临时文本上传和 clear；本文不要求修改
ZMK 主仓库，也不包含桌面应用实现代码。

本文只描述应用端职责。固件端的逐字节 wire contract 以
[`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md) 为唯一权威来源；认证公共契约以
[`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md) 为唯一权威来源。
仓库中的 `tools/runtime_macro_cli.py` 是可运行的 Python 参考 client，可作为 wire
行为和 fake-HID 测试的对照实现。

## 2. 范围与组件职责

### 2.1 桌面应用负责

桌面应用或后台服务必须负责：

- 在 Linux、macOS、Windows 上发现并选择正确的 Runtime Macro management HID；
- 打开独占的、可关闭的 HID transport，并串行化同一设备上的 request；
- 在发送任何 dynamic request 前校验本地输入；
- 发送 `CAPABILITIES`，严格校验 response；
- 按本规范执行 `DYNAMIC_BEGIN`、`DYNAMIC_DATA`、`DYNAMIC_CLEAR`；
- 过滤 stale response，校验 response metadata，并按 retry 规则恢复；
- 在 UI 中明确显示“dynamic 不支持”“上传中”“已提交（本地观察状态）”“已清除”和错误；
- 不自动登录、不保存密码、不把 dynamic operation 接入 static auth 状态机；
- 不把动态文本、HID frame、密码或其他敏感信息写入普通日志；
- 在设备断开、应用重启或无法确定生命周期后，把本地 active 状态标记为未知，而不是假设
  dynamic text 仍然存在。

### 2.2 固件负责

固件负责：

- 按 `DYNAMIC_PROTOCOL.md` 解析和校验 32-byte v2 frame；
- 保持一个 RAM-only dynamic object，不写 Settings/NVS；
- 完成 staging 到 committed 的原子替换、TTL、executor 消费和 lifecycle clear；
- 不提供 dynamic text readback；
- 在 `PROTECTED` 或 `ERROR_LOCKED` 时仍处理合法 dynamic request，但不使用 static
  password gate；
- 发布固定 capability metadata 和实际 lifecycle flags。

桌面应用不能用 static `LIST/GET/SET/CLEAR` 代替 dynamic API，也不能通过读取 static
slot 推断 dynamic 内容。

### 2.3 共享契约

以下文件必须作为同一版本一起维护：

1. `docs/DYNAMIC_PROTOCOL.md`：dynamic v1 wire contract；
2. `docs/PROTOCOL.md`：32-byte v2 frame、status 和 HID transport 基础规则；
3. `docs/AUTHENTICATION_PROTOCOL.md`：static auth 状态机；dynamic 不改变该状态机；
4. `tools/runtime_macro_cli.py`：参考 client；
5. 本文：桌面应用交互、UI 和验收要求。

## 3. 产品边界和安全要求

### 3.1 Dynamic 不是密码保护通道

Dynamic opcode 不经过 static password authorization gate：

- `CAPABILITIES`、`DYNAMIC_BEGIN`、`DYNAMIC_DATA`、`DYNAMIC_CLEAR` 不要求先 login；
- 设备处于 `OPEN`、`PROTECTED` 或 `ERROR_LOCKED` 时，合法 dynamic request 都应按
  dynamic contract 处理；
- dynamic request 不自动执行 `AUTH_INFO`/`AUTH_CHALLENGE`/`AUTH_PROVE`；
- dynamic request 不刷新、不延长、不修改桌面端已有的 static authenticated session；
- `DYNAMIC_CLEAR` 不清 static slots、密码、static staging 或 auth session；
- `LOCK` 也不等价于 `DYNAMIC_CLEAR`，需要清 committed dynamic object 时必须显式 clear。

USB management HID 未加密、未认证。Dynamic 只允许用于非-secret 文本，这是产品和客户端
使用约束，不是固件可验证的安全保证。禁止通过 dynamic 传输、保存、注入或依赖以下内容：

- 密码、PIN、OTP、验证码；
- token、API key、session cookie；
- 加密密钥、恢复码、私钥；
- 任何需要保密性、完整性或来源认证的文本。

应用日志不得记录 dynamic text 或完整 frame。若 UI 需要显示用户输入，只在用户可见的
编辑区域显示，不复制到诊断日志、崩溃报告或遥测。

### 3.2 Volatile 与一次性语义

Dynamic object 具有以下语义，UI 必须照此表达：

- 只有一个 object，最大 `256 bytes`；
- 仅存在 RAM，重启/掉电后为空；
- 成功上传后有 TTL，默认 `300 seconds`；
- 显式 TTL 范围为 `1..86400 seconds`，两端包含；
- TTL 从最后一个合法 DATA 完成 committed 替换时开始，不从 BEGIN 开始；
- 物理 `&runtime_macro_dynamic` behavior 被 executor 接受后立即消费；
- executor 忙或启动失败时不消费，固件返回错误或保留 object；
- 没有 GET、LIST 或任何 dynamic readback；
- 空文本不能上传，清空必须使用 `DYNAMIC_CLEAR`。

应用只能显示“最近一次上传已收到成功 ACK”的本地观察状态，不能声称它能读取或证明
设备当前仍保存该文本。TTL 到期、固件重启或 lifecycle policy clear 都可能在没有应用
request 的情况下清除 object。

## 4. HID 设备发现与连接

### 4.1 选择 management HID

Runtime Macro 使用现有 vendor HID management interface，默认 usage page/usage 为：

- Usage Page：`0xff60`；
- Usage：`0x61`；
- 逻辑 request/response frame：`32 bytes`。

hidapi 写入时可能需要在逻辑 frame 前加 report ID `0`，因此 host write 可能是 `33 bytes`；
读取可能返回 `32 bytes` 或带前导零 report ID 的 `33 bytes`。report ID 不属于协议 byte 0。
应用应复用现有 HID transport 的 report normalization，不要把 report ID 误当成 version。

设备选择要求：

1. 优先使用配置的 vendor/product、product name 和 HID path；
2. 如果匹配到多个 management HID，不能静默选择，必须要求用户选择或使用精确 path；
3. Linux 某些 hidapi 后端会把 usage/usage_page 报为 `0`，不能因此猜测设备；
4. Totem 等同时暴露 Raw HID 和 Runtime Macro HID 的设备，必须按 product name、接口
   信息和已配置的 Runtime Macro path 选择，不能简单按“接口号最大/最小”猜测；
5. 不要硬编码某一台设备的接口号；接口号可能随 descriptor、板卡和操作系统变化；
6. 连接对象必须支持 hot-unplug、read timeout、write failure 和 close，不得让 HID worker
   永久阻塞 UI 线程。

### 4.2 连接生命周期

应用应把一次 HID 打开过程视为短生命周期 session：

- 打开后先执行 capability discovery；
- 同一设备同时只能有一个 dynamic transaction；
- 所有 request 必须在一个串行队列中发送，不能并行发送 BEGIN/DATA/CLEAR；
- 任意 write/read error、设备消失或 close 后，丢弃本地未完成 transaction；
- 重新连接后必须重新 discovery，不得复用旧的 request ID、旧的 capability 或旧的
  “active” UI 状态；
- 应用关闭时可尽力发送 `DYNAMIC_CLEAR`，但不能把 host shutdown 解释成固件必然收到
  disconnect clear。无论是否发送成功，关闭后的本地状态都应标记为 unknown。

## 5. v2 Frame 基础

每个 request/response 的逻辑 frame 固定为 32 bytes，所有整数 little-endian：

| Byte | Size | 字段 | 应用端要求 |
| ---: | ---: | --- | --- |
| `0` | 1 | `version` | request 固定 `2`；response 必须与 request 匹配 |
| `1` | 1 | `opcode` | 必须与当前操作匹配 |
| `2` | 1 | `request_id` | response echo；每次新 retry 使用新值 |
| `3` | 1 | `status` | request 必须为 `0` |
| `4` | 1 | `slot` | dynamic/capability 固定 `0xff` |
| `5` | 1 | `payload_length` | `0..22` |
| `6..7` | 2 | `offset` | little-endian；按操作校验 |
| `8..9` | 2 | `total_length` | little-endian；按操作校验 |
| `10..31` | 22 | `payload` | 声明长度之后必须全为零 |

应用生成 frame 时必须先全零填充，再写 header 和 payload；不得把 TTL、文本或其他数据
放进 declared payload 之后。

动态 opcode：

| 名称 | 值 |
| --- | ---: |
| `DYNAMIC_BEGIN` | `0x20` |
| `DYNAMIC_DATA` | `0x21` |
| `DYNAMIC_CLEAR` | `0x22` |
| `CAPABILITIES` | `0x23` |

Dynamic/capability 使用 slot sentinel `0xff`。应用不得把 dynamic object 映射成 static
slot number。

## 6. Capability Discovery

### 6.1 Request

连接成功后，应用必须先发送 canonical `CAPABILITIES` request：

- version `2`；
- opcode `0x23`；
- status `0`；
- slot `0xff`；
- offset `0`；
- total length `0`；
- payload length `0`；
- 22 个 payload bytes 全零。

能力探测本身可以因 timeout/transport error 使用新 request ID 重试。收到匹配但 malformed
response 时立即报告 protocol error，不要把它当作“不支持”或静默重试。

### 6.2 Response payload

成功 response 必须有 `payload_length=22`、`offset=0`、`total_length=22`。payload：

| Payload byte | Size | 字段 | v1 固定值 |
| ---: | ---: | --- | ---: |
| `0` | 1 | `capability_version` | `1` |
| `1` | 1 | `dynamic_object_count` | `1` |
| `2..3` | 2 | `lifecycle_flags` | little-endian |
| `4..5` | 2 | `max_dynamic_length` | `256` |
| `6..9` | 4 | `default_ttl_seconds` | `300` |
| `10..13` | 4 | `min_ttl_seconds` | `1` |
| `14..17` | 4 | `max_ttl_seconds` | `86400` |
| `18..21` | 4 | `transaction_timeout_seconds` | `30` |

应用必须拒绝以下 response，不得发送 BEGIN：

- capability version 不是 `1`；
- object count 不是 `1`；
- `max_dynamic_length/default/min/max TTL/transaction timeout` 不是上表固定值；
- lifecycle reserved bits `6..15` 非零；
- 必需 flags `CLEAR_ON_BOOT`、`CLEAR_ON_TTL_EXPIRY`、`CLEAR_ON_EXECUTION_ACCEPT`
  缺失；
- response metadata、payload length 或 payload tail 不正确。

Lifecycle flags：

| Bit | 名称 | 默认值 | UI 含义 |
| ---: | --- | ---: | --- |
| `0` | `CLEAR_ON_BOOT` | `1` | 重启后不保留 |
| `1` | `CLEAR_ON_TTL_EXPIRY` | `1` | TTL 到期后清除 |
| `2` | `CLEAR_ON_EXECUTION_ACCEPT` | `1` | behavior 被接受后消费 |
| `3` | `CLEAR_ON_USB_DISCONNECT` | `1` | 实际 management USB disconnect 清除 |
| `4` | `CLEAR_ON_BLE_PROFILE_CHANGE` | `0` | 默认切换 BLE profile 保留 |
| `5` | `CLEAR_ON_SELECTED_ENDPOINT_CHANGE` | `0` | 默认切换 selected endpoint 保留 |
| `6..15` | reserved | `0` | 非零视为 malformed |

应用应在 capability 页面显示 flags，而不是把默认值硬编码为唯一行为。例如用户可以
看到“USB disconnect：clear”“BLE profile：preserve”“selected endpoint：preserve”。

### 6.3 Unsupported 与 malformed 的区分

- `BAD_VERSION (1)`：当前不是 v2 management protocol，提示固件/协议版本过旧并停止；
- `BAD_OPCODE (2)`：v2 firmware 不支持 dynamic capability，显示“不支持 dynamic macro”；
- 合法 `OK`：进入 dynamic 功能；
- timeout/transport error：显示设备不可用或连接不稳定，不能推断能力；
- 合法 frame 但 capability payload 错误：显示 firmware/client protocol mismatch，不能降级。

无论哪一种 unsupported 情况，都不能自动降级为 static `SET`，也不能自动 login。

## 7. Upload 流程

### 7.1 本地校验

在任何 HID write（包括 capability discovery）之前完成：

1. 将输入转换为 UTF-8/byte 结果时必须明确产品编码策略；协议本身只接受 bytes；
2. 长度为 `1..256 bytes`，不是字符数；
3. 每个 byte 必须属于：`0x20..0x7e`、LF `0x0a`、Tab `0x09`、Backspace `0x08`；
4. 拒绝 NUL、DEL、UTF-8 多字节字符、中文、Emoji 和其他 Unicode；
5. TTL 缺省表示 `300 seconds`；显式 TTL 必须为整数 `1..86400`；
6. 空文本不能转换为 clear，必须由用户明确点击 clear。

应用应在本地校验失败时不打开或不写入 HID，并给出字段级错误。不要先发送
`CAPABILITIES` 再发现文本非法。

### 7.2 BEGIN

完成本地校验后重新读取/确认 capability，然后生成一个新的 8-bit request ID。BEGIN：

- opcode `0x20`；
- slot `0xff`；
- offset `0`；
- total length 为文本 byte length，范围 `1..256`；
- payload length 为 `0`（使用 300 秒默认 TTL）或 `4`（显式 TTL）；
- 显式 TTL 为 uint32 little-endian seconds；
- payload tail 全零。

BEGIN response 必须是 empty-success response，且 `offset=0`、`total_length=请求的文本
长度`。收到其他 metadata、payload 或 request identity 时立即停止并报告 protocol error。

### 7.3 DATA 分块

BEGIN 成功后：

- 每个 DATA 使用同一个 request ID；
- chunk payload 最大 `22 bytes`；
- 第一个 offset 为 `0`，后续 offset 必须是前一 chunk 末尾；
- `total_length` 每个 DATA 都等于 BEGIN total；
- 最后一个 chunk 可以小于 22 bytes，但不能为零；
- 不要并行发送多个 DATA；必须等待并校验前一个 ACK；
- non-final ACK 的 offset 必须等于已发送 byte 总数；
- final ACK 的 offset 必须等于 total length；
- 所有 dynamic success response 的 payload 必须为空，不能含文本。

只有 final DATA 被固件接受后，文本才成为 committed；BEGIN、部分 DATA 或失败事务
不应替换旧 committed text。应用收到 final ACK 后可把 UI 标记为“已提交（TTL 从此刻开始）”，
但该状态仍不是 readback 证明。

### 7.4 Retry 与 request ID

同一次 upload 的 BEGIN/DATA 使用一个 request ID；每一次完整重试使用新 request ID，并
从新的 BEGIN 开始。禁止只重发某个 DATA 作为恢复手段。

需要从 BEGIN 重启的情况：

- HID write/read timeout；
- 设备重置、连接丢失或 transport error；
- DATA 收到 `BAD_REQUEST` 或 `BAD_OFFSET`；
- final DATA ACK 丢失。

final ACK 丢失时，第一次 commit 可能已经成功。应用仍必须用新 ID 从 BEGIN 重新上传；不要
重发旧 final DATA，也不要把 `BAD_REQUEST` 误认为文本读取失败。重新完整上传相同文本会
产生相同 committed 内容，并从新的 commit 时刻重新计算 TTL。

不能自动重试的情况：

- `BAD_LENGTH`：本地长度/TTL/frame 错误，先修复；
- `BAD_SLOT`：应用 frame 错误；
- `INVALID_TEXT`：输入校验遗漏或固件规则不一致；
- `INTERNAL`：报告 firmware error，由用户显式重试；
- response identity 匹配但 metadata malformed：立即报告 protocol error，不静默重试。

重试次数应由应用配置限制并展示最终失败原因。单个设备上不得有两个 upload worker
同时重试。

## 8. Clear 流程

`DYNAMIC_CLEAR` 是幂等的 canonical empty request：

- opcode `0x22`；
- version `2`、status `0`、slot `0xff`；
- offset `0`、total `0`、payload length `0`；
- payload 全零。

应用应先完成 capability discovery；不应使用 clear 探测固件是否支持 dynamic。匹配的
成功 response 必须是空 payload、offset/total 都为零。

Clear timeout、设备重置或 transport error 可用新 request ID 重发同一个 clear frame。Clear
不会影响 static slots、credentials、static staging、auth session 或已被 executor 接受的
executor snapshot。用户取消上传时，可以显式 clear；如果应用只是关闭连接，则不能假定
固件一定收到 clear。

## 9. Response 与错误处理

应用必须先由 transport 层丢弃 stale response，再检查当前 response：

- version；
- opcode；
- request ID；
- slot；
- status；
- payload length；
- offset；
- total length；
- declared payload 后的零填充。

匹配 response 的错误状态也必须验证 canonical empty error layout：payload length、offset、
总长度和 payload 全为零。不要只看 status。

dynamic 不应返回 auth status。如果收到了 `AUTH_REQUIRED`、`AUTH_FAILED`、`CREDENTIAL_INVALID`
等 auth status，应作为 firmware/protocol mismatch 报告，不能偷偷执行 login。应用已有的
static login 状态必须保持不变。

建议向 UI 映射为以下类别，而不是直接展示所有底层数字：

| 类别 | 典型 status/原因 | UI 行为 |
| --- | --- | --- |
| Unsupported | `BAD_VERSION`、`BAD_OPCODE` | 关闭 dynamic 控件，保留 static 功能 |
| Input | 本地长度、字符或 TTL 错误 | 不发 HID，定位到输入字段 |
| Recoverable transport | timeout、disconnect、`BAD_OFFSET`、DATA `BAD_REQUEST` | 自动从新 BEGIN 重试，显示进度 |
| Protocol mismatch | malformed capability/ACK、identity mismatch | 停止重试，提示升级或报告 bug |
| Firmware failure | `INTERNAL`、未知 status | 停止当前事务，提示用户显式重试 |
| Cleared/unknown | disconnect、reconnect、reboot、TTL 未知 | 不声称 object 仍存在，要求重新上传 |

## 10. UI 和后台服务行为

### 10.1 推荐状态

应用可以使用以下本地状态：

- `Unsupported`：能力探测明确返回 BAD_OPCODE/BAD_VERSION；
- `Ready`：能力合法，尚未在本次连接中成功上传；
- `Uploading(offset, total)`：正在等待 BEGIN/DATA ACK；
- `CommittedLocally(ttl, committed_at)`：本次连接收到 final ACK，只表示本地观察；
- `ClearedLocally`：本次连接收到 clear ACK；
- `Unknown`：连接断开、应用重启、固件重启可能发生或事务结果不明；
- `Error`：当前操作失败，保留可重试入口。

`CommittedLocally` 不应在应用重启后自动恢复，也不应显示为“设备当前内容”，因为没有
readback。TTL 倒计时只能标记为本地估计；如果设备时钟、传输重试或 lifecycle event
不确定，应转为 `Unknown`。

### 10.2 USB management 与 BLE output 工作流

支持“USB host A 管理、BLE host B 输出”的正确顺序：

1. 应用 A 打开 Runtime Macro management HID，并完成 capability discovery；
2. 键盘选择 BLE profile B / `OUT_BLE`；默认 endpoint/profile policy 会保留 dynamic object；
3. 应用 A 在 BLE profile 切换完成后上传 dynamic text；
4. 用户按物理 `&runtime_macro_dynamic` behavior；
5. 文本通过当前 selected output B 输出，不会作为普通键盘文本输出到 management host A；
6. A 的 management HID 仍可继续执行 capability、upload 或 clear；
7. 如果 capability flags 显示 profile/endpoint clear 已打开，应用必须在切换后把本地
   committed 状态标记为 unknown，并要求用户重新上传。

USB management disconnect 默认会清 committed dynamic object；suspend/resume、reset、
configured 和 host OS shutdown 不能被应用当作可靠的 disconnect clear 证明。实际拔出
但没有稳定 disconnect notification 时，固件也不承诺一定清除。

### 10.3 编辑器和日志

编辑器必须以 byte limit 为准显示 `0..256 bytes`，而不是只显示 Unicode code point 数量。
建议在提交按钮旁显示：

- 当前 byte length；
- 选中的 TTL 或“默认 300s”；
- capability 的最大长度和 TTL 范围；
- lifecycle flags 的用户可读摘要。

日志只记录 request 类型、长度、offset、total、request ID 和结果类别；不得记录 payload、
密码、token 或完整 HID frame。诊断导出应默认去除 product serial、HID path 和用户文本。

## 11. 测试与验收

### 11.1 Fake-HID 自动化

桌面应用测试至少覆盖：

1. 合法 capability response（flags `0x000f` 和可选 bit 4/5）；
2. `BAD_OPCODE`、`BAD_VERSION`、malformed capability、reserved flags；
3. 输入 1、22、23、256 bytes；
4. LF、Tab、Backspace、可打印 ASCII；
5. NUL、DEL、中文、Emoji、UTF-8 多字节、空文本、257 bytes；
6. invalid input 在任何 HID write 前失败；
7. 默认 TTL 和显式 TTL 的 little-endian BEGIN payload；
8. BEGIN/DATA 使用同一 request ID；
9. DATA timeout、BEGIN timeout、final ACK 丢失后用新 ID 从 BEGIN 重启；
10. DATA `BAD_REQUEST`/`BAD_OFFSET` 后从 BEGIN 重启；
11. malformed ACK offset/total/payload tail/identity 立即失败；
12. stale response 被丢弃且不延长 deadline；
13. clear timeout 使用新 ID 重试；
14. PROTECTED/ERROR_LOCKED 不触发 login；
15. dynamic 操作不调用 static LIST/GET/SET/CLEAR 或 auth API；
16. 设备断开、重新连接和 app 重启将本地状态置为 unknown；
17. 多设备、Raw HID 共存、Linux usage metadata 为 0 时的设备选择。

### 11.2 手工固件联调

使用当前固件和真实键盘至少验证：

#### A. 基础上传

1. 枚举 management HID；
2. 上传包含字母、数字、标点、LF、Tab、Backspace 的固定文本；
3. 按一次 `&runtime_macro_dynamic`，核对普通键盘输出；
4. 再按一次，确认已消费且无输出；
5. 上传后在 TTL 到期前执行成功，过期后无输出。

#### B. Busy 与 retry

1. 运行长 static macro；
2. 触发 dynamic behavior，确认 busy 时 dynamic 保留；
3. static 完成后再次触发，确认完整输出并消费；
4. 反向验证 dynamic running 时 static executor 不并发、不排队；
5. 在 BEGIN、DATA、final ACK 阶段注入断开/超时，确认应用从新 BEGIN 恢复。

#### C. Lifecycle

1. 默认切换 BLE profile 和 selected endpoint，dynamic 保留；
2. 实际拔掉 management USB，dynamic 被清除；
3. suspend/resume、reset、configured 不被错误显示为 disconnect clear；
4. 如果启用 optional profile/endpoint clear，切换后重新上传前不执行旧文本；
5. 重启/掉电后 object 为空；
6. host shutdown 只记录观察结果，不作为跨平台保证。

#### D. Static/auth 回归

1. static LIST/GET/SET/CLEAR 保持正常；
2. OPEN/PROTECTED/ERROR_LOCKED 下 dynamic 都不自动 login；
3. dynamic 不出现在 static LIST/GET；
4. dynamic clear 不改变 static staging、credentials 或认证状态；
5. dynamic 操作不延长 authenticated session。

### 11.3 通过标准

桌面应用实现满足以下条件才算完成：

- Linux、macOS、Windows 三个平台均能完成设备发现、capability 和基本 upload/clear；
- 所有本地非法输入在 HID write 前拒绝；
- 所有 dynamic frame 和 ACK 校验与 `DYNAMIC_PROTOCOL.md` 一致；
- upload retry 永远以新 request ID 从 BEGIN 开始；
- 没有 dynamic readback、static fallback 或自动 login；
- UI 能区分 unsupported、transport retry、protocol mismatch、firmware failure 和 unknown；
- 日志和诊断不会泄露 dynamic text 或 secret；
- fake-HID 自动化和上述真实固件联调脚本全部通过。

## 12. 参考实现对照

仓库 Python client 已实现以下可直接对照的接口：

- `RuntimeMacroClient.get_capabilities()`；
- `RuntimeMacroClient.upload_dynamic(data, ttl_seconds=None)`；
- `RuntimeMacroClient.clear_dynamic()`；
- `DynamicCapabilities` 及 lifecycle flag 属性；
- `capabilities`、`dynamic-set`、`dynamic-clear` CLI；
- 59 个 Python fake-HID/static/auth/dynamic regression tests。

桌面应用不要求使用 Python，但其 observable behavior、retry 边界、错误分类和安全边界必须
与该参考 client 及 `DYNAMIC_PROTOCOL.md` 一致。
