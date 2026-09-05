# Runtime Macro 密码认证协议（v2 设计契约）

> **状态：固件已实现；桌面 GUI/CLI 尚未升级。**
>
> 本文是固件、桌面 GUI 和后台应用共同遵循的 v2 wire 契约。当前固件使用
> [`PROTOCOL.md`](PROTOCOL.md) 所述 v2 frame，并拒绝 v1 的宏管理命令；客户端必须
> 实现本文认证流程后才能管理 `PROTECTED` 设备。

## 1. 目标

v2 为 Runtime Macro 的 USB HID 管理通道增加可选密码保护，同时保持宏按键执行路径不变。

- 新刷固件或清除 Settings 后不强制设置密码，可以直接管理宏。
- 设置密码后，`LIST`、`GET`、`SET`、`CLEAR` 全部需要先认证。
- GUI 和后台应用使用同一套协议，不引入只供某一种客户端使用的特殊权限。
- 密码可以在已认证状态下更换，但不能通过协议清除或禁用。
- 忘记密码时只能刷 ZMK settings reset 固件；这会清除全部 Settings 数据。
- v2 仍使用现有独立 vendor HID interface 和固定 32-byte frame。

本协议提供访问控制和防止认证报文直接重放，不提供 USB 数据加密。

## 2. 安全边界

本设计面向个人使用，主要防止不知道密码的普通本机程序直接读取或修改宏。明确不处理：

- 固件提取、调试接口、任意固件刷写或 secure boot；
- USB 抓包、主机内核或已控制桌面会话的高权限恶意程序；
- 按键穷举和高等级物理攻击；
- 宏内容在正常按键执行时产生的键盘输出；
- 用户选择弱密码后发生的离线字典攻击。

不开启 ZMK Studio 时，普通 keyboard HID 不提供完整 keymap、层或键位绑定读取接口。将 Runtime Macro 按键放在需要组合键进入的隐藏层，只是降低物理试键发现宏的概率，不属于密码协议的一部分。

设备进入认证管理窗口后，该窗口对整个 Runtime Macro HID interface 生效，不绑定到某个 GUI 进程。因此同一主机上的其他进程可能在窗口有效期间利用该窗口。该限制在当前个人使用威胁模型内可以接受。

每个 `zmk_usb_conn_state_changed` 通知都会执行一次保守的逻辑 transport reset，先置离线，再清除认证窗口、challenge、协议事务、排队请求并递增 generation；只有该通知状态为 `ZMK_USB_CONN_HID` 且原始 USB status 双采样稳定时，逻辑清理完成后才重新接受请求。因此即使 ZMK 将快速 RESET/DISCONNECTED→CONFIGURED 通知合并为最终 HID 通知，也不会保留旧会话。IN response buffer 的 endpoint ownership 与逻辑清理分开处理：固件只在原始 `zmk_usb_get_status()` 明确为 `USB_DC_RESET`、`USB_DC_DISCONNECTED` 或 `USB_DC_CONFIGURED` 时回收 permit；`USB_DC_SUSPEND`、`USB_DC_RESUME`、`USB_DC_CLEAR_HALT` 以及 CONNECTED/UNKNOWN/ERROR 等状态保留 permit，等待正常 `DATA_IN` 或后续已知安全边界。异步通知期间状态双采样不稳定时既不提前回收 permit，也保持 transport offline，等待后续稳定通知；即使双采样稳定，也只有 raw status 映射出的 `zmk_usb_get_conn_state()` 与 event 的 `conn_state` 一致时才上线。该映射与 ZMK 保持一致：SUSPEND/CONFIGURED/RESUME/CLEAR_HALT/SOF 为 HID，DISCONNECTED/UNKNOWN 为 NONE，其余为 POWERED。由于 suspend/resume 等 USB 状态也可能映射为 HID，客户端应准备重新查询 `AUTH_INFO` 并重新认证。

## 3. 状态模型

### 3.1 持久状态

设备只有两种持久认证状态。

| 状态 | Settings 凭据记录 | 管理权限 |
| --- | --- | --- |
| `OPEN` | 不存在 | 所有管理命令可直接使用 |
| `PROTECTED` | 存在且有效 | 宏管理命令需要认证 |

#### `OPEN`

- 新刷且无旧 Settings 的设备处于 `OPEN`。
- 刷 settings reset 固件后回到 `OPEN`。
- 用户可以一直保持 `OPEN`，首次使用不强制设置密码。
- GUI 必须明显显示“未设置管理密码/宏未受保护”。
- `OPEN` 表示“没有凭据记录”，不是对空字符串设置密码。
- 任意拥有 HID 访问权限的进程都可以在 `OPEN` 下读取、修改宏或首先设置密码；GUI 应向用户说明这一点。

#### `PROTECTED`

- 成功执行 `PASSWORD_SET` 后进入 `PROTECTED`。
- `LIST`、`GET`、`SET`、`CLEAR` 必须在有效认证会话中执行。
- 已认证客户端可以用新的非空密码替换旧密码。
- 协议不提供 `PASSWORD_CLEAR`、恢复默认密码或禁用密码命令。
- 无法认证时，只能刷 settings reset 固件恢复到 `OPEN`。

settings reset 通常还会清除 Runtime Macro slots、蓝牙配对和其他 ZMK Settings 数据。这是忘记密码恢复的预期代价，而不是需要绕过的限制。

### 3.2 RAM 临时状态

固件维护以下非持久状态：

- 当前认证管理窗口及到期时间；
- 当前一次性 challenge 及到期时间；
- 连续认证失败次数和限速截止时间；
- 未完成的 `SET` staging；
- 未完成的 `PASSWORD_SET` staging。

这些状态不得写入 Settings。认证窗口过期时必须清除所有 staging。重启、每个 USB connection-state 通知、认证成功、密码成功变更或 Settings 凭据重新加载时，清除认证窗口、challenge、失败计数/限速和所有 staging。显式 `LOCK` 只清除认证窗口、challenge 和所有 staging，保留失败计数及 cooldown，防止未认证客户端通过反复 `LOCK` 绕过限速。

默认认证窗口为最后一次成功受保护管理操作后的 5 分钟，可通过
`CONFIG_ZMK_RUNTIME_MACRO_AUTH_SESSION_TIMEOUT` 调整；`AUTH_INFO` 不延长窗口。
challenge 默认 30 秒过期，可通过 `CONFIG_ZMK_RUNTIME_MACRO_AUTH_CHALLENGE_TIMEOUT`
调整，并且最多用于一次 `AUTH_PROVE` 尝试。

## 4. 密码和凭据

### 4.1 密码字节

桌面端必须按以下顺序处理用户输入：

1. 将字符串转换为 Unicode NFC；
2. 编码为 UTF-8 bytes；
3. 确认结果不是空 bytes；
4. 使用这些 bytes 执行 KDF。

协议不传输原始密码。固件收到的是 KDF 输出，因此无法自行判断输入是否为空；客户端不得对空输入执行 `PASSWORD_SET`。GUI 可以提出密码强度建议，但“不设置密码”必须通过保持 `OPEN` 表达，不能通过空字符串表达。

### 4.2 KDF

v2 使用：

```text
K = PBKDF2-HMAC-SHA256(
    password = NFC_UTF8(password),
    salt = random 16 bytes,
    iterations = 600000,
    output_length = 32 bytes
)
```

- 新密码默认使用 `600000` iterations。
- salt 必须由桌面端 CSPRNG 生成，每次设置或更换密码都生成新的 16 bytes。
- 固件保存 `iterations`、`salt` 和 `K`，但不执行 PBKDF2。
- `K` 是可以直接完成认证的对称密钥，安全等级等同于密码本身，必须作为敏感凭据处理。
- v2 固件必须接受 `600000`；为未来参数迁移，可以接受 `100000..5000000`，范围外返回 `CREDENTIAL_INVALID`。
- 全零 salt 或全零 `K` 必须拒绝。

允许更高迭代次数不能消除弱密码的离线字典风险。客户端不得把密码、`K`、proof 或新的凭据对象写入日志。

## 5. Challenge-response

### 5.1 Challenge

`PROTECTED` 状态下，客户端通过 `AUTH_CHALLENGE` 请求 challenge。设备使用 CSPRNG 生成 16-byte `nonce`。

- 新 challenge 会替换同一协议上下文中尚未使用的旧 challenge。
- nonce 只保存在 RAM。
- nonce 默认 30 秒过期。
- 一次 `AUTH_PROVE` 尝试后，无论成功或失败，nonce 都立即作废。
- 每个 USB connection-state 通知、`LOCK` 和密码变更都会立即作废 nonce。

### 5.2 Proof

客户端计算：

```text
message = ASCII("ZMK-RUNTIME-MACRO-AUTH-V2") || nonce
full_proof = HMAC-SHA256(K, message)
proof = full_proof[0:16]
```

ASCII domain 字符串不包含结尾 NUL；`||` 表示直接拼接。wire 中只发送 HMAC 输出的前 16 bytes。

固件必须使用 constant-time comparison 比较 proof。验证成功后创建认证管理窗口并清零连续失败计数；验证失败返回 `AUTH_FAILED`，作废 challenge，并更新 RAM-only 限速状态。

### 5.3 失败限速

固件不得通过 sleep 阻塞协议工作线程。第 `n` 次连续 proof 失败后，只记录一个未来可重试时间：

```text
cooldown_seconds = min(2^(n - 1), 8)
```

限速期内的 `AUTH_CHALLENGE` 返回 `RATE_LIMITED`，不生成 nonce。认证成功、重启或任一 USB connection-state 通知（transport reset）后连续失败计数及 cooldown 归零；显式 `LOCK` 不清除它们。客户端收到 `RATE_LIMITED` 后应等待，不得高频轮询；无法获知其他客户端造成的失败计数时至少等待 8 秒再重试。

该限速只减少在线猜测，不阻止攻击者利用公开 salt、nonce 和 proof 进行离线字典攻击。

## 6. v2 frame

v2 沿用 v1 的固定 32-byte frame 和 22-byte payload。所有整数仍为 unsigned little-endian。

| Byte | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | `version` | v2 固定为 `2` |
| 1 | 1 | `opcode` | 操作码 |
| 2 | 1 | `request_id` | response 原样返回；分块事务所有 chunk 相同 |
| 3 | 1 | `status` | request 必须为 `0`；response 为状态码 |
| 4 | 1 | `slot` | 宏 slot；认证管理命令固定为 `0xff` |
| 5 | 1 | `payload_length` | 当前 payload 长度，`0..22` |
| 6..7 | 2 | `offset` | 分块对象 offset |
| 8..9 | 2 | `total_length` | 逻辑对象总长度 |
| 10..31 | 22 | `payload` | payload；未使用部分必须为零 |

除本文明确覆盖的认证语义外，canonical request、response echo、错误 response 零填充、整数编码和 `LIST/GET/SET/CLEAR` 分块规则与 v1 相同。

固件实现 v2 后不得继续接受 v1 的 `LIST/GET/SET/CLEAR`，否则 v1 会成为认证旁路。收到 `version=1` 时返回 `BAD_VERSION`，response 的 version 仍 echo 收到的 `1`。

### 6.1 Opcodes

| Name | Value | 认证要求 |
| --- | ---: | --- |
| `LIST` | `0x01` | `OPEN` 无需；`PROTECTED` 需要 |
| `GET` | `0x02` | `OPEN` 无需；`PROTECTED` 需要 |
| `SET` | `0x03` | `OPEN` 无需；`PROTECTED` 需要 |
| `CLEAR` | `0x04` | `OPEN` 无需；`PROTECTED` 需要 |
| `AUTH_INFO` | `0x10` | 始终公开 |
| `AUTH_CHALLENGE` | `0x11` | 始终可请求，但仅 `PROTECTED` 能成功 |
| `AUTH_PROVE` | `0x12` | 使用一次性 challenge |
| `PASSWORD_SET` | `0x13` | `OPEN` 无需；`PROTECTED` 需要 |
| `LOCK` | `0x14` | 始终允许且幂等 |

### 6.2 Status codes

v2 保留 v1 的 `0..9`：

| Name | Value | Meaning |
| --- | ---: | --- |
| `OK` | `0` | 成功 |
| `BAD_VERSION` | `1` | 不支持的协议版本 |
| `BAD_OPCODE` | `2` | 未知 opcode |
| `BAD_REQUEST` | `3` | 字段、零填充或事务 metadata 非法 |
| `BAD_SLOT` | `4` | slot 越界 |
| `BAD_OFFSET` | `5` | offset 非法或不是下一个分块位置 |
| `BAD_LENGTH` | `6` | payload/total length 非法 |
| `INVALID_TEXT` | `7` | 宏文本非法 |
| `STORAGE_ERROR` | `8` | Settings 操作失败 |
| `INTERNAL` | `9` | 内部错误 |

v2 新增：

| Name | Value | Meaning |
| --- | ---: | --- |
| `AUTH_REQUIRED` | `10` | 当前操作需要有效认证窗口 |
| `AUTH_FAILED` | `11` | proof 不匹配；不进一步暴露失败原因 |
| `AUTH_NOT_CONFIGURED` | `12` | 当前为 `OPEN`，无需也不能执行 challenge 登录 |
| `RATE_LIMITED` | `13` | 认证失败退避期尚未结束 |
| `AUTH_NO_CHALLENGE` | `14` | challenge 不存在、已使用或已过期 |
| `CREDENTIAL_INVALID` | `15` | 新凭据对象、salt、KDF 参数或密钥非法 |

错误 response 继续使用 `payload_length=0`、`offset=0`、`total_length=0` 和全零 payload。

处理受保护宏命令时，固件先完成 version、opcode、request status、frame 长度和零填充等基础校验，再检查认证，最后检查 slot、offset 和宏内容语义。这样未认证请求不会通过 `BAD_SLOT` 等响应探测受保护数据。认证检查失败的 `SET` 必须同时丢弃该 context 的 SET staging。

## 7. 认证管理命令

所有认证管理命令使用 `slot=0xff`。除 `PASSWORD_SET` 外，request 的 `request_id` 只用于 response 匹配，不跨命令复用。

### 7.1 `AUTH_INFO (0x10)`

`AUTH_INFO` 始终公开，用于客户端在显示管理界面前查询状态和 KDF 参数。

Request：

```text
status=0
slot=0xff
payload_length=0
offset=0
total_length=0
payload 全零
```

成功 response 的 `payload_length=22`、`offset=0`、`total_length=22`：

| Payload offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | `flags` |
| 1 | 1 | `kdf_id`，v2 中 PBKDF2-HMAC-SHA256 为 `1` |
| 2..5 | 4 | `iterations`，little-endian |
| 6..21 | 16 | `salt` |

`flags`：

- bit 0：`PASSWORD_CONFIGURED`；`1` 表示 `PROTECTED`；
- bit 1：`SESSION_AUTHENTICATED`；当前认证窗口有效；
- bit 2..7：保留，必须为 `0`。

`OPEN` 时 flags 为 `0`、salt 全零、iterations 返回新密码推荐默认值 `600000`。`PROTECTED` 时返回实际保存的 iterations 和 salt。`AUTH_INFO` 不返回 `K`，也不延长认证窗口。

如果持久化凭据存在但无法校验，固件处于 `ERROR_LOCKED`：`AUTH_INFO` 必须返回 `CREDENTIAL_INVALID`，并使用错误 response 的全零 payload（`payload_length=0`、`offset=0`、`total_length=0`）。不得将其表现为成功的 `OPEN`，也不得泄露损坏凭据的 metadata。

### 7.2 `AUTH_CHALLENGE (0x11)`

Request 与 `AUTH_INFO` 的空 request 约束相同。

成功 response：

```text
payload_length=16
offset=0
total_length=16
payload[0..15]=nonce
payload[16..21]=0
```

特殊结果：

- `OPEN`：`AUTH_NOT_CONFIGURED`；
- 失败退避期：`RATE_LIMITED`；
- CSPRNG 失败：`INTERNAL`，不得返回弱 nonce 或重复旧 nonce。

### 7.3 `AUTH_PROVE (0x12)`

Request：

```text
status=0
slot=0xff
payload_length=16
offset=0
total_length=16
payload[0..15]=proof
payload[16..21]=0
```

成功 response 无 payload，`offset=0`、`total_length=0`，并创建认证窗口。

- 没有可用 challenge：`AUTH_NO_CHALLENGE`；
- proof 不匹配：`AUTH_FAILED`；
- `OPEN`：`AUTH_NOT_CONFIGURED`。

任何进入 proof 验证步骤的尝试都必须先消费 challenge，再做比较，避免同一 nonce 被反复尝试。

### 7.4 `PASSWORD_SET (0x13)`

`PASSWORD_SET` 使用与 `SET` 相同的严格分块事务模型，但逻辑对象固定为 52 bytes，`slot=0xff`：

| Object offset | Size | Field |
| ---: | ---: | --- |
| 0..3 | 4 | `iterations`，little-endian |
| 4..19 | 16 | 新 salt |
| 20..51 | 32 | 新 `K` |

典型 chunk 为：

```text
offset=0,  payload_length=22, total_length=52
offset=22, payload_length=22, total_length=52
offset=44, payload_length=8,  total_length=52
```

所有 chunk 必须使用相同的 `request_id`、`slot=0xff` 和 `total_length=52`。后续 offset 必须恰好等于已接收长度。非法 chunk、重复的非最终 chunk、metadata 改变、认证窗口过期或新 `offset=0` 事务都会按 v1 `SET` 的恢复规则丢弃旧 staging。

权限：

- `OPEN`：无需认证，可以首次设置密码；
- `PROTECTED`：第一个及后续每个 chunk 都要求认证窗口仍有效；
- 不存在用全零长度表达“清除密码”的形式；`total_length` 不是 52 时拒绝。

提交必须采用 **storage-first**，与宏 slot 的 RAM-first 语义不同：

1. 完整接收并校验 52-byte 对象；
2. 将完整新凭据写入 Settings；
3. 只有 Settings 成功后才替换 RAM 中的凭据并进入/保持 `PROTECTED`；
4. Settings 失败返回 `STORAGE_ERROR`，继续使用旧凭据和旧持久状态；
5. 成功后清除认证窗口、challenge、失败计数以及所有 `SET/PASSWORD_SET` staging。

成功 response 无 payload。客户端必须重新认证后才能执行下一项受保护操作。

如果最终 ACK 丢失，客户端不得盲目重复最后一个 chunk或整笔事务。应重新执行 `AUTH_INFO`：

- salt 已变成新 salt：使用新密码认证；
- salt 仍为旧 salt：使用旧密码认证后重试更换；
- 首次设置从 `OPEN` 变成 `PROTECTED`：使用新密码认证。

### 7.5 `LOCK (0x14)`

Request 与 `AUTH_INFO` 的空 request 约束相同。`LOCK` 始终允许且幂等：

- 清除认证窗口；
- 清除 challenge；
- 清除所有 `SET/PASSWORD_SET` staging；
- 保留连续失败计数和 cooldown，不能借此绕过认证限速；
- 不改变持久凭据或宏内容。

成功 response 无 payload。

## 8. 宏命令的 v2 认证规则

`LIST`、`GET`、`SET`、`CLEAR` 的数据格式和业务语义继续遵循 v1，只有 version 和认证门禁发生变化。

| 状态 | `LIST/GET/SET/CLEAR` |
| --- | --- |
| `OPEN` | 直接处理 |
| `PROTECTED` 且窗口有效 | 处理成功后刷新 5 分钟 inactivity timeout |
| `PROTECTED` 且窗口无效 | 返回 `AUTH_REQUIRED` |

任何会话过期、`LOCK`、USB connection-state 通知或密码成功变更必须丢弃未完成的多包 `SET`。不能允许认证前开始的事务在认证后继续，也不能允许认证窗口内开始的事务在窗口过期后提交。

宏按键执行不经过本协议，不要求密码，也不创建或刷新认证窗口。

## 9. 桌面 GUI 实现流程

### 9.1 连接与状态

```text
连接 runtime macro HID
  -> 发送 v2 AUTH_INFO
  -> OK:
       PASSWORD_CONFIGURED=0 -> OPEN 管理界面 + 明显未保护提示
       PASSWORD_CONFIGURED=1 -> 锁定管理界面 + 登录入口
  -> BAD_VERSION 且响应表明固件为 v1:
       显示“旧固件不支持认证”警告
       不得把 v1 自动当作安全管理模式
```

GUI 可以提供用户明确确认后的 legacy v1 管理入口，但不得自动静默降级，因为 v1 的全部宏操作均未认证。

### 9.2 登录

```text
AUTH_INFO -> 读取 iterations + salt
用户密码 -> NFC -> UTF-8 -> PBKDF2 -> K
AUTH_CHALLENGE -> nonce
proof = Truncate16(HMAC-SHA256(K, domain || nonce))
AUTH_PROVE(proof)
  -> OK: 进入管理界面
  -> AUTH_FAILED: 清除内存中的输入和 K，显示密码错误
  -> RATE_LIMITED: 等待后重新从 AUTH_CHALLENGE 开始
```

客户端不得重用 nonce 或 proof。传输超时后必须重新执行 `AUTH_CHALLENGE`，不能重发旧 `AUTH_PROVE`。

### 9.3 首次设置密码

```text
确认 AUTH_INFO 为 OPEN
获取并二次确认非空新密码
生成 random salt
计算 K
分块发送 PASSWORD_SET
收到最终 OK
重新 AUTH_INFO，确认 PROTECTED 且 salt 匹配
使用新密码重新登录
```

用户关闭设置对话框时保持 `OPEN`，不应强制设置密码。

### 9.4 更换密码

```text
使用旧密码登录
获取并二次确认非空新密码
生成新的 random salt
计算新 K
分块发送 PASSWORD_SET
最终 OK 后旧会话立即失效
重新 AUTH_INFO 并使用新密码登录
最后更新 OS credential store
```

OS credential store 只能在设备确认新凭据已经生效后更新。最终 ACK 丢失时按 `PASSWORD_SET` 的 salt 检查流程判断新旧凭据，不得先覆盖唯一可用的旧凭据。

### 9.5 退出管理

用户点击“退出管理”、GUI 正常退出或切换设备时，应尽力发送 `LOCK`。客户端不能假设关闭窗口必然成功送达；固件自身的 timeout 和 USB lifecycle 清理仍是最终保障。

## 10. 后台应用和凭据保存

后台应用与 GUI 使用完全相同的 `AUTH_INFO -> AUTH_CHALLENGE -> AUTH_PROVE` 流程。固件不区分“GUI 管理员”和“后台服务”。

如果用户选择记住凭据：

- 只能保存到操作系统 credential store/keychain；
- 可以保存原密码，也可以保存派生后的 `K`；保存 `K` 时必须视为等同密码；
- 保存记录应绑定到明确的设备 profile；不要仅依赖可能变化的 HID path；
- 不得写入普通 JSON、TOML、日志、崩溃报告、命令行参数或环境诊断输出；
- 密码变更成功后再原子替换保存的凭据；
- 发现 salt/iterations 与保存记录不一致时，不得继续使用旧 `K`，应要求重新输入密码。

后台批量修改时应保持单一串行协议消费者，完成后主动 `LOCK`。多个本机客户端同时管理同一 HID interface 不受支持，应由桌面端/后台服务自行协调独占访问。

## 11. 兼容性与迁移

### 11.1 从当前 v1 升级

现有 v1 固件没有凭据记录。升级到 v2 固件但保留旧 Settings 时：

- 原有宏 slots 仍可能存在；
- 认证状态为 `OPEN`；
- 在用户设置密码前，旧宏可以通过 v2 管理通道直接读取或修改；
- GUI 必须立即显示未保护状态并建议设置密码，但不得强制。

这是无预共享凭据迁移到可选密码机制的必然窗口。

### 11.2 客户端版本探测

- v2 客户端首先发送 `version=2` 的 `AUTH_INFO`。
- v2 固件不接受 v1 宏命令。
- v1 固件通常会对 `version=2` 返回 `BAD_VERSION`。
- 客户端可以识别并提示升级固件；如保留 legacy 模式，必须明确标注“无认证”。
- 不允许在 v2 `AUTH_REQUIRED`、`AUTH_FAILED` 或传输超时后自动回退到 v1。

## 12. 实现阶段必须验证的行为

固件和参考客户端至少需要覆盖：

- `OPEN` 下所有宏命令可用，且可一直不设置密码；
- 首次设置密码后立即进入 `PROTECTED`，旧开放访问不再可用；
- `PROTECTED` 下四个宏命令都返回 `AUTH_REQUIRED`，不存在 v1 旁路；
- 正确、错误、重复、过期和缺失 challenge；
- constant-time proof 比较路径；
- 限速不阻塞协议 worker；
- 认证 timeout、`LOCK`、每个 USB connection-state 通知、重启的 session/challenge/staging 清理，以及 `LOCK` 保留失败限速、transport reset/认证成功清除失败限速；
- `SET/PASSWORD_SET` staging 在所有失效边界正确丢弃；
- `PASSWORD_SET` Settings 失败时旧凭据仍可用；
- 密码变更成功后旧凭据和旧会话立即失效；
- 丢失最终 `PASSWORD_SET` ACK 后可通过 `AUTH_INFO` 的 salt 恢复；
- settings reset 后回到 `OPEN`，不存在协议内清除密码路径；
- GUI 不记录密码、`K`、proof 或凭据对象；
- v1 客户端无法对 v2 固件执行宏管理操作。
