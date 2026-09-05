# Runtime Macro Development Plan

This document records the public project scope and implementation status. It does
not contain machine-specific build paths, device identifiers, or private test
data.

## Status

- Phases 1–5 are implemented and committed, including the v2-authenticated
  reference Python client.
- Phase 6 hardware validation is in progress; physical password authentication
  has not yet been verified.
- One compatible central device has completed a real HID protocol round trip
  and slot read/write check.
- Physical key output, reboot/NVS retention, and portability across other boards
  and host backends still need validation. Firmware USB reconnect/reset lifecycle
  handling is implemented; hardware rediscovery still needs validation.

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

- Fixed 32-byte v2 `LIST`, `GET`, `SET`, and `CLEAR` protocol; v1 management
  requests are rejected with `BAD_VERSION`.
- Optional authenticated `AUTH_INFO`, challenge/proof login, password replacement,
  and `LOCK` management commands; `OPEN` remains usable without a password.
- 22-byte payload chunks, atomic SET staging, and 52-byte `PASSWORD_SET` staging.
- Optional vendor USB HID transport (HID_1 by default) using Usage Page `0xff60` and Usage `0x61`.
- HID_0 remains the normal ZMK keyboard interface.
- Interrupt OUT remains disabled globally so HID_0 is not changed.
- Central-only USB build, NVS build, transport-off build, and Studio CDC ACM
  coexistence build coverage.
- Every USB connection-state notification conservatively clears sessions,
  protocol staging, and queued requests; only an HID notification brings the
  transport online, so reconnect or suspend/resume may require reauthentication.
  IN buffer permits are recycled separately only at known endpoint-loss/configuration
  boundaries, not for suspend/resume or unknown/error statuses. Unstable raw-status
  sampling keeps the transport offline until a stable notification.

### Phase 5: Python client and authentication CLI

- v2 `hidapi` client with `auth-info`, `login`, `set-password`, `lock`,
  `list`, `get`, `set`, and `clear`.
- `AuthInfo` API with strict OPEN/PROTECTED metadata validation.
- NFC/UTF-8 password handling, PBKDF2-HMAC-SHA256 derivation, and truncated
  HMAC challenge-response proofs.
- Password setup/replacement with 52-byte 22/22/8 chunking, salt confirmation
  after final-ACK loss, and no credential persistence.
- Explicit path, VID/PID filtering, fixed-frame validation, pagination, 32/33-byte
  report compatibility, stale-response handling, and transaction restart logic.
- Fake-HID tests covering authentication, retries, v1 rejection, and CLI input.

## Phase 6: Hardware validation

### Completed checks

- Enumerate a real compatible runtime macro HID interface.
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

- A graphical or background client may reuse the completed Python client's v2
  authentication flow and must implement [`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md).
- v1 is retained as a historical data-format reference, but current firmware
  and the reference client use only v2 management frames; there is no fallback.
- OS credential-store integration, Unicode, new ZMK Studio RPC messages,
  dynamic layout detection, and a multi-macro queue require separate design
  decisions.

## Explicit non-goals

- No Unicode, Chinese text, or Emoji support in the current protocol.
- No modifications to the ZMK main repository or ZMK Studio protobuf schema.
- The protocol does not encrypt USB traffic; authentication protects management
  operations when a password is configured.
- No concurrent macro queue.
