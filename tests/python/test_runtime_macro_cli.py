import contextlib
import hashlib
import hmac
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[2] / "tools"))
import runtime_macro_cli as cli


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FakeDevice:
    def __init__(self, responder=None, *, open_error=None, write_result=None):
        self.responder = responder
        self.open_error = open_error
        self.write_result = write_result
        self.writes = []
        self.read_calls = []
        self.opened_path = None
        self.closed = False
        self.read_queue = []
        self.read_handler = None

    def open_path(self, path):
        self.opened_path = path
        if self.open_error is not None:
            raise self.open_error

    def close(self):
        self.closed = True

    def write(self, data):
        data = bytes(data)
        self.writes.append(data)
        if self.responder:
            self.responder(self, data)
        if callable(self.write_result):
            return self.write_result(data)
        if self.write_result is not None:
            return self.write_result
        return len(data)

    def read(self, length, timeout_ms):
        self.read_calls.append((length, timeout_ms))
        if self.read_handler is not None:
            return self.read_handler(self, length, timeout_ms)
        if self.read_queue:
            return self.read_queue.pop(0)
        return []


class FakeHid:
    def __init__(self, records, device):
        self.records = records
        self.device_instance = device

    def enumerate(self):
        return self.records

    def device(self):
        return self.device_instance


def record(
    path=b"/dev/hidraw-runtime",
    vid=0x1234,
    pid=0x5678,
    usage_page=0xFF60,
    usage=0x61,
):
    return {
        "path": path,
        "vendor_id": vid,
        "product_id": pid,
        "usage_page": usage_page,
        "usage": usage,
        "serial_number": "serial",
        "product_string": "dongle",
    }


def request_field(wire, offset, size=1):
    """Read a field from the 33-byte hidapi write, including report ID."""
    frame_offset = offset + 1
    if size == 1:
        return wire[frame_offset]
    return int.from_bytes(wire[frame_offset : frame_offset + size], "little")


def response(
    request,
    *,
    payload=b"",
    offset=0,
    total=0,
    status=0,
    bad_tail=False,
):
    # Deliberately build expected wire bytes here instead of calling the
    # client's production frame builder.
    if len(payload) > 22:
        raise ValueError("test response payload exceeds one frame")
    frame = bytearray(32)
    frame[0] = request[0]
    frame[1] = request[1]
    frame[2] = request[2]
    frame[3] = status
    frame[4] = request[4]
    frame[5] = len(payload)
    frame[6:8] = offset.to_bytes(2, "little")
    frame[8:10] = total.to_bytes(2, "little")
    frame[10 : 10 + len(payload)] = payload
    if bad_tail:
        frame[10 + len(payload)] = 0xA5
    return bytes(frame)


def page_responder(logical, *, total=None):
    logical = bytes(logical)
    logical_total = len(logical) if total is None else total

    def reply(device, wire):
        request = wire[1:]
        offset = int.from_bytes(request[6:8], "little")
        chunk = logical[offset : offset + 22]
        device.read_queue.append(
            response(request, payload=chunk, offset=offset, total=logical_total)
        )

    return reply


def auth_info_payload(
    *, configured, authenticated=False, iterations=600000, salt=bytes(16)
):
    flags = 0
    if configured:
        flags |= cli.AUTH_FLAG_PASSWORD_CONFIGURED
    if authenticated:
        flags |= cli.AUTH_FLAG_SESSION_AUTHENTICATED
    return bytes([flags, cli.AUTH_KDF_ID]) + iterations.to_bytes(4, "little") + bytes(salt)


def empty_ack(request):
    frame = bytearray(32)
    frame[0] = request[0]
    frame[1] = request[1]
    frame[2] = request[2]
    frame[4] = request[4]
    return bytes(frame)


def opcode(wire):
    return wire[1 + cli.OPCODE_OFFSET]


class ClientTests(unittest.TestCase):
    def make_client(
        self, responder=None, records=None, *, device=None, clock=None, retries=1
    ):
        device = device or FakeDevice(responder)
        module = FakeHid([record()] if records is None else records, device)
        transport = cli.open_transport(module, clock=clock or cli.time.monotonic)
        self.addCleanup(transport.close)
        return (
            cli.RuntimeMacroClient(transport, timeout_ms=10, retries=retries),
            device,
        )

    def test_device_discovery_strictly_filters_usage_vid_pid_and_path_types(self):
        records = [
            record(path="wrong-page", usage_page=0xFF5F),
            record(path="wrong-usage", usage=0x60),
            record(path="wrong-vid", vid=0x1111),
            record(path="wrong-pid", pid=0x2222),
            record(path=b"/dev/runtime", vid=0x1111, pid=0x2222),
        ]
        selected = cli.find_device(
            FakeHid(records, FakeDevice()),
            path="/dev/runtime",
            vid=0x1111,
            pid=0x2222,
        )
        self.assertEqual(b"/dev/runtime", selected["path"])

    def test_device_rejects_zero_and_multiple_and_allows_exact_path(self):
        with self.assertRaises(cli.DeviceError):
            cli.find_device(FakeHid([], FakeDevice()))
        with self.assertRaises(cli.DeviceError) as ctx:
            cli.find_device(
                FakeHid([record(path="a"), record(path="b")], FakeDevice())
            )
        self.assertIn("--path", str(ctx.exception))
        selected = cli.find_device(
            FakeHid([record(path="a"), record(path="b")], FakeDevice()),
            path=b"b",
        )
        self.assertEqual("b", selected["path"])

    def test_explicit_path_accepts_missing_usage_metadata(self):
        unknown_usage = record(
            path=b"/dev/hidraw-runtime",
            usage_page=0,
            usage=0,
        )
        selected = cli.find_device(
            FakeHid([unknown_usage], FakeDevice()),
            path="/dev/hidraw-runtime",
        )
        self.assertEqual(b"/dev/hidraw-runtime", selected["path"])

        with self.assertRaises(cli.DeviceError) as ctx:
            cli.find_device(FakeHid([unknown_usage], FakeDevice()))
        self.assertIn("--path", str(ctx.exception))

    def test_open_path_failure_closes_device(self):
        device = FakeDevice(open_error=OSError("permission denied"))
        module = FakeHid([record()], device)
        with self.assertRaises(cli.DeviceError) as ctx:
            cli.open_transport(module)
        self.assertIn("无法打开", str(ctx.exception))
        self.assertTrue(device.closed)

    def test_fixed_clear_request_bytes_and_report_id(self):
        def reply(device, wire):
            self.assertEqual(33, len(wire))
            self.assertEqual(b"\0", wire[:1])
            expected = bytes([2, 4, 0, 0, 2, 0, 0, 0, 0, 0] + [0] * 22)
            self.assertEqual(expected, wire[1:])
            device.read_queue.append(response(wire[1:], offset=0, total=0))

        client, device = self.make_client(reply)
        client.clear_slot(2)
        self.assertEqual(1, len(device.writes))
        self.assertEqual(33, len(device.writes[0]))

    def test_normalize_rejects_short_long_and_nonzero_report_id(self):
        for data in (b"\0" * 31, b"\0" * 34, b"\1" + b"\0" * 32):
            with self.subTest(length=len(data), prefix=data[:1]), self.assertRaises(
                cli.ProtocolError
            ):
                cli.HidTransport.normalize_read(data)
        self.assertEqual(b"\0" * 32, cli.HidTransport.normalize_read(b"\0" * 33))

    def test_write_rejects_partial_invalid_and_backend_failure(self):
        frame = bytes(32)
        for result in (31, 34, -1):
            with self.subTest(result=result):
                transport = cli.HidTransport(FakeDevice(write_result=result))
                with self.assertRaises(cli.TransportError):
                    transport.write_frame(frame)
        device = FakeDevice()
        device.responder = lambda _device, _wire: (_ for _ in ()).throw(
            OSError("write failed")
        )
        transport = cli.HidTransport(device)
        with self.assertRaises(cli.TransportError):
            transport.write_frame(frame)

    def test_read_timeout_is_reported_with_requested_timeout(self):
        device = FakeDevice()
        transport = cli.HidTransport(device)
        with self.assertRaises(cli.TimeoutError):
            transport.read_frame(7)
        self.assertEqual([(33, 7)], device.read_calls)

    def test_stale_responses_do_not_extend_monotonic_deadline(self):
        clock = FakeClock()
        device = FakeDevice()

        def stale_read(dev, _length, _timeout_ms):
            request = dev.writes[0][1:]
            stale = bytearray(response(request, total=0))
            stale[2] = (request[2] + 1) & 0xFF
            clock.advance(0.004)
            return bytes(stale)

        device.read_handler = stale_read
        transport = cli.HidTransport(device, clock=clock)
        request = bytes([2, 4, 0, 0, 0, 0, 0, 0, 0, 0] + [0] * 22)
        with self.assertRaises(cli.TimeoutError):
            transport.exchange(request, 10)
        self.assertGreaterEqual(clock.now, 0.010)
        self.assertLessEqual(len(device.read_calls), 4)

    def test_matching_malformed_response_fails_immediately(self):
        def reply(device, wire):
            device.read_queue.append(response(wire[1:], total=0, bad_tail=True))

        client, _ = self.make_client(reply, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.get_slot(0)

    def test_stale_response_is_discarded_before_matching_response(self):
        def reply(device, wire):
            request = wire[1:]
            stale = bytearray(response(request, total=0))
            stale[2] = (request[2] + 1) & 0xFF
            device.read_queue.extend([bytes(stale), response(request, total=0)])

        client, _ = self.make_client(reply)
        self.assertEqual(b"", client.get_slot(0))

    def test_get_empty_slot_and_22_23_byte_boundaries(self):
        client, _ = self.make_client(page_responder(b""), retries=0)
        self.assertEqual(b"", client.get_slot(0))

        for logical in (b"x" * 22, b"y" * 23):
            with self.subTest(length=len(logical)):
                client, device = self.make_client(
                    page_responder(logical), retries=0
                )
                self.assertEqual(logical, client.get_slot(0))
                self.assertEqual(
                    [0] if len(logical) == 22 else [0, 22],
                    [request_field(wire, cli.OFFSET_OFFSET, 2) for wire in device.writes],
                )

    def test_get_rejects_wrong_offset_total_change_and_zero_progress(self):
        def wrong_offset(device, wire):
            device.read_queue.append(
                response(wire[1:], payload=b"x", offset=1, total=1)
            )

        client, _ = self.make_client(wrong_offset, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.get_slot(0)

        calls = []

        def changing_total(device, wire):
            request = wire[1:]
            calls.append(request)
            offset = int.from_bytes(request[6:8], "little")
            if len(calls) == 1:
                device.read_queue.append(
                    response(request, payload=b"a" * 22, offset=0, total=23)
                )
            else:
                device.read_queue.append(
                    response(request, payload=b"b", offset=offset, total=24)
                )

        client, _ = self.make_client(changing_total, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.get_slot(0)

        def no_progress(device, wire):
            device.read_queue.append(response(wire[1:], payload=b"", total=1))

        client, _ = self.make_client(no_progress, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.get_slot(0)

    def test_get_rejects_invalid_text(self):
        def reply(device, wire):
            device.read_queue.append(
                response(wire[1:], payload=b"\x80", total=1)
            )

        client, _ = self.make_client(reply, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.get_slot(0)

    def test_list_empty_slots_and_crosses_page(self):
        logical = bytes([12]) + b"".join(i.to_bytes(2, "little") for i in range(12))
        client, _ = self.make_client(page_responder(logical), retries=0)
        result = client.list_slots()
        self.assertEqual(
            [(i, i) for i in range(12)],
            [(item.slot, item.length) for item in result],
        )

        empty = bytes([3, 0, 0, 0, 0, 0, 0])
        client, _ = self.make_client(page_responder(empty), retries=0)
        self.assertEqual(
            [(0, 0), (1, 0), (2, 0)],
            [(item.slot, item.length) for item in client.list_slots()],
        )

    def test_list_rejects_wrong_offset_total_change_zero_progress_and_bad_result(self):
        def wrong_offset(device, wire):
            request = wire[1:]
            device.read_queue.append(
                response(request, payload=b"\x01\x00\x00", offset=1, total=3)
            )

        client, _ = self.make_client(wrong_offset, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.list_slots()

        calls = []

        def changing_total(device, wire):
            request = wire[1:]
            calls.append(request)
            offset = int.from_bytes(request[6:8], "little")
            total = 23 if len(calls) == 1 else 24
            chunk = b"a" * min(22, total - offset)
            device.read_queue.append(
                response(request, payload=chunk, offset=offset, total=total)
            )

        client, _ = self.make_client(changing_total, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.list_slots()

        def no_progress(device, wire):
            device.read_queue.append(response(wire[1:], payload=b"", total=3))

        client, _ = self.make_client(no_progress, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.list_slots()

        malformed = bytes([2, 0])
        client, _ = self.make_client(page_responder(malformed), retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.list_slots()

    def test_set_boundaries_and_canonical_little_endian_frames(self):
        seen = []

        def reply(device, wire):
            request = wire[1:]
            offset = int.from_bytes(request[6:8], "little")
            payload_length = request[5]
            total = int.from_bytes(request[8:10], "little")
            seen.append((request[2], offset, payload_length, total))
            device.read_queue.append(
                response(
                    request,
                    offset=offset + payload_length,
                    total=total,
                )
            )

        client, device = self.make_client(reply, retries=0)
        client.set_slot(7, b"A" * 23)
        expected_first = bytearray(32)
        expected_first[:10] = bytes([2, 3, 0, 0, 7, 22, 0, 0, 23, 0])
        expected_first[10:32] = b"A" * 22
        expected_second = bytearray(32)
        expected_second[:10] = bytes([2, 3, 0, 0, 7, 1, 22, 0, 23, 0])
        expected_second[10:11] = b"A"
        self.assertEqual(b"\0" + bytes(expected_first), device.writes[0])
        self.assertEqual(b"\0" + bytes(expected_second), device.writes[1])
        self.assertEqual([(0, 0, 22, 23), (0, 22, 1, 23)], seen)

        for length, expected_chunks in (
            (0, [0]),
            (1, [1]),
            (22, [22]),
            (23, [22, 1]),
            (64, [22, 22, 20]),
        ):
            with self.subTest(length=length):
                client, device = self.make_client(
                    reply, retries=0
                )
                device.writes.clear()
                client.set_slot(0, b"b" * length)
                frames = [wire[1:] for wire in device.writes]
                self.assertEqual(expected_chunks, [frame[5] for frame in frames])
                self.assertEqual(
                    [0, 22, 44][: len(frames)],
                    [int.from_bytes(frame[6:8], "little") for frame in frames],
                )
                self.assertTrue(
                    all(
                        frame[10 + frame[5] :] == b"\0" * (22 - frame[5])
                        for frame in frames
                    )
                )

    def test_set_rejects_bad_ack_offset_and_total(self):
        def bad_offset(device, wire):
            request = wire[1:]
            device.read_queue.append(
                response(request, offset=request[6] + request[5] + 1, total=1)
            )

        client, device = self.make_client(bad_offset, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.set_slot(0, b"x")
        self.assertEqual(1, len(device.writes))

        def bad_total(device, wire):
            request = wire[1:]
            device.read_queue.append(
                response(request, offset=request[6] + request[5], total=2)
            )

        client, device = self.make_client(bad_total, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.set_slot(0, b"x")
        self.assertEqual(1, len(device.writes))

    def test_set_rejects_all_unsupported_ascii_bytes_before_writing(self):
        for data in (b"\x00", b"\x07", b"\x7f", b"\x80", "é".encode()):
            with self.subTest(data=data):
                device = FakeDevice()
                client, _ = self.make_client(device=device, retries=0)
                with self.assertRaises(ValueError):
                    client.set_slot(0, data)
                self.assertEqual([], device.writes)

    def test_set_middle_timeout_restarts_complete_transaction_with_new_id(self):
        seen = []

        def reply(device, wire):
            request = wire[1:]
            offset = int.from_bytes(request[6:8], "little")
            payload_length = request[5]
            seen.append((request[2], offset, payload_length))
            if len(seen) == 2:
                return
            device.read_queue.append(
                response(request, offset=offset + payload_length, total=23)
            )

        client, _ = self.make_client(reply, retries=1)
        client.set_slot(1, b"c" * 23)
        self.assertEqual(
            [(0, 0, 22), (0, 22, 1), (1, 0, 22), (1, 22, 1)],
            seen,
        )

    def test_set_nonrecoverable_status_is_not_retried(self):
        for status in (cli.STATUS_STORAGE_ERROR, cli.STATUS_INVALID_TEXT):
            with self.subTest(status=status):
                calls = []

                def reply(device, wire, *, calls=calls, status=status):
                    calls.append(wire)
                    device.read_queue.append(response(wire[1:], status=status))

                client, _ = self.make_client(reply, retries=3)
                with self.assertRaises(cli.RemoteError) as ctx:
                    client.set_slot(0, b"x")
                self.assertEqual(status, ctx.exception.status)
                self.assertEqual(1, len(calls))

    def test_clear_status_error_is_not_retried(self):
        calls = []

        def reply(device, wire):
            calls.append(wire)
            device.read_queue.append(
                response(wire[1:], status=cli.STATUS_BAD_SLOT)
            )

        client, _ = self.make_client(reply, retries=3)
        with self.assertRaises(cli.RemoteError) as ctx:
            client.clear_slot(0)
        self.assertEqual(cli.STATUS_BAD_SLOT, ctx.exception.status)
        self.assertEqual(1, len(calls))

    def test_clear_timeout_retries_with_new_request_id(self):
        calls = []

        def reply(device, wire):
            calls.append(wire)
            if len(calls) > 1:
                device.read_queue.append(response(wire[1:]))

        client, device = self.make_client(reply, retries=1)
        client.clear_slot(0)
        self.assertEqual([0, 1], [request_field(wire, 2) for wire in calls])
        client.transport.close()
        self.assertTrue(device.closed)

    def test_safe_display_and_ascii_input(self):
        self.assertEqual("a\\n\\t\\b\\\\", cli.visible_text(b"a\n\t\b\\"))
        for data in (b"\n", b"\t", b"\b", b"~", b" "):
            cli.validate_ascii(data)
        with self.assertRaises(ValueError):
            cli.validate_ascii(b"\xff")


class AuthClientTests(unittest.TestCase):
    def make_client(
        self, responder=None, records=None, *, device=None, clock=None, retries=1
    ):
        device = device or FakeDevice(responder)
        module = FakeHid([record()] if records is None else records, device)
        transport = cli.open_transport(module, clock=clock or cli.time.monotonic)
        self.addCleanup(transport.close)
        return (
            cli.RuntimeMacroClient(transport, timeout_ms=10, retries=retries),
            device,
        )

    def test_auth_info_open_and_protected_have_strict_public_layout(self):
        def open_reply(device, wire):
            request = wire[1:]
            expected = bytes([2, cli.OPCODE_AUTH_INFO, 0, 0, 0xFF, 0, 0, 0, 0, 0]) + bytes(22)
            self.assertEqual(expected, request)
            device.read_queue.append(
                response(
                    request,
                    payload=auth_info_payload(configured=False),
                    offset=0,
                    total=cli.AUTH_INFO_SIZE,
                )
            )

        client, device = self.make_client(open_reply, retries=0)
        info = client.auth_info()
        self.assertEqual("OPEN", info.state)
        self.assertTrue(info.open)
        self.assertFalse(info.authenticated)
        self.assertEqual(cli.AUTH_ITERATIONS_DEFAULT, info.iterations)
        self.assertEqual(bytes(16), info.salt)
        self.assertEqual(1, len(device.writes))

        salt = b"0123456789abcdef"

        def protected_reply(device, wire):
            request = wire[1:]
            device.read_queue.append(
                response(
                    request,
                    payload=auth_info_payload(
                        configured=True,
                        authenticated=True,
                        iterations=100000,
                        salt=salt,
                    ),
                    offset=0,
                    total=cli.AUTH_INFO_SIZE,
                )
            )

        client, _ = self.make_client(protected_reply, retries=0)
        info = client.auth_info()
        self.assertEqual("PROTECTED", info.state)
        self.assertTrue(info.protected)
        self.assertTrue(info.authenticated)
        self.assertEqual(100000, info.iterations)
        self.assertEqual(salt, info.salt)

    def test_auth_info_rejects_bad_length_flags_kdf_and_metadata(self):
        cases = [
            {"payload": bytes(21), "total": 21},
            {
                "payload": bytes([0x04, cli.AUTH_KDF_ID])
                + (600000).to_bytes(4, "little")
                + bytes(16),
                "total": 22,
            },
            {
                "payload": bytes([0, 99])
                + (600000).to_bytes(4, "little")
                + bytes(16),
                "total": 22,
            },
            {
                "payload": auth_info_payload(
                    configured=False, salt=b"x" + bytes(15)
                ),
                "total": 22,
            },
            {
                "payload": auth_info_payload(
                    configured=True, iterations=100000, salt=bytes(16)
                ),
                "total": 22,
            },
        ]
        for case in cases:
            with self.subTest(case=case):
                def reply(device, wire, *, case=case):
                    device.read_queue.append(
                        response(
                            wire[1:],
                            payload=case["payload"],
                            total=case["total"],
                        )
                    )

                client, _ = self.make_client(reply, retries=0)
                with self.assertRaises(cli.ProtocolError):
                    client.auth_info()

    def test_nfc_utf8_pbkdf2_known_vector_and_validation(self):
        salt = b"1234567890abcdef"
        self.assertEqual(b"\xc3\xa9", cli.normalize_password("e\u0301"))
        self.assertEqual(
            "62bd92e0e20fec971905b7a53c6f1a86f5091a4d60704c7827ca8a9464707172",
            cli.derive_key("e\u0301", salt, 100000).hex(),
        )
        with self.assertRaises(ValueError):
            cli.normalize_password("")
        with self.assertRaises(ValueError):
            cli.derive_key("x", bytes(16), 100000)
        with self.assertRaises(ValueError):
            cli.derive_key("x", salt, cli.AUTH_ITERATIONS_MIN - 1)
        with self.assertRaises(ValueError):
            cli.derive_key("x", salt, cli.AUTH_ITERATIONS_MAX + 1)

    def test_authenticate_emits_complete_v2_wire_sequence(self):
        password = "e\u0301"
        salt = b"0123456789abcdef"
        nonce = bytes(range(1, 17))
        key = hashlib.pbkdf2_hmac("sha256", "é".encode(), salt, 100000, dklen=32)
        expected_proof = hmac.new(
            key, cli.AUTH_DOMAIN + nonce, hashlib.sha256
        ).digest()[:16]
        seen = []

        def reply(device, wire):
            request = wire[1:]
            seen.append(request)
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                self.assertEqual(expected_proof, request[10:26])
                self.assertEqual(16, request[5])
                self.assertEqual(16, int.from_bytes(request[8:10], "little"))
                device.read_queue.append(empty_ack(request))
            else:
                self.fail(f"unexpected opcode {request[1]}")

        client, device = self.make_client(reply, retries=0)
        result = client.authenticate(password)
        self.assertEqual("PROTECTED", result.state)
        self.assertTrue(result.authenticated)
        self.assertEqual(
            [cli.OPCODE_AUTH_INFO, cli.OPCODE_AUTH_CHALLENGE, cli.OPCODE_AUTH_PROVE],
            [request[1] for request in seen],
        )
        self.assertEqual([0, 1, 2], [request[2] for request in seen])
        self.assertEqual(3, len(device.writes))

    def test_authenticate_wrong_password_rate_limit_and_bad_challenge(self):
        salt = b"0123456789abcdef"

        def wrong_password(device, wire):
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=b"n" * 16, total=16))
            else:
                device.read_queue.append(response(request, status=cli.STATUS_AUTH_FAILED))

        client, _ = self.make_client(wrong_password, retries=0)
        with self.assertRaises(cli.RemoteError) as ctx:
            client.authenticate("wrong")
        self.assertEqual(cli.STATUS_AUTH_FAILED, ctx.exception.status)

        def rate_limited(device, wire):
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            else:
                device.read_queue.append(response(request, status=cli.STATUS_RATE_LIMITED))

        client, device = self.make_client(rate_limited, retries=2)
        with self.assertRaises(cli.RemoteError) as ctx:
            client.authenticate("wrong")
        self.assertEqual(cli.STATUS_RATE_LIMITED, ctx.exception.status)
        self.assertEqual(2, len(device.writes))

        def malformed_challenge(device, wire):
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            else:
                device.read_queue.append(response(request, payload=b"n" * 15, total=15))

        client, device = self.make_client(malformed_challenge, retries=0)
        with self.assertRaises(cli.ProtocolError):
            client.authenticate("password")
        self.assertEqual(2, len(device.writes))

    def test_auth_prove_timeout_restarts_with_new_nonce_id_and_proof(self):
        salt = b"0123456789abcdef"
        nonces = [bytes(range(1, 17)), bytes(range(17, 33))]
        proof_values = []
        challenge_count = 0
        prove_count = 0

        def reply(device, wire):
            nonlocal challenge_count, prove_count
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                nonce = nonces[challenge_count]
                challenge_count += 1
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                proof_values.append(request[10:26])
                prove_count += 1
                if prove_count == 2:
                    device.read_queue.append(empty_ack(request))
            else:
                self.fail(f"unexpected opcode {request[1]}")

        client, device = self.make_client(reply, retries=1)
        result = client.authenticate("password")
        self.assertTrue(result.authenticated)
        self.assertEqual(2, challenge_count)
        self.assertEqual(2, prove_count)
        self.assertEqual(
            [
                cli.OPCODE_AUTH_INFO,
                cli.OPCODE_AUTH_CHALLENGE,
                cli.OPCODE_AUTH_PROVE,
                cli.OPCODE_AUTH_INFO,
                cli.OPCODE_AUTH_CHALLENGE,
                cli.OPCODE_AUTH_PROVE,
            ],
            [opcode(wire) for wire in device.writes],
        )
        self.assertEqual([0, 1, 2, 3, 4, 5], [request_field(wire, 2) for wire in device.writes])
        self.assertNotEqual(proof_values[0], proof_values[1])

    def test_password_set_open_uses_22_22_8_same_id_then_reauthenticates(self):
        salt = b"abcdefghijklmnop"
        iterations = 100000
        password = "n\u0303ew"
        normalized = "ñew".encode()
        key = hashlib.pbkdf2_hmac("sha256", normalized, salt, iterations, dklen=32)
        credential = iterations.to_bytes(4, "little") + salt + key
        nonce = b"N" * 16
        auth_info_count = 0
        password_frames = []

        def reply(device, wire):
            nonlocal auth_info_count
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                auth_info_count += 1
                configured = auth_info_count > 1
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=configured,
                            iterations=iterations if configured else 600000,
                            salt=salt if configured else bytes(16),
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_PASSWORD_SET:
                password_frames.append(request)
                offset = int.from_bytes(request[6:8], "little")
                length = request[5]
                device.read_queue.append(
                    response(request, offset=offset + length, total=52)
                )
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                expected = hmac.new(
                    key, cli.AUTH_DOMAIN + nonce, hashlib.sha256
                ).digest()[:16]
                self.assertEqual(expected, request[10:26])
                device.read_queue.append(empty_ack(request))
            else:
                self.fail(f"unexpected opcode {request[1]}")

        client, device = self.make_client(reply, retries=0)
        with mock.patch.object(cli.secrets, "token_bytes", return_value=salt):
            result = client.set_password(password, iterations=iterations)
        self.assertTrue(result.authenticated)
        self.assertEqual([22, 22, 8], [request[5] for request in password_frames])
        self.assertEqual([0, 22, 44], [int.from_bytes(request[6:8], "little") for request in password_frames])
        self.assertEqual(1, len({request[2] for request in password_frames}))
        self.assertEqual(credential, b"".join(request[10 : 10 + request[5]] for request in password_frames))
        self.assertEqual(
            [cli.OPCODE_AUTH_INFO, cli.OPCODE_PASSWORD_SET, cli.OPCODE_PASSWORD_SET, cli.OPCODE_PASSWORD_SET,
             cli.OPCODE_AUTH_INFO, cli.OPCODE_AUTH_INFO, cli.OPCODE_AUTH_CHALLENGE, cli.OPCODE_AUTH_PROVE],
            [opcode(wire) for wire in device.writes],
        )

    def test_password_set_requires_management_window_when_protected(self):
        salt = b"0123456789abcdef"

        def reply(device, wire):
            request = wire[1:]
            device.read_queue.append(
                response(
                    request,
                    payload=auth_info_payload(
                        configured=True, iterations=100000, salt=salt
                    ),
                    total=22,
                )
            )

        client, device = self.make_client(reply, retries=0)
        with self.assertRaises(cli.RemoteError) as ctx:
            client.set_password("new")
        self.assertEqual(cli.STATUS_AUTH_REQUIRED, ctx.exception.status)
        self.assertEqual([cli.OPCODE_AUTH_INFO], [opcode(wire) for wire in device.writes])

    def test_password_set_middle_timeout_restarts_from_zero_with_new_id(self):
        salt = b"0123456789abcdef"
        iterations = 100000
        nonce = b"N" * 16
        auth_info_count = 0
        password_frames = []
        timed_out = False
        key = hashlib.pbkdf2_hmac("sha256", b"new", salt, iterations, dklen=32)

        def reply(device, wire):
            nonlocal auth_info_count, timed_out
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                auth_info_count += 1
                configured = auth_info_count > 1
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=configured,
                            iterations=iterations if configured else 600000,
                            salt=salt if configured else bytes(16),
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_PASSWORD_SET:
                password_frames.append(request)
                offset = int.from_bytes(request[6:8], "little")
                if offset == 22 and not timed_out:
                    timed_out = True
                    return
                device.read_queue.append(response(request, offset=offset + request[5], total=52))
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                self.assertEqual(
                    hmac.new(key, cli.AUTH_DOMAIN + nonce, hashlib.sha256).digest()[:16],
                    request[10:26],
                )
                device.read_queue.append(empty_ack(request))

        client, _ = self.make_client(reply, retries=1)
        with mock.patch.object(cli.secrets, "token_bytes", return_value=salt):
            client.set_password("new", iterations=iterations)
        self.assertEqual(
            [(1, 0), (1, 22), (2, 0), (2, 22), (2, 44)],
            [(request[2], int.from_bytes(request[6:8], "little")) for request in password_frames],
        )

    def test_password_set_final_timeout_confirms_matching_salt_without_resend(self):
        salt = b"0123456789abcdef"
        iterations = 100000
        nonce = b"N" * 16
        auth_info_count = 0
        password_frames = []
        key = hashlib.pbkdf2_hmac("sha256", b"new", salt, iterations, dklen=32)

        def reply(device, wire):
            nonlocal auth_info_count
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                auth_info_count += 1
                configured = auth_info_count > 1
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=configured,
                            iterations=iterations if configured else 600000,
                            salt=salt if configured else bytes(16),
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_PASSWORD_SET:
                password_frames.append(request)
                offset = int.from_bytes(request[6:8], "little")
                if offset + request[5] == 52:
                    return
                device.read_queue.append(response(request, offset=offset + request[5], total=52))
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                self.assertEqual(
                    hmac.new(key, cli.AUTH_DOMAIN + nonce, hashlib.sha256).digest()[:16],
                    request[10:26],
                )
                device.read_queue.append(empty_ack(request))

        client, _ = self.make_client(reply, retries=1)
        with mock.patch.object(cli.secrets, "token_bytes", return_value=salt):
            result = client.set_password("new", iterations=iterations)
        self.assertTrue(result.authenticated)
        self.assertEqual(3, len(password_frames))
        self.assertEqual(1, len([frame for frame in password_frames if int.from_bytes(frame[6:8], "little") == 44]))

    def test_password_set_final_timeout_with_old_salt_fails_without_resend(self):
        new_salt = b"0123456789abcdef"
        old_salt = b"fedcba9876543210"
        auth_info_count = 0
        password_frames = []

        def reply(device, wire):
            nonlocal auth_info_count
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                auth_info_count += 1
                salt = bytes(16) if auth_info_count == 1 else old_salt
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=auth_info_count > 1,
                            iterations=100000 if auth_info_count > 1 else 600000,
                            salt=salt,
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_PASSWORD_SET:
                password_frames.append(request)
                offset = int.from_bytes(request[6:8], "little")
                if offset + request[5] != 52:
                    device.read_queue.append(response(request, offset=offset + request[5], total=52))

        client, _ = self.make_client(reply, retries=1)
        with mock.patch.object(cli.secrets, "token_bytes", return_value=new_salt), self.assertRaises(
            cli.PasswordSetUnconfirmed
        ):
            client.set_password("new", iterations=100000)
        self.assertEqual(3, len(password_frames))

    def test_lock_retries_with_canonical_empty_ack(self):
        calls = []

        def reply(device, wire):
            calls.append(wire[1:])
            if len(calls) > 1:
                device.read_queue.append(empty_ack(wire[1:]))

        client, device = self.make_client(reply, retries=1)
        client.lock()
        self.assertEqual([cli.OPCODE_LOCK, cli.OPCODE_LOCK], [request[1] for request in calls])
        self.assertEqual([0, 1], [request[2] for request in calls])
        self.assertTrue(all(request[4] == 0xFF and request[5:10] == bytes(5) for request in calls))
        self.assertEqual(2, len(device.writes))

    def test_v1_bad_version_is_explicit_and_never_falls_back(self):
        def reply(device, wire):
            request = wire[1:]
            device.read_queue.append(response(request, status=cli.STATUS_BAD_VERSION))

        client, device = self.make_client(reply, retries=3)
        with self.assertRaises(cli.LegacyFirmwareError) as ctx:
            client.auth_info()
        self.assertIn("v1", str(ctx.exception))
        self.assertIn("不会自动回退", str(ctx.exception))
        self.assertEqual(1, len(device.writes))


class CliTests(unittest.TestCase):
    def test_cli_parses_decimal_and_hex_vid_pid(self):
        args = cli.make_parser().parse_args(
            ["--vid", "4660", "--pid", "0x5678", "list"]
        )
        self.assertEqual(4660, args.vid)
        self.assertEqual(0x5678, args.pid)

    def test_cli_invalid_option_and_timeout_have_parser_exit_code(self):
        with self.assertRaises(SystemExit) as ctx:
            cli.make_parser().parse_args(["list", "--unknown"])
        self.assertEqual(2, ctx.exception.code)

        with self.assertRaises(SystemExit) as ctx:
            cli.main(["--timeout-ms", "0", "list"], hid_module=FakeHid([], FakeDevice()))
        self.assertEqual(2, ctx.exception.code)

    def test_cli_invalid_slot_returns_error_and_closes_device(self):
        device = FakeDevice()
        module = FakeHid([record()], device)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            result = cli.main(["get", "-1"], hid_module=module)
        self.assertEqual(1, result)
        self.assertIn("between 0 and 254", err.getvalue())
        self.assertTrue(device.closed)

    def test_cli_set_rejects_bad_text_with_error(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            result = cli.main(
                ["set", "0", "--text", "bad\x00"],
                hid_module=FakeHid([record()], FakeDevice()),
            )
        self.assertEqual(1, result)
        self.assertIn("0x00", err.getvalue())

    def test_cli_list_output_filters_vid_pid_and_closes(self):
        def reply(device, wire):
            request = wire[1:]
            logical = bytes([1, 3, 0])
            device.read_queue.append(
                response(request, payload=logical, offset=0, total=3)
            )

        device = FakeDevice(reply)
        module = FakeHid(
            [record(vid=0x1111), record(vid=0x1234, pid=0x5678)], device
        )
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = cli.main(
                ["--vid", "4660", "--pid", "0x5678", "list"],
                hid_module=module,
            )
        self.assertEqual(0, result)
        self.assertEqual("0\t3\n", out.getvalue())
        self.assertTrue(device.closed)

    def test_cli_set_input_is_mutually_exclusive(self):
        with self.assertRaises(SystemExit):
            cli.make_parser().parse_args(["set", "0", "--text", "a", "--stdin"])

    def test_cli_auth_info_reports_state_without_salt_or_secret(self):
        def reply(device, wire):
            request = wire[1:]
            self.assertEqual(cli.OPCODE_AUTH_INFO, request[1])
            device.read_queue.append(
                response(
                    request,
                    payload=auth_info_payload(configured=False),
                    total=cli.AUTH_INFO_SIZE,
                )
            )

        device = FakeDevice(reply)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            result = cli.main(
                ["auth-info"], hid_module=FakeHid([record()], device)
            )
        self.assertEqual(0, result)
        self.assertEqual(
            "state=OPEN authenticated=no iterations=600000\n", out.getvalue()
        )
        self.assertNotIn("salt", out.getvalue())

    def test_cli_login_uses_getpass_and_does_not_print_password(self):
        salt = b"0123456789abcdef"
        nonce = b"N" * 16
        key = hashlib.pbkdf2_hmac("sha256", b"password", salt, 100000, dklen=32)

        def reply(device, wire):
            request = wire[1:]
            if request[1] == cli.OPCODE_AUTH_INFO:
                device.read_queue.append(
                    response(
                        request,
                        payload=auth_info_payload(
                            configured=True, iterations=100000, salt=salt
                        ),
                        total=22,
                    )
                )
            elif request[1] == cli.OPCODE_AUTH_CHALLENGE:
                device.read_queue.append(response(request, payload=nonce, total=16))
            elif request[1] == cli.OPCODE_AUTH_PROVE:
                expected = hmac.new(
                    key, cli.AUTH_DOMAIN + nonce, hashlib.sha256
                ).digest()[:16]
                self.assertEqual(expected, request[10:26])
                device.read_queue.append(empty_ack(request))
            else:
                self.fail(f"unexpected opcode {request[1]}")

        device = FakeDevice(reply)
        out = io.StringIO()
        with mock.patch.object(cli, "getpass", return_value="password"), contextlib.redirect_stdout(out):
            result = cli.main(
                ["login"], hid_module=FakeHid([record()], device)
            )
        self.assertEqual(0, result)
        self.assertEqual("authenticated\n", out.getvalue())
        self.assertNotIn("password", out.getvalue())

    def test_cli_set_password_mismatch_or_empty_sends_no_hid_request(self):
        for answers in (("new", "different"), ("", "")):
            with self.subTest(answers=answers):
                device = FakeDevice()
                err = io.StringIO()
                with mock.patch.object(cli, "getpass", side_effect=answers), contextlib.redirect_stderr(err):
                    result = cli.main(
                        ["set-password"],
                        hid_module=FakeHid([record()], device),
                    )
                self.assertEqual(1, result)
                self.assertEqual([], device.writes)
                self.assertTrue(err.getvalue())

    def test_cli_has_no_password_command_line_option(self):
        with self.assertRaises(SystemExit):
            cli.make_parser().parse_args(["login", "--password", "secret"])

    def test_cli_missing_hidapi_is_clear(self):
        # Exercise lazy import behavior without requiring hidapi to be
        # installed in the host test environment.
        old = cli._load_hid
        cli._load_hid = lambda: (_ for _ in ()).throw(cli.DeviceError("缺少 hidapi"))
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(1, cli.main(["list"]))
            self.assertIn("缺少 hidapi", err.getvalue())
        finally:
            cli._load_hid = old


if __name__ == "__main__":
    unittest.main()
