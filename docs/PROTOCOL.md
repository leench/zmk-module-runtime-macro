# Runtime Macro Protocol v1 (historical)

This document records the original 32-byte transport-independent runtime macro
protocol and its optional USB HID mapping. The current firmware implements the
same slot and chunk data semantics as **v2**, adds authenticated management
commands, and rejects v1 management requests with `BAD_VERSION`. See
[`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md) for the v2 wire and
authentication contract.

The protocol core receives complete frames and is called by one serialized work
consumer; it does not create a thread, queue, or USB device.

See [`CLI.md`](CLI.md) for the reference Python client's command-line and module APIs;
the existing client has not yet been upgraded to v2 authentication.

## Frame

每个 request 和 response 都严格为 32 bytes，不使用 CRC。USB HID（或其他定长
transport）负责提供 report 边界和可靠传输。

| Byte | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 1 | `version` | Historical v1 is `1`; current firmware uses v2 (`2`) |
| 1 | 1 | `opcode` | `LIST=1`, `GET=2`, `SET=3`, `CLEAR=4` |
| 2 | 1 | `request_id` | Opaque request identifier, echoed by the response |
| 3 | 1 | `status` | Request must be `0`; response contains a status code |
| 4 | 1 | `slot` | Slot index; `0xff` is used by `LIST` |
| 5 | 1 | `payload_length` | Number of payload bytes, `0..22` |
| 6..7 | 2 | `offset` | Little-endian byte offset |
| 8..9 | 2 | `total_length` | Little-endian logical object/transaction length |
| 10..31 | 22 | `payload` | Payload bytes; unused bytes must be zero |

Integer fields are unsigned little-endian. Responses are always initialized as a
complete zero-filled frame before fields are written, so unused payload bytes never
contain data from an earlier response.

Every response echoes the request's `version`, `opcode`, `request_id`, and `slot`,
including error responses. An error response has `payload_length=0`, `offset=0`,
`total_length=0`, and a zero-filled payload. The echoed version is the received
version even for `BAD_VERSION`.

A canonical request also zero-fills the part of the payload after
`payload_length`. Requests that contain non-zero unused payload bytes are rejected
with `BAD_REQUEST` (or `BAD_LENGTH` when the declared payload length itself is too
large for a `SET`).

## Opcodes (historical v1 set)

The following table is the v1 command set. In current firmware these four
opcodes use version `2` and are subject to the OPEN/PROTECTED authentication
gate documented in [`AUTHENTICATION_PROTOCOL.md`](AUTHENTICATION_PROTOCOL.md).

| Name | Value | Description |
| --- | ---: | --- |
| `LIST` | 1 | Read slot metadata |
| `GET` | 2 | Read one slot's text |
| `SET` | 3 | Replace one slot using one or more chunks |
| `CLEAR` | 4 | Clear one slot |

## Status codes

These values are stable wire values and are declared in
`include/zmk/runtime_macro_protocol.h`.

| Name | Value | Meaning |
| --- | ---: | --- |
| `OK` | 0 | Request accepted or operation completed |
| `BAD_VERSION` | 1 | `version` is not supported |
| `BAD_OPCODE` | 2 | `opcode` is unknown |
| `BAD_REQUEST` | 3 | Request fields or SET transaction metadata are invalid |
| `BAD_SLOT` | 4 | Slot is outside the configured slot range |
| `BAD_OFFSET` | 5 | Offset is outside the logical object or not the next SET offset |
| `BAD_LENGTH` | 6 | Payload/total length is invalid |
| `INVALID_TEXT` | 7 | Text contains a byte outside the supported US ASCII set |
| `STORAGE_ERROR` | 8 | Settings save/delete failed |
| `INTERNAL` | 9 | An unexpected module/API failure occurred |

A request with non-zero `status` is rejected as `BAD_REQUEST`. Unsupported
`version` is reported as `BAD_VERSION` before other request semantics are processed.

## LIST

A `LIST` request must have:

- `status=0`;
- `slot=0xff`;
- `payload_length=0`;
- `total_length=0`;
- zero-filled unused payload bytes.

The logical result is constructed as follows:

```text
byte 0       slot_count (uint8)
byte 1..2    slot 0 length (uint16 little-endian)
byte 3..4    slot 1 length
...
byte 1+2*n   slot n length
```

The logical result length is `1 + 2 * slot_count`. The request `offset` is the
logical result offset. At most 22 bytes are returned in a response. `offset` may
equal `total_length`, which returns an empty successful chunk; an offset greater
than the total returns `BAD_OFFSET`. A successful response sets `response.offset`
to the requested offset and `response.total_length` to the logical result length.

## GET

A `GET` request must have `status=0`, a valid slot, `payload_length=0`,
`total_length=0`, and zero-filled unused payload bytes. The request `offset` is an
offset into a synchronized snapshot of the slot text. At most 22 bytes are returned.
`offset` may equal the text length for an empty final chunk; an offset greater than
the length returns `BAD_OFFSET`.

A successful response contains the requested chunk, sets `response.offset` to the
request offset, and sets `response.total_length` to the snapshot's text length. The
protocol uses the slot store's safe copy API; it never returns the slot store's
internal RAM pointer.

## SET

`SET` requests have `status=0`, a valid slot, and `payload_length<=22`.
`total_length` must be no greater than `CONFIG_ZMK_RUNTIME_MACRO_MAX_TEXT_LEN`.
The range `[offset, offset + payload_length)` must fit within `total_length`; the
implementation checks this without an overflowing integer addition.

For a non-empty transaction, every chunk must have `payload_length>0`. The only
zero-length SET is the empty transaction with `offset=0`, `total_length=0`, and
`payload_length=0`; it is committed immediately. Every text byte is validated
against the module's supported US ASCII mapping before it is staged.

- A legal chunk with `offset=0` starts or replaces the context's current SET
  transaction. It records `request_id`, `slot`, and `total_length`.
- A later chunk must use the same `request_id`, `slot`, and `total_length`, and its
  `offset` must equal the number of bytes already received.
- A non-zero offset without a matching active transaction is `BAD_REQUEST` and
  clears staging. This includes retransmitting a final chunk after its successful
  completion when the host did not receive the final ACK; the client must restart
  at `offset=0`.
- A non-zero offset with matching metadata but an offset other than the next
  expected offset is `BAD_OFFSET` and clears staging. Therefore a repeated
  non-final chunk (whose offset was already processed) also requires a complete
  restart from `offset=0`.
- Any invalid SET chunk clears staging; the client must restart at `offset=0`.
  A new legal `offset=0` chunk may replace incomplete staging.
- The slot and NVS are not changed until the final chunk has been received. The
  final chunk causes exactly one call to `zmk_runtime_macro_slot_set()`.

SET responses never carry a payload. For a successful accepted chunk,
`response.offset` is the next expected offset and `response.total_length` echoes the
transaction total. The final successful response therefore has
`offset == total_length`. Error responses use the common zero offset/length error
form.

`runtime_macro_slot_set()` is RAM-first: it updates the in-memory slot before it
calls the Settings backend. If persistence fails, the response is
`STORAGE_ERROR`, but the new value remains in RAM and can be returned by `GET` or
used for execution. A `-EINVAL` returned by the slot API is mapped to
`INVALID_TEXT`; other failures are mapped to `STORAGE_ERROR`.

### SET recovery

If a non-final chunk is repeated after it was already accepted, its offset is no
longer the next expected offset. The response is `BAD_OFFSET`, and the staging
transaction is discarded. The client must restart the complete transaction with
a new `offset=0` chunk. If the final chunk was accepted but its final ACK was
lost, retransmitting that final non-zero-offset chunk has no active transaction
and returns `BAD_REQUEST`; the client must use the same full restart procedure.
A restart at offset zero replaces any incomplete staging and does not modify the
slot until the new transaction's final chunk is accepted.

## CLEAR

A `CLEAR` request must have `status=0`, a valid slot, `payload_length=0`,
`offset=0`, `total_length=0`, and zero-filled unused payload bytes. On success the
slot is cleared and the response is `OK` with zero offset/length and no payload.
A Settings deletion failure is `STORAGE_ERROR`. Clearing is also RAM-first: the
slot is empty in RAM even when the delete backend reports an error.

`LIST`, `GET`, and `CLEAR` do not discard an incomplete valid SET transaction. A
new `SET` with `offset=0` replaces it. One protocol context owns one SET staging
transaction; calls for that context must be serialized by its transport consumer.
The protocol core does not create a thread, queue, USB device, or singleton state.

## USB HID transport (optional)

When `CONFIG_ZMK_RUNTIME_MACRO_USB_HID=y`, the module registers a dedicated
legacy USB HID instance. Its device name defaults to `HID_1` and can be changed
with `CONFIG_ZMK_RUNTIME_MACRO_USB_HID_DEVICE` when another module already owns
that instance. ZMK's normal keyboard remains `HID_0`; the module never changes
its descriptor or callbacks. The transport is compiled only for a USB-enabled
unibody or split central build, and requires
`CONFIG_USB_DEVICE_STACK=y`. The safe module defaults are
`CONFIG_HID_INTERRUPT_EP_MPS=32` (an explicit smaller configuration is rejected
at compile time). `CONFIG_USB_HID_DEVICE_COUNT` must include the configured
runtime macro HID device; for example, using `HID_2` requires at least three
HID instances. Do not enable
`CONFIG_ENABLE_HID_INT_OUT_EP`: that global option would add an OUT endpoint to
`HID_0` as well.

The HID report descriptor uses vendor Usage Page `0xff60` and top-level
Application Usage `0x61`. Usage `0x62` denotes the 32-byte Input report and
Usage `0x63` denotes the 32-byte Output report. There is no Report ID byte: the
report payload is exactly the protocol's 32-byte frame. The descriptor's report
count is 32 with 8-bit fields; there is no padding before, after, or between
fields.

Because interrupt OUT is deliberately disabled, host requests are delivered by
HID class control `SET_REPORT` to the configured runtime macro HID device's
`set_report` callback. The callback accepts only an Output report with report ID
0 and length 32, copies it to a fixed queue without parsing or blocking, and
schedules the transport work. The work consumer processes one request at a time
and sends every complete 32-byte response through that device's interrupt IN
endpoint using `hid_int_ep_write()`.
Only one IN transfer is in flight; the completion callback releases the send
permit and schedules the next queued request. The 32-byte response buffer is
static storage and remains unchanged until `int_in_ready` confirms completion.
A failed write releases the permit so the host can retry; the firmware does not
add a second retry protocol. Every ZMK USB connection-state notification,
including one reporting HID, performs a logical fail-closed reset of the
authentication core, protocol staging, queued requests, and generation. Only
after a stable HID notification does the transport become online again;
suspend/resume notifications may therefore require reauthentication. The raw status mapping
must also agree with the event (`SUSPEND`/`CONFIGURED`/`RESUME`/
`CLEAR_HALT`/`SOF` → HID, `DISCONNECTED`/`UNKNOWN` → NONE, all others →
POWERED). If the raw USB status changes during asynchronous event handling, the
transport also remains offline until a stable matching notification. Endpoint-owned
IN buffer recovery is separate: the legacy permit is recycled only for raw
`USB_DC_RESET`, `USB_DC_DISCONNECTED`, or
`USB_DC_CONFIGURED` statuses. A SUSPEND/RESUME/CLEAR_HALT (or unknown/error)
status retains the permit until the normal `DATA_IN` callback or a later known
endpoint boundary.
