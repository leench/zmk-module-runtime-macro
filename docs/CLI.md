# Python CLI 和接口

`tools/runtime_macro_cli.py` 是 runtime macro USB HID 配置接口的参考客户端。它通过
`hidapi` 连接固件的 runtime macro USB HID（默认是 HID_1），不依赖 ZMK Studio，也不会连接 ZMK 的键盘 HID_0。
当前客户端只发送 **v2** 固定 32-byte frame；不会自动回退到历史 v1。v2 固件在没有设置密码时保持
`OPEN`，设置密码后进入 `PROTECTED`。

## 安全模型

- `OPEN`：没有凭据。`list`、`get`、`set`、`clear` 可以直接使用，也可以一直不设置密码。
- `PROTECTED`：设置非空密码后，以上四个宏管理命令必须先 `login`。
- `set-password` 在 `OPEN` 可直接首次设置；在 `PROTECTED` 必须已有有效登录窗口。
- 客户端使用 `AUTH_INFO -> AUTH_CHALLENGE -> AUTH_PROVE` challenge-response，不传输原始密码。
- 密码经过 Unicode NFC 后以 UTF-8 编码，使用 PBKDF2-HMAC-SHA256 派生 32-byte key；proof 是
  `HMAC-SHA256("ZMK-RUNTIME-MACRO-AUTH-V2" || nonce)` 的前 16 bytes。
- 登录窗口默认在 5 分钟无成功管理操作后过期。`lock`、USB 生命周期事件和固件重启会使会话失效。
- 协议没有清除密码的命令。忘记密码只能使用 settings-reset 固件；这会清除 Settings 数据，包含宏
  slots 和密码记录。
- 客户端不保存密码或派生 key，不使用命令行参数、普通环境变量、普通配置文件或日志传递凭据。
  GUI/后台应用也应遵循 [`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md) 的凭据存储要求。
- 认证不加密 USB 流量；密码配置保护管理操作，不保护按键触发时的键盘输出。

## 安装

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
```

`tools/requirements.txt` 提供的 PyPI 包名是 `hidapi`，Python import 名是 `hid`。

## 命令行语法

```text
python3 tools/runtime_macro_cli.py [global-options] command [command-options]
```

全局选项必须写在子命令之前：

| 选项 | 说明 |
| --- | --- |
| `--path PATH` | 精确选择一个 HID path；多个设备时推荐使用 |
| `--vid VALUE` | 按十进制或 `0x` 十六进制 VID 过滤 |
| `--pid VALUE` | 按十进制或 `0x` 十六进制 PID 过滤 |
| `--timeout-ms N` | 每次响应等待时间，默认 `1000` |
| `--retries N` | 可恢复传输重试次数，默认 `2` |

密码命令没有密码参数。密码只能由终端 `getpass` 安全读取。

### `auth-info`

显示设备的认证状态、会话状态和 KDF iteration 数，不显示 salt、key 或密码：

```sh
python3 tools/runtime_macro_cli.py auth-info
```

示例：

```text
state=OPEN authenticated=no iterations=600000
state=PROTECTED authenticated=yes iterations=600000
```

`ERROR_LOCKED` 或无效凭据会显示为明确固件错误，绝不会被解释为 `OPEN`。

### `login`

在 `PROTECTED` 设备上输入密码并建立管理窗口：

```sh
python3 tools/runtime_macro_cli.py login
```

客户端每次登录都重新获取 `AUTH_INFO` 和 challenge。错误密码、过期/缺失 challenge、限速和协议错误
都会直接报告；`AUTH_PROVE` 超时不会重发旧 nonce 或旧 proof，而是从 `AUTH_INFO` 重新开始。
`OPEN` 设备不能登录，会提示先设置密码。

### `set-password`

首次设置或更换密码：

```sh
python3 tools/runtime_macro_cli.py set-password
```

命令会要求输入两次非空密码；规范化后的输入不一致或为空时，在任何 HID 请求前失败。

- `OPEN`：可以直接执行。
- `PROTECTED`：必须先在同一设备上运行 `login`，再运行 `set-password`；登录窗口由固件保存，
  因此可以使用两个短命令完成。
- 每次设置都会由客户端生成新的 16-byte CSPRNG salt，并按 22/22/8 bytes 发送 52-byte credential
  object（iterations LE32、salt、key），三个 chunk 使用同一 request ID。
- 成功后客户端重新读取 salt，并使用新密码登录确认；不会保存凭据。
- 中间 chunk 超时会以新的 request ID 从 offset 0 重启整笔事务。最终 ACK 超时只读取 `AUTH_INFO`
  比较 salt；匹配则认为已提交并继续确认，不匹配则失败，绝不盲目重发最终 chunk。

协议内不能将 `PROTECTED` 改回空密码。需要恢复 `OPEN` 时必须刷 settings-reset 固件。

### `lock`

关闭当前管理窗口并丢弃固件端的认证 challenge/协议 staging：

```sh
python3 tools/runtime_macro_cli.py lock
```

`LOCK` 是幂等操作，传输超时时可以用新的 request ID 重试。

### `list`

列出 slots 及其当前文本长度：

```sh
python3 tools/runtime_macro_cli.py list
python3 tools/runtime_macro_cli.py --path "$HID_PATH" list
```

输出为每行一个 slot：

```text
0\t12
1\t0
```

`PROTECTED` 状态下未登录时，固件会返回 `AUTH_REQUIRED`，客户端不会改用 v1 或其他旁路。

### `get SLOT`

读取一个 slot。默认输出会将控制字符转义，避免改变终端状态：

```sh
python3 tools/runtime_macro_cli.py get 0
```

使用 `--raw` 将原始 bytes 写到 stdout，适合保存或管道传输：

```sh
python3 tools/runtime_macro_cli.py get 0 --raw > slot-0.txt
```

### `set SLOT`

使用字面 ASCII 文本：

```sh
python3 tools/runtime_macro_cli.py set 0 --text 'Hello'
```

`--text` 不解释反斜杠。因此需要换行、Tab 或 Backspace 时，使用 stdin、文件或 shell 的
ANSI-C quoting：

```sh
printf 'Hello\n' | python3 tools/runtime_macro_cli.py set 0 --stdin
printf $'A\tB\bC' | python3 tools/runtime_macro_cli.py set 1 --stdin
python3 tools/runtime_macro_cli.py set 2 --file slot-2.txt
```

`set` 会先在客户端校验输入，再按协议的 22-byte payload 分块发送。传输超时或事务状态错误
会以新的 request ID 从 offset `0` 重新开始；设备不会在完整 SET 事务完成前改变 slot。

### `clear SLOT`

清空一个 slot：

```sh
python3 tools/runtime_macro_cli.py clear 0
```

## 输入限制

固件和客户端都允许：

- printable US ASCII：`0x20`–`0x7e`；
- LF：`0x0a`；
- Tab：`0x09`；
- Backspace：`0x08`。

不允许 NUL、DEL、UTF-8 多字节字符、中文、Emoji 或其他 Unicode。最大长度由固件的
`CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN` 决定，默认是 64 bytes。

宏按 US 键盘 usage 执行，而不是发送字符流；主机键盘布局可能影响标点最终产生的字符。

## 设备发现和 Linux 权限

正常情况下，客户端按 vendor Usage Page `0xff60`、Usage `0x61` 找到 runtime macro HID。它会拒绝
多个匹配设备，并要求使用 `--path`，避免误操作其他设备。如果固件通过
`CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE` 将 runtime macro transport 配置为 HID_2，而另一个模块也
暴露了相同 Usage，则必须使用 runtime macro HID 的精确 `--path`。

部分 Linux `hidapi` 后端无法返回解析后的 Usage Page/Usage。这种情况下，客户端不会自动猜测设备；
请从系统枚举结果中获取 runtime macro HID 的 path，并显式传入：

```sh
python3 tools/runtime_macro_cli.py --path "$HID_PATH" auth-info
```

这里的 `$HID_PATH` 只应在本机 shell 中设置，不要把真实 path、序列号或设备拓扑提交到公开仓库。

如果出现权限错误，应为实际设备创建最小权限的 udev 规则。下面只是假值示例：

```udev
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", GROUP="plugdev"
```

重新加载规则并重新插拔设备后再测试。不要长期使用全局 `chmod 666`。

## 输出和退出码

- `0`：命令成功；
- `1`：设备、传输、协议、固件状态或输入错误；
- `2`：命令行参数解析错误。

固件状态会显示为名称和数值，例如 `AUTH_REQUIRED (10)`、`AUTH_FAILED (11)`、
`RATE_LIMITED (13)`、`CREDENTIAL_INVALID (15)`、`BAD_SLOT (4)` 或 `STORAGE_ERROR (8)`。
收到 v2 `BAD_VERSION` 时客户端会明确提示“固件仍是 v1/需升级”，且不会回退到 v1。
客户端会校验 response 的 version、opcode、request ID、slot、长度、offset、total length 和零填充；
不匹配的 response 不会被当作成功结果。

## Python 调用接口

脚本也可以作为轻量 Python module 使用。下面的 API 适合自动化测试或编写其他本地工具：

```python
from tools.runtime_macro_cli import HidTransport, RuntimeMacroClient

# device 是 hid.device() 实例；必须由调用者先按设备 path 打开。
transport = HidTransport(device)
try:
    client = RuntimeMacroClient(transport, timeout_ms=1000, retries=2)
    info = client.auth_info()       # AuthInfo
    if info.open:
        client.set_password("输入密码")
    elif not info.authenticated:
        client.authenticate("输入密码")
    slots = client.list_slots()     # list[SlotInfo]
    text = client.get_slot(0)       # bytes
    client.set_slot(0, b"Hello\n")
    client.clear_slot(0)
    client.lock()
finally:
    transport.close()
```

### `RuntimeMacroClient`

```python
RuntimeMacroClient(
    transport: HidTransport,
    *,
    timeout_ms: int = 1000,
    retries: int = 2,
)
```

方法：

| 方法 | 返回值 | 行为 |
| --- | --- | --- |
| `auth_info()` | `AuthInfo` | 严格读取 OPEN/PROTECTED、会话、iterations 和 salt 元数据 |
| `authenticate(password)` | `AuthInfo` | 用 NFC/UTF-8、PBKDF2 和 challenge-response 建立会话 |
| `set_password(new_password, *, iterations=600000)` | `AuthInfo` | 设置/更换密码，确认 salt 后用新密码重新登录 |
| `lock()` | `None` | 发送 canonical LOCK，关闭管理窗口 |
| `list_slots()` | `list[SlotInfo]` | 返回每个 slot 的序号和 byte 长度 |
| `get_slot(slot)` | `bytes` | 获取一个 slot 的 ASCII/control bytes |
| `set_slot(slot, data)` | `None` | 校验并原子替换一个 slot |
| `clear_slot(slot)` | `None` | 清空并删除一个 slot |

`AuthInfo` 是 frozen dataclass，包含：

- `configured: bool`：是否为 `PROTECTED`；
- `authenticated: bool`：当前管理窗口是否有效；
- `iterations: int`：OPEN 时为默认值，PROTECTED 时为设备值；
- `salt: bytes`：OPEN 时为 16 个零字节，PROTECTED 时为设备 salt；
- `state`、`open`、`protected`：便于直白判断的只读属性。

辅助密码函数为：

```python
normalize_password(password: str) -> bytes
validate_iterations(iterations: int) -> None
validate_auth_parameters(iterations: int, salt: bytes) -> None
derive_key(password: str, salt: bytes, iterations: int = 600000) -> bytes
build_auth_proof(key: bytes, nonce: bytes) -> bytes
```

这些函数不会记录输入。`derive_key` 使用标准库 `hashlib.pbkdf2_hmac`；客户端拒绝空密码、范围外
iterations、全零 salt，并在设置密码时拒绝全零派生 key。

输入错误抛出 `ValueError`；设备/传输错误抛出 `DeviceError` 或 `TransportError`；格式错误抛出
`ProtocolError`；固件返回非零 status 时抛出 `RemoteError`，其 `.status` 保存原始状态码。
收到 v2 `BAD_VERSION` 时抛出 `LegacyFirmwareError`，不会执行 v1 fallback。

### `HidTransport`

```python
HidTransport(device, *, clock=time.monotonic)
```

主要方法：

- `write_frame(frame)`：发送一个 32-byte protocol frame；hidapi 写入时自动添加 report ID `0`；
- `read_frame(timeout_ms)`：读取并规范化 32-byte response；兼容 32-byte 和带前导零 report ID 的
  33-byte hidapi 返回值；
- `exchange(frame, timeout_ms)`：发送并等待匹配的 response，丢弃陈旧 request；
- `close()`：关闭底层 HID device；也支持 `with` 上下文管理器。

### 低层辅助函数

```python
build_frame(opcode, request_id, slot, *, payload=b"", offset=0, total_length=0) -> bytes
validate_ascii(data: bytes) -> None
find_device(hid_module, *, path=None, vid=None, pid=None) -> dict
open_transport(hid_module, *, path=None, vid=None, pid=None, clock=...) -> HidTransport
```

`build_frame()` 生成固定 32-byte、零填充的 v2 request；`validate_ascii()` 校验客户端支持的输入范围；
`find_device()` 负责设备筛选；`open_transport()` 负责筛选、打开和失败清理。这些函数使用依赖注入
参数，便于 fake HID 测试。

## Wire interface

Python 客户端使用的是当前 v2 固定帧协议。宏命令沿用 `LIST`、`GET`、`SET`、`CLEAR` 的分页和事务
布局，认证使用 `AUTH_INFO`、`AUTH_CHALLENGE`、`AUTH_PROVE`、`PASSWORD_SET`、`LOCK`。
详细字段、状态码、分页、SET 事务和 USB HID descriptor 见 [`PROTOCOL.md`](PROTOCOL.md) 与
[`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md)。固件侧 C API 见：

- [`include/zmk/runtime_macro.h`](../include/zmk/runtime_macro.h)
- [`include/zmk/runtime_macro_protocol.h`](../include/zmk/runtime_macro_protocol.h)

## v1 固件迁移

当前 v2 客户端首先发送 version `2` 的请求。v1 固件通常返回 `BAD_VERSION`；客户端会提示升级。
客户端不会在 `AUTH_REQUIRED`、`AUTH_FAILED`、传输超时或其他认证错误后尝试 v1，因为那会绕过保护。
升级 v1 固件到 v2 且保留旧 Settings 时，设备暂时处于 `OPEN`；宏可能仍存在，客户端会显示并建议
设置密码，但不会强制。参见认证协议的迁移说明。

## 本地测试

```sh
python3 -m unittest discover -s tests/python -v
python3 -m py_compile tools/runtime_macro_cli.py
```

测试使用 fake HID module/device，不需要真实 USB 设备，也不会修改真实 slots。实体设备的 USB 重连、
密码生命周期、NVS 保留和跨平台发现仍需按项目硬件验证计划确认。
