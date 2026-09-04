# Python CLI 和接口

`tools/runtime_macro_cli.py` 是 runtime macro USB HID 配置接口的参考客户端。它通过
`hidapi` 连接固件的 HID_1，不依赖 ZMK Studio，也不会连接 ZMK 的键盘 HID_0。

该接口没有加密或认证机制。任何能够访问设备 HID interface 的本机进程都可能读取或修改
slots；不要在不可信的 USB 主机上启用该 transport。

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

`set` 会先在客户端校验输入，再按协议的 22-byte payload 分块发送。传输超时或事务状态
错误会从 offset `0` 重新开始；设备不会在完整 SET 事务完成前改变 slot。

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

正常情况下，客户端按 vendor Usage Page `0xff60`、Usage `0x61` 找到 HID_1。它会拒绝
多个匹配设备，并要求使用 `--path`，避免误操作其他设备。

部分 Linux `hidapi` 后端无法返回解析后的 Usage Page/Usage。这种情况下，客户端不会自动
猜测设备；请从系统枚举结果中获取 HID_1 的 path，并显式传入：

```sh
python3 tools/runtime_macro_cli.py --path "$HID_PATH" list
```

这里的 `$HID_PATH` 只应在本机 shell 中设置，不要把真实 path、序列号或设备拓扑提交到
公开仓库。

如果出现权限错误，应为实际设备创建最小权限的 udev 规则。下面只是假值示例：

```udev
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", MODE="0660", GROUP="plugdev"
```

重新加载规则并重新插拔设备后再测试。不要长期使用全局 `chmod 666`。

## 输出和退出码

- `0`：命令成功；
- `1`：设备、传输、协议、固件状态或输入错误；
- `2`：命令行参数解析错误。

固件状态会显示为名称和数值，例如 `BAD_SLOT (4)`、`INVALID_TEXT (7)` 或
`STORAGE_ERROR (8)`。客户端会校验 response 的 version、opcode、request ID、slot、
长度、offset、total length 和零填充；不匹配的 response 不会被当作成功结果。

## Python 调用接口

脚本也可以作为轻量 Python module 使用。下面的 API 适合自动化测试或编写其他本地工具：

```python
from tools.runtime_macro_cli import HidTransport, RuntimeMacroClient

# device 是 hid.device() 实例；必须由调用者先按设备 path 打开。
transport = HidTransport(device)
try:
    client = RuntimeMacroClient(transport, timeout_ms=1000, retries=2)
    slots = client.list_slots()       # list[SlotInfo]
    text = client.get_slot(0)         # bytes
    client.set_slot(0, b"Hello\n")
    client.clear_slot(0)
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
| `list_slots()` | `list[SlotInfo]` | 返回每个 slot 的序号和 byte 长度 |
| `get_slot(slot)` | `bytes` | 获取一个 slot 的 ASCII/control bytes |
| `set_slot(slot, data)` | `None` | 校验并原子替换一个 slot |
| `clear_slot(slot)` | `None` | 清空并删除一个 slot |

`SlotInfo` 是包含 `slot: int` 和 `length: int` 的 dataclass。输入错误抛出
`ValueError`；设备/传输错误抛出 `DeviceError` 或 `TransportError`；格式错误抛出
`ProtocolError`；固件返回非零 status 时抛出 `RemoteError`，其 `.status` 保存原始状态码。

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

### 辅助函数

```python
build_frame(opcode, request_id, slot, *, payload=b"", offset=0, total_length=0) -> bytes
validate_ascii(data: bytes) -> None
find_device(hid_module, *, path=None, vid=None, pid=None) -> dict
open_transport(hid_module, *, path=None, vid=None, pid=None, clock=...) -> HidTransport
```

`build_frame()` 生成固定 32-byte、零填充的 v1 request；`validate_ascii()` 校验客户端支持的
输入范围；`find_device()` 负责设备筛选；`open_transport()` 负责筛选、打开和失败清理。
这些函数使用依赖注入参数，便于 fake HID 测试。

## Wire interface

Python 客户端使用的二进制接口是稳定的 v1 固定帧协议，详细字段、状态码、分页、SET 事务
和 USB HID descriptor 见 [`PROTOCOL.md`](PROTOCOL.md)。固件侧 C API 见：

- [`include/zmk/runtime_macro.h`](../include/zmk/runtime_macro.h)
- [`include/zmk/runtime_macro_protocol.h`](../include/zmk/runtime_macro_protocol.h)

## 本地测试

```sh
python3 -m unittest discover -s tests/python -v
python3 -m py_compile tools/runtime_macro_cli.py
```

测试使用 fake HID module/device，不需要真实 USB 设备，也不会修改真实 slots。
