# ZMK Runtime Macro Development Plan

This document records the public project scope, implementation status, and the
planned RAM-only dynamic macro feature. The dynamic macro feature described in
this document is **planned only**: it has not been implemented, tested, or
committed as firmware code.

The document does not contain machine-specific build paths, device identifiers,
private test data, or credentials.

## Status

- Phases 1–5 are implemented and committed, including the v2-authenticated
  reference Python client.
- Phase 6 hardware validation is in progress; physical password authentication
  has not yet been verified.
- One compatible central device has completed a real HID protocol round trip
  and slot read/write check.
- The RAM-only dynamic macro design is agreed at the product level but is not
  implemented.
- Physical key output, reboot/NVS retention, portability across other boards
  and host backends, and complete hardware lifecycle validation still need
  verification.

## Current architecture

The current feature is a Flash-backed runtime macro system:

```text
static macro slot
    -> RAM cache
    -> Zephyr Settings/NVS persistence
    -> existing runtime macro executor
    -> ZMK keycode event pipeline
    -> normal keyboard HID output
```

The optional management transport is a dedicated vendor USB HID interface,
defaulting to `HID_1`. ZMK's normal keyboard HID remains `HID_0`.

The current executor is a single delayable-work state machine. Static macro
execution and any future dynamic macro execution must share this executor; a
second macro is rejected with `-EBUSY` and is not queued.

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

# Planned feature: RAM-only dynamic macro

## Goal

Add one temporary, RAM-only macro object that can be written by a background
service and executed by a physical keyboard behavior:

```text
background service
    -> dynamic macro protocol
    -> RAM-only pending text
    -> &runtime_macro_dynamic key binding
    -> existing runtime macro executor
    -> ZMK keycode event pipeline
    -> selected USB/Bluetooth keyboard output
```

The dynamic macro feature is intended for:

- injecting one-time OTP or verification codes;
- injecting a code received from a phone or another live source;
- writing temporary text, switching the keyboard's Bluetooth profile/output,
  and then typing the text into another host device.

All output must continue to be generated as normal ZMK keycode events. The
feature must not construct raw keyboard HID reports or create a second output
worker.

## Product decisions for the first implementation

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

A single dynamic macro is sufficient for the current OTP, phone-code, and
cross-Bluetooth-device text-transfer use cases. A new complete upload replaces
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

## Proposed keymap behavior

The first version adds a behavior without a slot parameter:

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

## Proposed protocol surface

The dynamic protocol uses the existing fixed 32-byte frame and 22-byte payload.
The exact opcode values and capability advertisement are to be finalized during
implementation, but the first-version command set is:

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

## Multiple dynamic macros: future extension

The first implementation supports exactly one dynamic macro. If future use
cases require an OTP and a separate preloaded text at the same time, the design
can be extended without changing the basic executor model:

```dts
&runtime_macro_dynamic 0
&runtime_macro_dynamic 1
```

A future extension may add a separate configuration such as:

```text
CONFIG_ZMK_RUNTIME_MACRO_DYNAMIC_SLOT_COUNT
```

A practical future hard limit would be 4 slots. The extension should still
keep only one upload staging transaction and one executor. Each slot would have
its own committed buffer, length, validity state, and TTL; lifecycle events
would clear all slots, while execution would clear only the selected slot.

Supporting multiple simultaneous uploads, an execution queue, concurrent output,
or dynamic `LIST/GET` would be a separate larger feature and is not part of this
plan.

## Security boundary

The first version deliberately accepts the following boundary:

- any local program with access to the management HID interface may attempt to
  write or clear the dynamic buffer;
- any local program with sufficient HID access may potentially cause keyboard
  input through the dynamic behavior path or other host-side mechanisms;
- the firmware cannot identify which desktop process originated a HID request;
- USB transport is not encrypted;
- text typed into the target host is observable by that host and its software.

The feature is intended to prevent Flash wear, persistent device storage, and
readback through the macro management protocol. It is not intended to provide a
secure secret-entry channel or application identity authentication.

A separate authenticated/sensitive dynamic mode is intentionally out of scope
for this implementation phase.

## Implementation phases for dynamic macro

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

No dynamic firmware implementation should be started until this plan and the
wire-level details are reviewed and approved.

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
