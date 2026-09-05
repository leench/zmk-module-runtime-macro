"""Command-line client for the ZMK runtime-macro USB HID protocol."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import secrets
import sys
import time
import unicodedata
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from getpass import getpass
from pathlib import Path
from types import TracebackType
from typing import Any, Self

# These values intentionally mirror include/zmk/runtime_macro_protocol.h.  Keep
# this client independent of the firmware build: the wire contract is fixed.
FRAME_SIZE = 32
HEADER_SIZE = 10
PAYLOAD_SIZE = 22
VERSION = 2
LIST_SLOT = 0xFF

# Authentication values mirror include/zmk/runtime_macro_auth.h and
# include/zmk/runtime_macro_protocol.h.  The client derives the key; the
# firmware only receives the 52-byte iterations/salt/key object.
AUTH_CREDENTIAL_VERSION = 1
AUTH_CREDENTIAL_STORAGE_SIZE = 53
AUTH_KDF_ID = 1
AUTH_ITERATIONS_DEFAULT = 600_000
AUTH_ITERATIONS_MIN = 100_000
AUTH_ITERATIONS_MAX = 5_000_000
AUTH_SALT_SIZE = 16
AUTH_KEY_SIZE = 32
AUTH_NONCE_SIZE = 16
AUTH_PROOF_SIZE = 16
AUTH_INFO_SIZE = 22
PASSWORD_CREDENTIAL_SIZE = 52
PASSWORD_SET_LENGTH = PASSWORD_CREDENTIAL_SIZE
AUTH_DOMAIN = b"ZMK-RUNTIME-MACRO-AUTH-V2"
AUTH_FLAG_PASSWORD_CONFIGURED = 1 << 0
AUTH_FLAG_SESSION_AUTHENTICATED = 1 << 1
# Short aliases match the protocol document's flag names.
AUTH_FLAGS_CONFIGURED = AUTH_FLAG_PASSWORD_CONFIGURED
AUTH_FLAGS_SESSION = AUTH_FLAG_SESSION_AUTHENTICATED
AUTH_FLAG_RESERVED_MASK = 0xFC
AUTH_STATE_OPEN = 0
AUTH_STATE_PROTECTED = 1
AUTH_STATE_ERROR_LOCKED = 2

VERSION_OFFSET = 0
OPCODE_OFFSET = 1
REQUEST_ID_OFFSET = 2
STATUS_OFFSET = 3
SLOT_OFFSET = 4
PAYLOAD_LENGTH_OFFSET = 5
OFFSET_OFFSET = 6
TOTAL_LENGTH_OFFSET = 8
PAYLOAD_OFFSET = 10

OPCODE_LIST = 1
OPCODE_GET = 2
OPCODE_SET = 3
OPCODE_CLEAR = 4
OPCODE_AUTH_INFO = 0x10
OPCODE_AUTH_CHALLENGE = 0x11
OPCODE_AUTH_PROVE = 0x12
OPCODE_PASSWORD_SET = 0x13
OPCODE_LOCK = 0x14

STATUS_OK = 0
STATUS_BAD_VERSION = 1
STATUS_BAD_OPCODE = 2
STATUS_BAD_REQUEST = 3
STATUS_BAD_SLOT = 4
STATUS_BAD_OFFSET = 5
STATUS_BAD_LENGTH = 6
STATUS_INVALID_TEXT = 7
STATUS_STORAGE_ERROR = 8
STATUS_INTERNAL = 9
STATUS_AUTH_REQUIRED = 10
STATUS_AUTH_FAILED = 11
STATUS_AUTH_NOT_CONFIGURED = 12
STATUS_RATE_LIMITED = 13
STATUS_AUTH_NO_CHALLENGE = 14
STATUS_CREDENTIAL_INVALID = 15

STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_BAD_VERSION: "BAD_VERSION",
    STATUS_BAD_OPCODE: "BAD_OPCODE",
    STATUS_BAD_REQUEST: "BAD_REQUEST",
    STATUS_BAD_SLOT: "BAD_SLOT",
    STATUS_BAD_OFFSET: "BAD_OFFSET",
    STATUS_BAD_LENGTH: "BAD_LENGTH",
    STATUS_INVALID_TEXT: "INVALID_TEXT",
    STATUS_STORAGE_ERROR: "STORAGE_ERROR",
    STATUS_INTERNAL: "INTERNAL",
    STATUS_AUTH_REQUIRED: "AUTH_REQUIRED",
    STATUS_AUTH_FAILED: "AUTH_FAILED",
    STATUS_AUTH_NOT_CONFIGURED: "AUTH_NOT_CONFIGURED",
    STATUS_RATE_LIMITED: "RATE_LIMITED",
    STATUS_AUTH_NO_CHALLENGE: "AUTH_NO_CHALLENGE",
    STATUS_CREDENTIAL_INVALID: "CREDENTIAL_INVALID",
}


class RuntimeMacroError(Exception):
    """Base class for expected client failures."""


class DeviceError(RuntimeMacroError):
    pass


class TransportError(RuntimeMacroError):
    pass


class ProtocolError(RuntimeMacroError):
    pass


class TimeoutError(TransportError):
    pass


class RemoteError(RuntimeMacroError):
    def __init__(self, status: int):
        self.status = status
        name = STATUS_NAMES.get(status, f"UNKNOWN_STATUS_{status}")
        super().__init__(f"firmware returned {name} ({status})")


class LegacyFirmwareError(RemoteError):
    """The device rejected a v2 request, usually because it still runs v1."""

    def __init__(self):
        super().__init__(STATUS_BAD_VERSION)
        self.args = ("固件仍是 v1 或不支持 v2 认证协议，请升级固件后再试；客户端不会自动回退到 v1",)


class PasswordSetUnconfirmed(ProtocolError):
    """The final PASSWORD_SET result could not be confirmed by its salt."""


def u16(frame: bytes | bytearray, offset: int) -> int:
    return frame[offset] | (frame[offset + 1] << 8)


def put_u16(frame: bytearray, offset: int, value: int) -> None:
    frame[offset] = value & 0xFF
    frame[offset + 1] = (value >> 8) & 0xFF


def build_frame(
    opcode: int,
    request_id: int,
    slot: int,
    *,
    payload: bytes = b"",
    offset: int = 0,
    total_length: int = 0,
) -> bytes:
    """Build a canonical, zero-filled protocol request."""
    if not 0 <= request_id <= 0xFF or not 0 <= slot <= 0xFF:
        raise ValueError("request_id and slot must fit in one byte")
    if len(payload) > PAYLOAD_SIZE:
        raise ValueError("payload exceeds one protocol frame")
    if not 0 <= offset <= 0xFFFF or not 0 <= total_length <= 0xFFFF:
        raise ValueError("offset and total_length must fit in uint16")
    frame = bytearray(FRAME_SIZE)
    frame[VERSION_OFFSET] = VERSION
    frame[OPCODE_OFFSET] = opcode
    frame[REQUEST_ID_OFFSET] = request_id
    # Requests always carry status zero and zero-filled padding.
    frame[STATUS_OFFSET] = STATUS_OK
    frame[SLOT_OFFSET] = slot
    frame[PAYLOAD_LENGTH_OFFSET] = len(payload)
    put_u16(frame, OFFSET_OFFSET, offset)
    put_u16(frame, TOTAL_LENGTH_OFFSET, total_length)
    frame[PAYLOAD_OFFSET : PAYLOAD_OFFSET + len(payload)] = payload
    return bytes(frame)


def validate_ascii(data: bytes) -> None:
    for value in data:
        if not (0x20 <= value <= 0x7E or value in (0x08, 0x09, 0x0A)):
            raise ValueError(
                f"unsupported text byte 0x{value:02x}; only printable ASCII, "
                "LF, TAB, and Backspace are allowed"
            )


def visible_text(data: bytes) -> str:
    """Render supported text without control characters affecting a terminal."""
    out: list[str] = []
    for value in data:
        if value == 0x08:
            out.append(r"\b")
        elif value == 0x09:
            out.append(r"\t")
        elif value == 0x0A:
            out.append(r"\n")
        elif value == 0x5C:
            out.append(r"\\")
        elif 0x20 <= value <= 0x7E:
            out.append(chr(value))
        else:
            out.append(f"\\x{value:02x}")
    return "".join(out)


def _path_text(path: Any) -> str:
    if isinstance(path, bytes):
        return os.fsdecode(path)
    return str(path)


def same_path(left: Any, right: Any) -> bool:
    """Compare hidapi paths when one binding returns bytes and another str."""
    if isinstance(left, bytes) and isinstance(right, bytes):
        return left == right
    return _path_text(left) == _path_text(right)


def _field_int(info: dict[str, Any], name: str) -> int:
    value = info.get(name)
    try:
        return int(value)
    except (TypeError, ValueError):
        return -1


def _device_summary(info: dict[str, Any]) -> str:
    path = _path_text(info.get("path", "<no path>"))
    vid = _field_int(info, "vendor_id")
    pid = _field_int(info, "product_id")
    serial = info.get("serial_number") or "-"
    product = info.get("product_string") or "-"
    if isinstance(serial, bytes):
        serial = os.fsdecode(serial)
    if isinstance(product, bytes):
        product = os.fsdecode(product)
    return f"path={path}, VID=0x{vid:04x}, PID=0x{pid:04x}, serial={serial}, product={product}"


def find_device(
    hid_module: Any,
    *,
    path: Any = None,
    vid: int | None = None,
    pid: int | None = None,
) -> dict[str, Any]:
    records = []
    missing_usage_metadata = False
    for info in hid_module.enumerate():
        if path is not None and not same_path(info.get("path"), path):
            continue
        if vid is not None and _field_int(info, "vendor_id") != vid:
            continue
        if pid is not None and _field_int(info, "product_id") != pid:
            continue

        usage_page = _field_int(info, "usage_page")
        usage = _field_int(info, "usage")
        if usage_page == 0 and usage == 0:
            missing_usage_metadata = True
            # Some hidapi backends omit the parsed Usage fields. An explicit
            # path is still unambiguous; without one, never guess a device.
            if path is None:
                continue
        elif usage_page != 0xFF60 or usage != 0x61:
            continue
        records.append(info)

    if not records:
        if path is None and missing_usage_metadata:
            raise DeviceError(
                "hidapi 未提供 HID Usage 元数据，请使用 --path 精确选择 runtime macro HID；"
                "也请确认固件已启用 CONFIG_ZMK_RUNTIME_MACRO_USB_HID 并检查 hidraw/udev 权限"
            )
        raise DeviceError(
            "未找到 dongle runtime macro HID（请确认固件已启用 CONFIG_ZMK_RUNTIME_MACRO_USB_HID，"
            "并检查 hidraw/udev 权限）"
        )
    if len(records) > 1 and path is None:
        choices = "\n".join(f"  {_device_summary(info)}" for info in records)
        raise DeviceError("找到多个匹配的 dongle HID，请使用 --path 精确选择 runtime macro HID：\n" + choices)
    return records[0]


class HidTransport:
    """Small injectable wrapper around the classic Python hidapi API."""

    def __init__(self, device: Any, *, clock: Callable[[], float] = time.monotonic):
        self.device = device
        self.clock = clock
        self.closed = False

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            try:
                self.device.close()
            except Exception:  # noqa: BLE001 - close must not mask errors
                return

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def write_frame(self, frame: bytes) -> None:
        if len(frame) != FRAME_SIZE:
            raise TransportError("internal error: request frame is not 32 bytes")
        try:
            # hidapi requires report ID as the first byte even though the
            # firmware report descriptor has no Report ID field.
            result = self.device.write(bytes([0]) + frame)
        except Exception as exc:
            raise TransportError(f"HID write failed: {exc}") from exc
        if result not in (FRAME_SIZE, FRAME_SIZE + 1):
            raise TransportError(
                f"HID write returned {result}, expected 32 or 33 bytes (partial write)"
            )

    @staticmethod
    def normalize_read(data: Any) -> bytes:
        try:
            raw = bytes(data)
        except Exception as exc:
            raise ProtocolError(f"HID read returned non-byte data: {exc}") from exc
        if len(raw) == FRAME_SIZE:
            return raw
        if len(raw) == FRAME_SIZE + 1 and raw[0] == 0:
            return raw[1:]
        if len(raw) == FRAME_SIZE + 1:
            raise ProtocolError("HID response has a non-zero report ID")
        raise ProtocolError(f"HID response has length {len(raw)}, expected 32 or 33")

    def read_frame(self, timeout_ms: int) -> bytes:
        try:
            data = self.device.read(FRAME_SIZE + 1, max(0, timeout_ms))
        except Exception as exc:
            raise TransportError(f"HID read failed: {exc}") from exc
        if not data:
            raise TimeoutError("timed out waiting for HID response")
        return self.normalize_read(data)

    def exchange(self, frame: bytes, timeout_ms: int) -> bytes:
        self.write_frame(frame)
        deadline = self.clock() + max(0, timeout_ms) / 1000.0
        while True:
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for HID response")
            wait_ms = max(1, int(remaining * 1000))
            response = self.read_frame(wait_ms)
            # Stale responses can be left in the input queue after a host
            # timeout. They do not extend the original monotonic deadline.
            if (
                response[REQUEST_ID_OFFSET] != frame[REQUEST_ID_OFFSET]
                or response[OPCODE_OFFSET] != frame[OPCODE_OFFSET]
                or response[SLOT_OFFSET] != frame[SLOT_OFFSET]
            ):
                continue
            return response


def validate_response(response: bytes, request: bytes) -> int:
    if len(response) != FRAME_SIZE:
        raise ProtocolError("internal error: normalized response is not 32 bytes")
    for offset, name in (
        (VERSION_OFFSET, "version"),
        (OPCODE_OFFSET, "opcode"),
        (REQUEST_ID_OFFSET, "request_id"),
        (SLOT_OFFSET, "slot"),
    ):
        if response[offset] != request[offset]:
            raise ProtocolError(f"response {name} does not match request")
    if response[STATUS_OFFSET] not in STATUS_NAMES:
        raise ProtocolError(f"response has unknown status {response[STATUS_OFFSET]}")
    payload_length = response[PAYLOAD_LENGTH_OFFSET]
    if payload_length > PAYLOAD_SIZE:
        raise ProtocolError("response payload length exceeds 22 bytes")
    if any(response[PAYLOAD_OFFSET + payload_length :]):
        raise ProtocolError("response payload padding is not zero")
    status = response[STATUS_OFFSET]
    if status != STATUS_OK:
        if payload_length != 0 or u16(response, OFFSET_OFFSET) != 0 or u16(response, TOTAL_LENGTH_OFFSET) != 0:
            raise ProtocolError("error response contains payload or non-zero range")
        raise RemoteError(status)
    return payload_length


@dataclass(frozen=True)
class SlotInfo:
    slot: int
    length: int


@dataclass(frozen=True)
class AuthInfo:
    """Public v2 authentication metadata returned by ``AUTH_INFO``."""

    configured: bool
    authenticated: bool
    iterations: int
    salt: bytes

    @property
    def state(self) -> str:
        return "PROTECTED" if self.configured else "OPEN"

    @property
    def protected(self) -> bool:
        return self.configured

    @property
    def open(self) -> bool:
        return not self.configured


@dataclass(frozen=True)
class _PasswordSetTimeout(Exception):
    error: TransportError
    final_chunk: bool

    def __str__(self) -> str:
        return str(self.error)


def normalize_password(password: str) -> bytes:
    """Normalize a user password exactly as required by the v2 contract."""
    if not isinstance(password, str):
        raise TypeError("password must be a string")
    normalized = unicodedata.normalize("NFC", password)
    password_bytes = normalized.encode("utf-8")
    if not password_bytes:
        raise ValueError("password must not be empty")
    return password_bytes


def validate_iterations(iterations: int) -> None:
    if not isinstance(iterations, int) or not AUTH_ITERATIONS_MIN <= iterations <= AUTH_ITERATIONS_MAX:
        raise ValueError(
            f"iterations must be between {AUTH_ITERATIONS_MIN} and "
            f"{AUTH_ITERATIONS_MAX}"
        )


def validate_auth_parameters(iterations: int, salt: bytes) -> None:
    validate_iterations(iterations)
    if len(salt) != AUTH_SALT_SIZE:
        raise ValueError(f"salt must be exactly {AUTH_SALT_SIZE} bytes")
    if not any(salt):
        raise ValueError("salt must not be all zero")


def derive_key(password: str, salt: bytes, iterations: int = AUTH_ITERATIONS_DEFAULT) -> bytes:
    """Return the v2 PBKDF2-HMAC-SHA256 password key."""
    password_bytes = normalize_password(password)
    salt = bytes(salt)
    validate_auth_parameters(iterations, salt)
    return hashlib.pbkdf2_hmac(
        "sha256", password_bytes, salt, iterations, dklen=AUTH_KEY_SIZE
    )


def build_auth_proof(key: bytes, nonce: bytes) -> bytes:
    """Build the truncated v2 HMAC proof without retaining protocol state."""
    if len(key) != AUTH_KEY_SIZE:
        raise ValueError(f"key must be exactly {AUTH_KEY_SIZE} bytes")
    if len(nonce) != AUTH_NONCE_SIZE:
        raise ValueError(f"nonce must be exactly {AUTH_NONCE_SIZE} bytes")
    return hmac.new(key, AUTH_DOMAIN + nonce, hashlib.sha256).digest()[:AUTH_PROOF_SIZE]


class RuntimeMacroClient:
    def __init__(
        self,
        transport: HidTransport,
        *,
        timeout_ms: int = 1000,
        retries: int = 2,
    ):
        if timeout_ms < 1 or retries < 0:
            raise ValueError("timeout_ms must be positive and retries non-negative")
        self.transport = transport
        self.timeout_ms = timeout_ms
        self.retries = retries
        self._next_request_id = 0

    def _request_id(self) -> int:
        value = self._next_request_id
        self._next_request_id = (value + 1) & 0xFF
        return value

    def _call(self, frame: bytes) -> bytes:
        response = self.transport.exchange(frame, self.timeout_ms)
        try:
            validate_response(response, frame)
        except RemoteError as exc:
            if exc.status == STATUS_BAD_VERSION:
                raise LegacyFirmwareError() from exc
            raise
        return response

    def _call_with_timeout_retry(self, make_frame: Callable[[int], bytes]) -> bytes:
        """Retry only a newly-built request after transport failure."""
        last: Exception | None = None
        for _ in range(self.retries + 1):
            request = make_frame(self._request_id())
            try:
                return self._call(request)
            except TimeoutError as exc:
                last = exc
            except TransportError as exc:
                last = exc
            except (ProtocolError, RemoteError):
                raise
        assert last is not None
        raise last

    @staticmethod
    def _validate_empty_ack(response: bytes, operation: str) -> None:
        if (
            response[PAYLOAD_LENGTH_OFFSET] != 0
            or u16(response, OFFSET_OFFSET) != 0
            or u16(response, TOTAL_LENGTH_OFFSET) != 0
        ):
            raise ProtocolError(f"{operation} acknowledgement is not empty")

    @staticmethod
    def _validate_chunk_ack(
        response: bytes, operation: str, expected_offset: int, total: int
    ) -> None:
        if response[PAYLOAD_LENGTH_OFFSET] != 0:
            raise ProtocolError(f"{operation} acknowledgement contains a payload")
        if u16(response, OFFSET_OFFSET) != expected_offset:
            raise ProtocolError(f"{operation} acknowledgement offset is incorrect")
        if u16(response, TOTAL_LENGTH_OFFSET) != total:
            raise ProtocolError(f"{operation} acknowledgement total length is incorrect")

    def _auth_info_once(self) -> AuthInfo:
        request = build_frame(OPCODE_AUTH_INFO, self._request_id(), LIST_SLOT)
        response = self._call(request)
        payload_length = response[PAYLOAD_LENGTH_OFFSET]
        if (
            payload_length != AUTH_INFO_SIZE
            or u16(response, OFFSET_OFFSET) != 0
            or u16(response, TOTAL_LENGTH_OFFSET) != AUTH_INFO_SIZE
        ):
            raise ProtocolError("AUTH_INFO response has invalid length or metadata")

        payload = response[PAYLOAD_OFFSET : PAYLOAD_OFFSET + AUTH_INFO_SIZE]
        flags = payload[0]
        if flags & AUTH_FLAG_RESERVED_MASK:
            raise ProtocolError("AUTH_INFO response contains reserved flags")
        if payload[1] != AUTH_KDF_ID:
            raise ProtocolError("AUTH_INFO response contains an unknown KDF")
        iterations = int.from_bytes(payload[2:6], "little")
        salt = bytes(payload[6:22])
        configured = bool(flags & AUTH_FLAG_PASSWORD_CONFIGURED)
        authenticated = bool(flags & AUTH_FLAG_SESSION_AUTHENTICATED)
        if not configured:
            if authenticated or iterations != AUTH_ITERATIONS_DEFAULT or any(salt):
                raise ProtocolError("AUTH_INFO OPEN metadata is inconsistent")
        else:
            try:
                validate_auth_parameters(iterations, salt)
            except ValueError as exc:
                raise ProtocolError(
                    f"AUTH_INFO PROTECTED metadata is invalid: {exc}"
                ) from exc
        return AuthInfo(configured, authenticated, iterations, salt)

    def auth_info(self) -> AuthInfo:
        """Read and strictly validate the public v2 authentication metadata."""
        return self._retry_call(self._auth_info_once)

    def _retry_call(self, operation: Callable[[], Any]) -> Any:
        """Retry an idempotent operation without reusing a request frame."""
        last: Exception | None = None
        for _ in range(self.retries + 1):
            try:
                return operation()
            except TimeoutError as exc:
                last = exc
            except TransportError as exc:
                last = exc
        assert last is not None
        raise last

    def _auth_challenge_once(self) -> bytes:
        request = build_frame(
            OPCODE_AUTH_CHALLENGE, self._request_id(), LIST_SLOT
        )
        response = self._call(request)
        payload_length = response[PAYLOAD_LENGTH_OFFSET]
        if (
            payload_length != AUTH_NONCE_SIZE
            or u16(response, OFFSET_OFFSET) != 0
            or u16(response, TOTAL_LENGTH_OFFSET) != AUTH_NONCE_SIZE
        ):
            raise ProtocolError("AUTH_CHALLENGE response has invalid length or metadata")
        nonce = bytes(response[PAYLOAD_OFFSET : PAYLOAD_OFFSET + AUTH_NONCE_SIZE])
        if not any(nonce):
            raise ProtocolError("AUTH_CHALLENGE returned an all-zero nonce")
        return nonce

    def _auth_prove_once(self, proof: bytes) -> None:
        request = build_frame(
            OPCODE_AUTH_PROVE,
            self._request_id(),
            LIST_SLOT,
            payload=proof,
            total_length=AUTH_PROOF_SIZE,
        )
        response = self._call(request)
        self._validate_empty_ack(response, "AUTH_PROVE")

    def authenticate(self, password: str) -> AuthInfo:
        """Authenticate with a fresh info/challenge/proof sequence.

        A transport failure at any point starts a completely new sequence. In
        particular, an AUTH_PROVE timeout never resends its old nonce or proof.
        """
        password_bytes = normalize_password(password)
        last: Exception | None = None
        for _ in range(self.retries + 1):
            try:
                info = self._auth_info_once()
                if not info.configured:
                    raise RemoteError(STATUS_AUTH_NOT_CONFIGURED)
                nonce = self._auth_challenge_once()
                key = hashlib.pbkdf2_hmac(
                    "sha256",
                    password_bytes,
                    info.salt,
                    info.iterations,
                    dklen=AUTH_KEY_SIZE,
                )
                proof = build_auth_proof(key, nonce)
                self._auth_prove_once(proof)
                return AuthInfo(True, True, info.iterations, info.salt)
            except TimeoutError as exc:
                last = exc
            except TransportError as exc:
                last = exc
        assert last is not None
        raise last

    def _password_set_once(self, credential: bytes, request_id: int) -> None:
        for offset in range(0, len(credential), PAYLOAD_SIZE):
            chunk = credential[offset : offset + PAYLOAD_SIZE]
            final_chunk = offset + len(chunk) == len(credential)
            request = build_frame(
                OPCODE_PASSWORD_SET,
                request_id,
                LIST_SLOT,
                payload=chunk,
                offset=offset,
                total_length=len(credential),
            )
            try:
                response = self._call(request)
            except (TimeoutError, TransportError) as exc:
                raise _PasswordSetTimeout(exc, final_chunk) from exc
            self._validate_chunk_ack(
                response, "PASSWORD_SET", offset + len(chunk), len(credential)
            )

    def _confirm_password_set(self, salt: bytes, password: str) -> AuthInfo:
        info = self.auth_info()
        if (
            not info.configured
            or not hmac.compare_digest(info.salt, salt)
        ):
            raise PasswordSetUnconfirmed(
                "PASSWORD_SET was not confirmed: device salt does not match"
            )
        return self.authenticate(password)

    def set_password(
        self,
        new_password: str,
        *,
        iterations: int = AUTH_ITERATIONS_DEFAULT,
    ) -> AuthInfo:
        """Set or replace the password using one authenticated v2 transaction."""
        password_bytes = normalize_password(new_password)
        validate_iterations(iterations)
        info = self.auth_info()
        if info.configured and not info.authenticated:
            raise RemoteError(STATUS_AUTH_REQUIRED)

        salt = secrets.token_bytes(AUTH_SALT_SIZE)
        validate_auth_parameters(iterations, salt)
        key = hashlib.pbkdf2_hmac(
            "sha256", password_bytes, salt, iterations, dklen=AUTH_KEY_SIZE
        )
        if not any(key):
            raise ValueError("derived key must not be all zero")
        credential = (
            iterations.to_bytes(4, "little") + salt + key
        )

        last: Exception | None = None
        for attempt in range(self.retries + 1):
            request_id = self._request_id()
            try:
                self._password_set_once(credential, request_id)
                return self._confirm_password_set(salt, new_password)
            except _PasswordSetTimeout as exc:
                if exc.final_chunk:
                    # The final write may already have committed. Never resend
                    # it blindly; compare the public salt instead.
                    return self._confirm_password_set(salt, new_password)
                last = exc.error
                if attempt == self.retries:
                    raise last
            except RemoteError as exc:
                if exc.status not in (STATUS_BAD_REQUEST, STATUS_BAD_OFFSET):
                    raise
                last = exc
                if attempt == self.retries:
                    raise
        assert last is not None
        raise last

    def lock(self) -> None:
        """Close the current management session and discard protocol staging."""
        def do_lock() -> None:
            request = build_frame(OPCODE_LOCK, self._request_id(), LIST_SLOT)
            response = self._call(request)
            self._validate_empty_ack(response, "LOCK")

        self._retry_call(do_lock)

    def list_slots(self) -> list[SlotInfo]:
        offset = 0
        total: int | None = None
        result = bytearray()
        while total is None or offset < total:
            requested_offset = offset
            response = self._call_with_timeout_retry(
                lambda request_id, requested_offset=requested_offset: build_frame(
                    OPCODE_LIST, request_id, LIST_SLOT, offset=requested_offset
                )
            )
            payload_length = response[PAYLOAD_LENGTH_OFFSET]
            response_offset = u16(response, OFFSET_OFFSET)
            response_total = u16(response, TOTAL_LENGTH_OFFSET)
            if response_offset != requested_offset:
                raise ProtocolError("LIST response offset does not match request")
            if total is None:
                total = response_total
            elif total != response_total:
                raise ProtocolError("LIST response total length changed")
            if response_offset + payload_length > response_total:
                raise ProtocolError("LIST response exceeds logical result")
            if response_total > 0 and response_offset < response_total and payload_length == 0:
                raise ProtocolError("LIST response made no progress")
            result.extend(response[PAYLOAD_OFFSET : PAYLOAD_OFFSET + payload_length])
            offset += payload_length
            if total == 0:
                break
        if total is None:
            total = 0
        if len(result) != total or total < 1:
            raise ProtocolError("LIST logical result has invalid length")
        count = result[0]
        if total != 1 + 2 * count or len(result) != 1 + 2 * count:
            raise ProtocolError("LIST logical result is truncated or has extra data")
        return [SlotInfo(i, result[1 + 2 * i] | (result[2 + 2 * i] << 8)) for i in range(count)]

    def get_slot(self, slot: int) -> bytes:
        if not 0 <= slot <= 0xFE:
            raise ValueError("slot must be between 0 and 254")
        offset = 0
        total: int | None = None
        result = bytearray()
        while total is None or offset < total:
            requested_offset = offset
            response = self._call_with_timeout_retry(
                lambda request_id, requested_offset=requested_offset: build_frame(
                    OPCODE_GET, request_id, slot, offset=requested_offset
                )
            )
            payload_length = response[PAYLOAD_LENGTH_OFFSET]
            response_offset = u16(response, OFFSET_OFFSET)
            response_total = u16(response, TOTAL_LENGTH_OFFSET)
            if response_offset != requested_offset:
                raise ProtocolError("GET response offset does not match request")
            if total is None:
                total = response_total
            elif total != response_total:
                raise ProtocolError("GET response total length changed")
            if response_offset + payload_length > response_total:
                raise ProtocolError("GET response exceeds logical result")
            if response_total > 0 and response_offset < response_total and payload_length == 0:
                raise ProtocolError("GET response made no progress")
            result.extend(response[PAYLOAD_OFFSET : PAYLOAD_OFFSET + payload_length])
            offset += payload_length
            if total == 0:
                break
        if total is None or len(result) != total:
            raise ProtocolError("GET logical result has invalid length")
        data = bytes(result)
        try:
            validate_ascii(data)
        except ValueError as exc:
            raise ProtocolError(f"GET returned invalid text: {exc}") from exc
        return data

    def _set_once(self, data: bytes, request_id: int) -> None:
        total = len(data)
        chunks: Iterable[tuple[int, bytes]] = ((0, b""),) if total == 0 else (
            (offset, data[offset : offset + PAYLOAD_SIZE])
            for offset in range(0, total, PAYLOAD_SIZE)
        )
        for offset, chunk in chunks:
            request = build_frame(
                OPCODE_SET,
                request_id,
                self._set_slot,
                payload=chunk,
                offset=offset,
                total_length=total,
            )
            response = self._call(request)
            if response[PAYLOAD_LENGTH_OFFSET] != 0:
                raise ProtocolError("SET acknowledgement contains a payload")
            if u16(response, OFFSET_OFFSET) != offset + len(chunk):
                raise ProtocolError("SET acknowledgement offset is incorrect")
            if u16(response, TOTAL_LENGTH_OFFSET) != total:
                raise ProtocolError("SET acknowledgement total length is incorrect")

    def set_slot(self, slot: int, data: bytes) -> None:
        if not 0 <= slot <= 0xFE:
            raise ValueError("slot must be between 0 and 254")
        validate_ascii(data)
        self._set_slot = slot
        last: Exception | None = None
        for _ in range(self.retries + 1):
            request_id = self._request_id()
            try:
                self._set_once(data, request_id)
                return
            except (TimeoutError, TransportError) as exc:
                last = exc
            except RemoteError as exc:
                # Only transaction context errors are safe to recover by
                # restarting at offset zero. Slot/length/text/storage errors
                # describe the request and must be shown to the user.
                if exc.status not in (STATUS_BAD_REQUEST, STATUS_BAD_OFFSET):
                    raise
                last = exc
            except ProtocolError:
                # A matching malformed response is not silently retried.
                raise
        assert last is not None
        raise last

    def clear_slot(self, slot: int) -> None:
        if not 0 <= slot <= 0xFE:
            raise ValueError("slot must be between 0 and 254")
        last: Exception | None = None
        for _ in range(self.retries + 1):
            request = build_frame(OPCODE_CLEAR, self._request_id(), slot)
            try:
                response = self._call(request)
                if response[PAYLOAD_LENGTH_OFFSET] != 0 or u16(response, OFFSET_OFFSET) != 0 or u16(response, TOTAL_LENGTH_OFFSET) != 0:
                    raise ProtocolError("CLEAR acknowledgement is not empty")
                return
            except TimeoutError as exc:
                last = exc
            except TransportError as exc:
                last = exc
        assert last is not None
        raise last


def parse_int(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a decimal or 0x-prefixed integer") from exc
    if not 0 <= parsed <= 0xFFFF:
        raise argparse.ArgumentTypeError("must be between 0 and 65535")
    return parsed


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Configure ZMK runtime macro slots")
    parser.add_argument("--path", help="exact HID path")
    parser.add_argument("--vid", type=parse_int)
    parser.add_argument("--pid", type=parse_int)
    parser.add_argument("--timeout-ms", type=int, default=1000)
    parser.add_argument("--retries", type=int, default=2)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    get = sub.add_parser("get")
    get.add_argument("slot", type=int)
    get.add_argument("--raw", action="store_true", help="write raw bytes to stdout")
    set_parser = sub.add_parser("set")
    set_parser.add_argument("slot", type=int)
    inputs = set_parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--text")
    inputs.add_argument("--file", type=Path)
    inputs.add_argument("--stdin", action="store_true")
    clear = sub.add_parser("clear")
    clear.add_argument("slot", type=int)
    sub.add_parser("auth-info", help="show OPEN/PROTECTED status")
    sub.add_parser("login", help="authenticate with the configured password")
    sub.add_parser("set-password", help="set or replace the management password")
    sub.add_parser("lock", help="close the current management session")
    return parser


def _load_hid() -> Any:
    try:
        import hid  # type: ignore
    except ImportError as exc:
        raise DeviceError("缺少 hidapi：请在 venv 中执行 pip install -r tools/requirements.txt") from exc
    return hid


def open_transport(
    hid_module: Any,
    *,
    path: Any = None,
    vid: int | None = None,
    pid: int | None = None,
    clock: Callable[[], float] = time.monotonic,
) -> HidTransport:
    info = find_device(hid_module, path=path, vid=vid, pid=pid)
    device = hid_module.device()
    try:
        device.open_path(info["path"])
    except Exception as exc:
        try:
            device.close()
        except Exception:  # noqa: BLE001,S110 - close must not mask open failure
            pass
        raise DeviceError(f"无法打开 dongle runtime macro HID（{_path_text(info.get('path'))}）：{exc}") from exc
    return HidTransport(device, clock=clock)


def _read_set_data(args: argparse.Namespace) -> bytes:
    if args.text is not None:
        try:
            data = args.text.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError("--text 只允许 ASCII 字符") from exc
    elif args.file is not None:
        try:
            data = args.file.read_bytes()
        except OSError as exc:
            raise ValueError(f"无法读取文件 {args.file}: {exc}") from exc
    else:
        data = sys.stdin.buffer.read()
    validate_ascii(data)
    return data


def _read_password(*, confirm: bool = False) -> str:
    password = getpass("Password: ")
    normalized = normalize_password(password)
    if confirm:
        repeated = getpass("Confirm password: ")
        repeated_normalized = normalize_password(repeated)
        if normalized != repeated_normalized:
            raise ValueError("password entries do not match")
    return password


def main(argv: list[str] | None = None, *, hid_module: Any = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    if args.timeout_ms < 1 or args.retries < 0:
        parser.error("--timeout-ms 必须为正数，--retries 不能为负数")
    try:
        if args.command == "set":
            data = _read_set_data(args)
        else:
            data = None

        # Passwords are deliberately collected before opening HID. They never
        # appear in argv, environment variables, ordinary files, or logs, and
        # invalid confirmation never sends a HID request.
        password = None
        if args.command == "login":
            password = _read_password()
        elif args.command == "set-password":
            password = _read_password(confirm=True)

        module = hid_module if hid_module is not None else _load_hid()
        transport = open_transport(module, path=args.path, vid=args.vid, pid=args.pid)
        with transport:
            client = RuntimeMacroClient(
                transport, timeout_ms=args.timeout_ms, retries=args.retries
            )
            if args.command == "list":
                for item in client.list_slots():
                    print(f"{item.slot}\t{item.length}")
            elif args.command == "get":
                result = client.get_slot(args.slot)
                if args.raw:
                    sys.stdout.buffer.write(result)
                    sys.stdout.buffer.flush()
                else:
                    print(visible_text(result))
            elif args.command == "set":
                assert data is not None
                client.set_slot(args.slot, data)
                print(f"slot {args.slot} set ({len(data)} bytes)")
            elif args.command == "clear":
                client.clear_slot(args.slot)
                print(f"slot {args.slot} cleared")
            elif args.command == "auth-info":
                info = client.auth_info()
                session = "yes" if info.authenticated else "no"
                print(
                    f"state={info.state} authenticated={session} "
                    f"iterations={info.iterations}"
                )
            elif args.command == "login":
                assert password is not None
                client.authenticate(password)
                print("authenticated")
            elif args.command == "set-password":
                assert password is not None
                client.set_password(password)
                print("password configured")
            elif args.command == "lock":
                client.lock()
                print("locked")
        return 0
    except (RuntimeMacroError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
