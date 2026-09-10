# ZMK Runtime Macro Development Plan

This document records the public project scope, implementation status, and the
implemented RAM-only dynamic macro feature.

The document does not contain machine-specific build paths, device identifiers,
private test data, or credentials.

## Status

- Static phases 1–5 are implemented and committed, including the
  v2-authenticated reference Python client.
- The RAM-only dynamic macro is implemented through the multislot D0–D5 stages:
  up to 8 independent RAM slots, 512-byte store/executor capacity, parameterized
  behavior, dynamic protocol v2, lifecycle clear policies, and the reference
  Python client/CLI with `--slot`/`--all` and no readback. The current multislot
  record is [`DYNAMIC_MULTISLOT_PLAN.md`](DYNAMIC_MULTISLOT_PLAN.md).
- Dynamic phase 8 (integration, hardware validation, and final documentation) is
  only partly complete. Host C, Python/`py_compile`/Ruff, and the current dongle
  build have passed. The current Totem dongle build is Flash `432468 B`, RAM
  `198654 / 262144 B` (remaining `63490 B`).
- The dongle has completed real v2 capability and CLI management checks: 8 slots,
  512-byte boundary upload, per-slot clear, clear-all iteration, and local slot
  validation. Physical key execution/consume, busy, TTL, lifecycle, USB-A to
  Bluetooth-B output, full static/auth regression, desktop integration, and the
  final release/readiness review remain unverified.
- The configuration repository currently uses the requested single
  `leen_totem.keymap`; this D6 record only claims the dongle build, not split
  peripheral builds.
- Physical password authentication, physical key output, reboot/NVS retention,
  portability across other boards and host backends, and complete hardware
  lifecycle validation still need verification.

## Current architecture

The static feature is a Flash-backed runtime macro system:

```text
static macro slot
    -> RAM cache
    -> Zephyr Settings/NVS persistence
    -> existing runtime macro executor
    -> ZMK keycode event pipeline
    -> normal keyboard HID output
```

The current RAM-only dynamic macro implementation provides up to eight independent
512-byte temporary text slots in volatile RAM, uses one shared executor and one
shared upload staging buffer, and is written through the dynamic commands on the
same management transport. Its state and commands are separate from the static
slot store; see [`DYNAMIC_MULTISLOT_PLAN.md`](DYNAMIC_MULTISLOT_PLAN.md) and
[`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md).

The optional management transport is a dedicated vendor USB HID interface,
defaulting to `HID_1`. ZMK's normal keyboard HID remains `HID_0`.

The current executor is a single delayable-work state machine. Static and
dynamic macro execution share this executor; a second macro is rejected with
`-EBUSY` and is not queued.

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
- Optional vendor USB HID transport (HID_1 by default) using Usage Page `0xff60`
  and Usage `0x61`.
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
- Complete the dynamic phase 8 physical workflows and desktop integration in
  [`DYNAMIC_MACRO_PLAN.md`](DYNAMIC_MACRO_PLAN.md) sections 13.2 and 13.5.

# RAM-only dynamic macro (current multislot delivery)

The original single-slot/256-byte sections below are historical v1 planning records.
The current implementation and stage status are maintained in
[`DYNAMIC_MULTISLOT_PLAN.md`](DYNAMIC_MULTISLOT_PLAN.md).

## Goal

The dynamic macro provides one temporary, RAM-only macro object that can be
written by a background service and executed by a physical keyboard behavior:

```text
background service
    -> dynamic macro protocol
    -> RAM-only pending text
    -> &runtime_macro_dynamic key binding
    -> existing runtime macro executor
    -> ZMK keycode event pipeline
    -> selected USB/Bluetooth keyboard output
```

The dynamic macro feature is intended for **non-secret temporary text only**:

- writing temporary text, switching the keyboard's Bluetooth profile/output,
  and then typing the text into another host device;
- typing short temporary text produced by a local background service.

The dynamic management channel is unencrypted and is not covered by the static
password/authentication gate, so passwords, PINs, OTP/verification codes,
tokens, API keys, and other credentials must not be sent through it. See
[`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md) section 7 and the security boundary
section below.

All output continues to be generated as normal ZMK keycode events. The feature
does not construct raw keyboard HID reports and does not create a second output
worker.

## Historical v1 product decisions

| Item | First implementation decision |
|---|---|
| Dynamic macro count | Exactly one |
| Maximum text length | 256 bytes |
| Text encoding | Existing printable US ASCII plus LF, Tab, Backspace |
| Storage | Volatile RAM only; no Settings/NVS calls |
| Readback | No `GET`; no `LIST` entry |
| Output trigger | Physical `&runtime_macro_dynamic` behavior |
| Protocol execute command | Not required in the first version |
| TTL | Optional per transaction; default 5 minutes when omitted |
| TTL start | After complete text commit |
| Concurrent execution | None; share the existing single executor |
| Successful execution | Consume and clear the dynamic text |
| Explicit clear | Clear the dynamic text and incomplete upload |
| Device restart/power loss | Clear through volatile RAM initialization |
| Bluetooth profile/output switch | Preserve by default; optional clear policy |
| Management USB disconnect | Clear by default when an actual disconnect is detected |
| Host shutdown while USB remains powered | Not guaranteed to be detectable |

A single dynamic macro is sufficient for the current non-secret temporary-text
and cross-Bluetooth-device transfer use cases. A new complete upload replaces
the previous committed text atomically. An incomplete or invalid upload must
not destroy the previous committed text unless an explicit clear is requested.

## Bluetooth and USB routing model

ZMK can keep USB and Bluetooth connections active at the same time. Bluetooth
profile selection and keyboard output selection are separate concepts:

```text
BT_SEL / BT_NXT  -> select the active Bluetooth profile
OUT_BLE          -> prefer Bluetooth for normal keyboard output
```

When the management USB connection to host A remains active and the keyboard
output is selected for Bluetooth host B, the background service can still write
the dynamic buffer through the management HID interface. Pressing
`&runtime_macro_dynamic` then sends the generated key events to B.

The dynamic implementation must not treat a Bluetooth profile change or an
`OUT_USB`/`OUT_BLE` preference change as a USB management disconnect.

References:

- [ZMK Output Selection Behavior](https://zmk.dev/docs/keymaps/behaviors/outputs)
- [ZMK Bluetooth Behavior](https://zmk.dev/docs/keymaps/behaviors/bluetooth)

## Keymap behavior

The implemented behavior takes no slot parameter:

```dts
#include <behaviors/runtime_macro.dtsi>

&runtime_macro_dynamic
```

Behavior semantics:

- If no committed dynamic text exists, the press is a no-op or returns the
  module's empty-buffer result without producing output.
- If the shared executor is busy, the behavior returns `-EBUSY` and does not
  consume the dynamic text.
- If the executor accepts the snapshot, the dynamic buffer is consumed and
  cleared, while the executor finishes the already accepted snapshot.
- Press and release handling follows the existing runtime macro behavior
  convention; the behavior itself does not directly emit HID reports.

The selected ZMK output destination determines whether the generated key events
reach USB or the active Bluetooth profile.

## Protocol surface

The dynamic protocol uses the existing fixed 32-byte frame and 22-byte payload.
[`DYNAMIC_PROTOCOL.md`](DYNAMIC_PROTOCOL.md) is the frozen, authoritative wire
contract; the summary below is a non-normative overview of the implemented
command set:

```text
DYNAMIC_BEGIN
DYNAMIC_DATA
DYNAMIC_CLEAR
```

There is intentionally no `DYNAMIC_GET`, `DYNAMIC_LIST`, or protocol-triggered
`DYNAMIC_EXECUTE` in the first version.

### `DYNAMIC_BEGIN`

- Starts or replaces an incomplete upload transaction.
- Declares the total text length.
- Uses the fixed single dynamic object rather than a static slot.
- May carry an optional TTL; an omitted TTL uses the 5-minute default.
- Does not modify the committed dynamic text until the final data chunk arrives.

### `DYNAMIC_DATA`

- Uses contiguous offsets and the same request ID and total length as
  `DYNAMIC_BEGIN`.
- Validates every byte with the existing ASCII-to-keycode mapping.
- The final chunk atomically replaces the committed dynamic text.
- Responses contain status/offset metadata only; they never contain text.

### `DYNAMIC_CLEAR`

- Clears the committed text, incomplete staging, and TTL state.
- Is idempotent.
- Has no effect on static slots, Settings, authentication credentials, or
  static macro contents.

The dynamic commands must use a separate protocol branch from the static
`LIST/GET/SET/CLEAR` branch. They must not pass through the static slot setter
or static management authorization gate.

## Storage and API boundary

The dynamic implementation must use separate state and functions from the
Flash-backed static slot store.

It must not call:

```c
zmk_runtime_macro_slot_set()
zmk_runtime_macro_slot_clear()
settings_save_one()
settings_delete()
```

The proposed state consists of:

- one committed dynamic text buffer;
- one bounded upload staging buffer;
- current length and validity state;
- TTL deadline;
- one transaction request ID and received offset;
- mutex/serialization state as required by the protocol consumer.

The executor receives a snapshot through an internal API. No public dynamic
read API should expose the buffer to another module or transport.

## Lifecycle and clear policy

### Always clear

- power-on/reset initialization;
- successful execution acceptance by `&runtime_macro_dynamic`;
- TTL expiration;
- explicit `DYNAMIC_CLEAR`;
- invalidation of an incomplete transaction;
- a detected management USB disconnect when the option is enabled.

### Optional clear

The following should be independently configurable or implemented behind one
clearly documented output-change policy:

- Bluetooth profile selection change;
- USB/Bluetooth output preference change;
- broader USB lifecycle reset or suspend handling.

The default output-change policy is **preserve**, because the intended workflow
writes the text before switching to Bluetooth host B.

The default management USB disconnect policy is **clear** for an actual
transport disconnect. This must not be documented as reliable detection of a
host operating-system shutdown. A host that powers down its USB data function
while continuing to provide VBUS may generate suspend, reset, no visible
change, or a true disconnect depending on the host/controller behavior.

The existing authenticated transport may reset authentication on conservative
USB lifecycle notifications. Dynamic clearing must be a separate policy and
must not automatically inherit every authentication reset event.

## Memory and execution limits

The hard limit is 256 bytes per dynamic text object. At the existing ASCII
executor timing defaults (`TAP_MS=30`, `WAIT_MS=15`), a 256-byte input may take
roughly 11–12 seconds to emit. This is acceptable for a hard maximum but should
be considered in TTL and user-interface feedback.

The first version must not queue dynamic executions. Static and dynamic macros
share the same executor and therefore have one global in-flight execution.

No sophisticated token-bucket rate limiter is required initially. Bounded frame
size, bounded text length, one staging transaction, the existing HID queue, and
single-executor `-EBUSY` behavior are sufficient for the current use cases.

## Historical v1 extension note

This section is a historical v1 extension record. The agreed follow-up scope has
now been implemented through D0–D5: up to 8 independent slots of 512 bytes each,
with a shared staging buffer and one executor. Each slot has its own committed
buffer, length, validity state, TTL, and consumption/keep policy; lifecycle events
clear all slots, while execution clears only the selected slot. See
[`DYNAMIC_MULTISLOT_PLAN.md`](DYNAMIC_MULTISLOT_PLAN.md).

The following remain out of scope: multiple simultaneous uploads, an execution
queue, concurrent output, dynamic `LIST/GET`, and dynamic readback.

## Security boundary

The first version deliberately accepts the following boundary:

- any local program with access to the management HID interface may attempt to
  write or clear the dynamic buffer;
- any local program with sufficient HID access may potentially cause keyboard
  input through the dynamic behavior path or other host-side mechanisms;
- the firmware cannot identify which desktop process originated a HID request;
- USB transport is not encrypted;
- text typed into the target host is observable by that host and its software.

The dynamic feature is intended to prevent Flash wear, persistent device
storage, and readback through the macro management protocol. It is not a secure
secret-entry channel and provides no application identity authentication:

- dynamic commands are processed identically in `OPEN`, `PROTECTED`, and
  `ERROR_LOCKED` states; they do not pass the static management authorization
  gate and never refresh an authenticated session;
- the dynamic channel must therefore be used only for non-secret temporary text;
  do not send passwords, PINs, OTP/verification codes, tokens, API keys, or any
  other credential through it;
- a separate authenticated and encrypted channel is required for secrets and is
  intentionally out of scope for this implementation phase.

## Implementation phases for dynamic macro

Dynamic phases 1–7 below are implemented and committed; phase 8 (integration,
hardware validation, and final documentation) is in progress. The authoritative
sequence, gates, and completion records are in
[`DYNAMIC_MACRO_PLAN.md`](DYNAMIC_MACRO_PLAN.md).

### Dynamic Phase 1: Core RAM store

- Add a dedicated dynamic state implementation.
- Add bounded committed and staging buffers.
- Add ASCII validation, atomic commit, TTL deadline, clear, and zeroization.
- Ensure no Settings/NVS path is reachable from dynamic writes.
- Add internal snapshot/consume support for the existing executor.

### Dynamic Phase 2: Protocol

- Add `DYNAMIC_BEGIN`, `DYNAMIC_DATA`, and `DYNAMIC_CLEAR`.
- Preserve the existing static v2 protocol behavior unchanged.
- Enforce one transaction, contiguous offsets, fixed maximum length, and no
  dynamic readback.
- Add capability advertisement or another explicit way for a client to detect
  dynamic support.

### Dynamic Phase 3: Keymap behavior

- Add `&runtime_macro_dynamic` to the existing behavior include surface.
- Route execution through the existing executor.
- Verify empty-buffer, busy, invalid-text, snapshot, and consume semantics.

### Dynamic Phase 4: Lifecycle integration

- Clear on boot/reset.
- Add configurable output/profile-switch clearing, default disabled.
- Add management USB disconnect clearing, default enabled for actual disconnect
  events.
- Keep USB suspend/host-shutdown ambiguity documented and avoid treating it as
  a reliable shutdown signal.

### Dynamic Phase 5: Tests and documentation

- Add host tests for the dynamic store, protocol, behavior, lifecycle, TTL, and
  executor interaction.
- Add Python client/library support for upload and clear, without readback.
- Add keymap and protocol documentation.
- Add hardware validation for USB A management plus Bluetooth B output.

Any further dynamic work (see the backlog at the end of
[`DYNAMIC_MACRO_PLAN.md`](DYNAMIC_MACRO_PLAN.md)) requires the same review and
approval sequence before implementation starts.

## Required dynamic tests

### Storage isolation

- Dynamic writes never call Settings save/delete.
- Dynamic data is absent after reboot.
- Static `LIST` and `GET` never expose dynamic data.
- Static `SET` and `CLEAR` cannot affect dynamic data.
- Dynamic clear cannot affect static slots or authentication credentials.

### Protocol

- Valid multi-chunk upload commits only after the final chunk.
- Incomplete, out-of-order, duplicated, oversized, and invalid-text uploads are
  rejected and cleaned up correctly.
- A failed replacement leaves the previous committed text unchanged.
- Dynamic responses never contain dynamic text.
- Dynamic commands do not refresh the static authentication session.

### Behavior and executor

- `&runtime_macro_dynamic` emits through the existing keycode event pipeline.
- Output uses the selected USB/Bluetooth destination.
- Successful execution consumes the dynamic buffer.
- Empty buffer is harmless.
- Static and dynamic execution share busy behavior and never queue.
- Executor-owned dynamic snapshots are cleared after completion.

### Lifecycle

- TTL clears pending dynamic text at the expected deadline.
- Explicit clear is idempotent.
- Reset/reboot clears dynamic state.
- Actual management USB disconnect clears when enabled.
- Output/profile switching preserves the buffer by default.
- Optional output/profile clearing works when enabled.
- USB suspend or host shutdown is not incorrectly treated as a guaranteed
  disconnect.

### Hardware workflow

- Host A remains connected by USB for management.
- The keyboard selects Bluetooth profile B and `OUT_BLE`.
- Host A uploads a dynamic text.
- `&runtime_macro_dynamic` types the text into B.
- Host A remains usable as the management channel.
- Power/reset and configured lifecycle boundaries clear the text.

## Future client work

- A graphical or background client may reuse the completed Python client's v2
  authentication flow and must implement [`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md).
- Dynamic client APIs should expose upload and clear operations but not dynamic
  readback.
- OS credential-store integration, Unicode, new ZMK Studio RPC messages,
  dynamic layout detection, and a multi-macro queue require separate design
  decisions.

## Explicit non-goals

- No Unicode, Chinese text, or Emoji support in the current protocol.
- No modifications to the ZMK main repository or ZMK Studio protobuf schema.
- The protocol does not encrypt USB traffic.
- No dynamic `GET` or `LIST` operation.
- No sensitive/authenticated dynamic mode in the first implementation.
- No concurrent macro queue.
- No multiple dynamic slots in the first implementation.
- No reliable firmware-level interpretation of host operating-system shutdown.
