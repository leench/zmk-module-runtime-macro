# ZMK Runtime Macro

[简体中文](README.zh-CN.md) · [Desktop application](https://github.com/leench/zmk-runtime-macro-desktop)

A Zephyr/ZMK external module that stores ASCII text in runtime macro slots and
emits the text as keyboard events when a `&runtime_macro <slot>` behavior is
triggered. The module does not require changes to the ZMK main repository. For a desktop
management application, see [zmk-runtime-macro-desktop](https://github.com/leench/zmk-runtime-macro-desktop).

## Features

- Fixed-size runtime macro slots.
- Zephyr Settings persistence, with NVS support on boards that provide a valid
  storage partition.
- Printable US ASCII (`0x20`–`0x7e`), LF, Tab, and Backspace.
- Normal ZMK keycode event pipeline instead of direct keyboard HID report
  construction.
- One delayable-work executor; a second trigger while a macro is running
  returns `-EBUSY` and is not queued.
- Optional module-owned USB HID configuration interface on HID_1 by default; the device can be changed when another module already owns it.
- Python `hidapi` client with v2 `auth-info`, `login`, `set-password`, `lock`,
  `list`, `get`, `set`, and `clear` commands.
- On first initialization, slots without persisted values receive defaults such as
  `Runtime Macro 1` and `Runtime Macro 2`. Existing values are preserved, and a
  cleared slot remains empty after reboot.

Unicode, CJK text, Emoji, automatic keyboard-layout conversion, and concurrent
macro queues are intentionally out of scope.

## Security notice

The current USB HID management channel is the authenticated v2 protocol. A new
credential leaves the device in `OPEN`, where management remains available
without a password. After a non-empty password is configured, `LIST`, `GET`,
`SET`, and `CLEAR` require a challenge-response login window. The protocol does
not provide a command to clear a password; recovery requires a settings-reset
firmware, which also clears Settings data such as macro slots.

Authentication does not encrypt USB traffic and does not protect text emitted
as keyboard input when a macro is triggered. Use the password mode when local
HID access must be restricted, and keep the USB interface disabled on hosts that
should have no management access. See [`docs/CLI.md`](docs/CLI.md) and
[`docs/AUTHENTICATION_PROTOCOL.md`](docs/AUTHENTICATION_PROTOCOL.md) for the
client flow and threat model.

## Add the module to a ZMK build

Pass this repository as a ZMK extra module. For example:

```sh
west build -b <board> -- \
  -DZMK_CONFIG=/path/to/your/zmk-config \
  -DZMK_EXTRA_MODULES=/path/to/zmk-module-runtime-macro
```

When several external modules are required, pass all module paths using the
CMake syntax expected by your build environment. No ZMK main-repository patch
is required.

## Keymap configuration

Include the behavior DTS file explicitly in the keymap:

```dts
#include <behaviors/runtime_macro.dtsi>
```

Bind slots as follows:

```dts
&runtime_macro 0
&runtime_macro 1
```

Slot numbering starts at `0` and is limited by
`CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT`. In a wireless split keyboard, the split
central owns the behavior, slots, Settings/NVS data, and USB configuration
interface; peripherals continue to report key positions normally.

## Kconfig

The core behavior requires Settings:

```conf
CONFIG_SETTINGS=y
CONFIG_ZMK_RUNTIME_MACRO_SLOT_COUNT=8
CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN=64
```

For NVS persistence, the board must provide a valid storage partition and the
corresponding flash configuration, for example:

```conf
CONFIG_SETTINGS_NVS=y
CONFIG_NVS=y
CONFIG_FLASH_MAP=y
```

Enable the optional USB HID configuration interface with:

```conf
CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y
```

This option is available for USB-enabled unibody or split-central builds. By
default it uses HID_1 and leaves ZMK's keyboard HID_0 unchanged. Set
`CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE` to another unused HID device when
another module already owns HID_1; that configuration must also increase
`CONFIG_USB_HID_DEVICE_COUNT` as needed. The transport requires a 32-byte
interrupt-IN endpoint. Do not enable `CONFIG_ENABLE_HID_INT_OUT_EP`; that
global option also changes HID_0.

For example, when a display Raw HID transport already owns HID_1:

```conf
CONFIG_USB_HID_DEVICE_COUNT=3
CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE="HID_2"
```

Execution timing uses runtime-macro-specific settings. They follow ZMK's
corresponding global macro settings by default:

```conf
CONFIG_ZMK_RUNTIME_MACRO_TAP_MS=30
CONFIG_ZMK_RUNTIME_MACRO_WAIT_MS=15
```

`TAP_MS` is the time each character remains pressed, and `WAIT_MS` is the delay
from release to the next character. These are build-time settings, not
per-slot or Python-client settings. Set both to `0` when matching a keymap whose
static macros explicitly use `tap-ms = <0>` and `wait-ms = <0>`. The global
`CONFIG_ZMK_MACRO_DEFAULT_*` values are not changed by these module settings.

## Python client

The reference client requires Python 3 and the PyPI `hidapi` package:

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
```

The complete command reference, device selection rules, Python module API,
wire behavior, error handling, and Linux permissions are documented in
[`docs/CLI.md`](docs/CLI.md). The fixed 32-byte protocol and USB HID mapping
are documented in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

Quick examples:

```sh
python3 tools/runtime_macro_cli.py auth-info
# Optional on an OPEN device; sets a non-empty password and re-authenticates.
python3 tools/runtime_macro_cli.py set-password
# Use login before management commands on a PROTECTED device.
python3 tools/runtime_macro_cli.py login
python3 tools/runtime_macro_cli.py lock
python3 tools/runtime_macro_cli.py list
python3 tools/runtime_macro_cli.py get 0
python3 tools/runtime_macro_cli.py get 0 --raw > slot-0.txt
python3 tools/runtime_macro_cli.py set 0 --text 'Hello'
printf 'Hello\n' | python3 tools/runtime_macro_cli.py set 0 --stdin
python3 tools/runtime_macro_cli.py set 0 --file slot-0.txt
python3 tools/runtime_macro_cli.py clear 0
```

Global options come before the subcommand. When more than one compatible
interface is connected, select one explicitly:

```sh
python3 tools/runtime_macro_cli.py --path "$HID_PATH" list
```

`--text` is literal and does not interpret backslash escapes. Use stdin, a
file, or shell ANSI-C quoting when LF, Tab, or Backspace is needed. Some Linux
`hidapi` backends omit parsed Usage Page/Usage metadata; in that case the
client requires an explicit `--path` rather than guessing a device. Do not
commit real paths, serial numbers, or host-specific device information.

## Protocol and C interfaces

- Wire protocol: [`docs/PROTOCOL.md`](docs/PROTOCOL.md)
- Python CLI and Python module API: [`docs/CLI.md`](docs/CLI.md)
- Runtime macro C API: [`include/zmk/runtime_macro.h`](include/zmk/runtime_macro.h)
- Protocol C API: [`include/zmk/runtime_macro_protocol.h`](include/zmk/runtime_macro_protocol.h)

The current v2 protocol provides `LIST`, `GET`, `SET`, and `CLEAR`, plus
`AUTH_INFO`, `AUTH_CHALLENGE`, `AUTH_PROVE`, `PASSWORD_SET`, and `LOCK`.
Long macro text uses 22-byte chunks; a slot is not changed until a complete
`SET` transaction has been received. Password credentials use the separate
52-byte chunked `PASSWORD_SET` transaction.

## Tests

```sh
./tests/host/run.sh
python3 -m unittest discover -s tests/python -v
python3 -m py_compile tools/runtime_macro_cli.py
```

Host tests cover slot and Settings logic, ASCII mapping, asynchronous
execution, the protocol core, and USB transport stubs. Python tests use a fake
HID device. Target builds require the target ZMK workspace and its configured
containerized Zephyr environment.

## Status

The authentication core, v2 protocol/USB lifecycle integration, and reference
Python client are implemented and committed. One compatible central device has
completed a real unauthenticated HID protocol round trip and slot read/write
check. Physical password login, USB reconnect behavior, reboot/NVS retention,
physical key output, and portability across other boards and host backends still
require validation on target hardware. This status does not claim that physical
USB authentication has been verified or that every board or host platform has
been tested.

## Privacy

This is a public repository. Documentation and tests must not contain:

- local, workspace, or container absolute paths;
- USB serial numbers, real hidraw paths, or personal device identifiers;
- private repository URLs, access tokens, user data, or unpublished
  configuration.

Use placeholders for paths, VID/PID values, device names, and serial numbers in
examples. Keep real hardware details in local test records only.

## Roadmap

Historical phases and remaining work are listed in [`docs/PLAN.md`](docs/PLAN.md).
A future graphical or background client should reuse the documented v2 wire
protocol and authentication flow. Unicode, new ZMK Studio RPC messages, and
ZMK main-repository changes remain outside the current scope.
