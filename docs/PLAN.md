# Runtime Macro Development Plan

This document records the public project scope and implementation status. It does
not contain machine-specific build paths, device identifiers, or private test
data.

## Status

- Phases 1–5 are implemented and committed.
- Phase 6 hardware validation is in progress.
- One compatible central device has completed a real HID protocol round trip
  and slot read/write check.
- Physical key output, USB reconnect behavior, reboot/NVS retention, and
  portability across other boards and host backends still need validation.

## Completed phases

### Phase 1: Module skeleton and behavior

- Zephyr module metadata, CMake, Kconfig, DTS behavior, and binding.
- `&runtime_macro <slot>` keymap binding.
- Central-side behavior execution boundary for split builds.

### Phase 2: Settings/NVS storage

- Fixed-size RAM slots and `runtime_macro/slot/<n>` Settings keys.
- Public `get`, `get_length`, `copy`, `set`, and `clear` APIs.
- Printable ASCII/control-byte validation.
- RAM and Settings write serialization, including persistence-error ordering.

### Phase 3: ASCII executor

- Complete printable US ASCII mapping plus LF, Tab, and Backspace.
- One delayable-work press/release state machine.
- ZMK keycode event pipeline integration.
- Snapshot semantics, empty-slot handling, busy rejection, and error recovery.

### Phase 4: Protocol and optional USB HID transport

- Fixed 32-byte v1 `LIST`, `GET`, `SET`, and `CLEAR` protocol.
- 22-byte payload chunks and atomic SET staging.
- Optional vendor HID_1 transport using Usage Page `0xff60` and Usage `0x61`.
- HID_0 remains the normal ZMK keyboard interface.
- Interrupt OUT remains disabled globally so HID_0 is not changed.
- Central-only USB build, NVS build, transport-off build, and Studio CDC ACM
  coexistence build coverage.

### Phase 5: Python client

- `hidapi` client with `list`, `get`, `set`, and `clear`.
- Explicit path, VID/PID filtering, fixed-frame validation, pagination, and
  transaction restart handling.
- 32/33-byte report compatibility and stale-response deadline handling.
- Fake-HID tests and documented Python module API.

## Phase 6: Hardware validation

### Completed checks

- Enumerate a real compatible HID_1 interface.
- Receive a valid 32-byte `LIST` response.
- Write and read back multiple slots through the protocol.
- Confirm that a second connected interface is not selected implicitly.

### Remaining checks

- Flash and verify the intended split-central firmware on the target device.
- Trigger each bound slot from the physical keyboard.
- Verify letters, digits, punctuation, LF, Tab, and Backspace at the host.
- Power-cycle the central and confirm NVS retention.
- Unplug/replug USB and confirm HID rediscovery.
- Verify busy behavior when a macro is triggered while another is running.
- Test host permissions and device discovery on supported Linux/macOS/Windows
  environments where applicable.
- Measure practical timing and confirm display/UART behavior on the target
  hardware.

## Phase 7: Future client work

- Keep the v1 wire protocol stable.
- A graphical client may reuse the existing protocol.
- Unicode, new ZMK Studio RPC messages, dynamic layout detection, and a
  multi-macro queue require separate design decisions.

## Explicit non-goals

- No Unicode, Chinese text, or Emoji support in the current protocol.
- No modifications to the ZMK main repository or ZMK Studio protobuf schema.
- No authentication or encryption in the local USB configuration channel.
- No concurrent macro queue.
