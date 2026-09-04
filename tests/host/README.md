# Runtime macro host tests

`run.sh` builds and runs small C unit tests for the slot RAM store, Settings
handler, ASCII mapping, delayable-work executor, and the transport-independent
32-byte protocol. The Settings test uses a pthread-backed mutex stub and a
deterministic backend gate to verify that concurrent slot updates serialize RAM
and persistence order without requiring a board-specific flash backend.

The executor test drives the work state machine directly and covers the full
US ASCII map, supported controls, maximum-length snapshots, busy behavior,
and schedule/error recovery. The protocol test covers fixed wire constants,
LIST/GET chunking, atomic SET staging, duplicate/restart recovery, malformed
frames, and storage errors. USB transport tests use host HID/device stubs to
verify descriptor bytes, callback queueing, work scheduling, IN endpoint
throttling, and initialization failures without requiring a USB device.

A full ZMK/native_sim test is still needed for end-to-end Settings/NVS
persistence on a target configuration; the host tests are intentionally limited
to module logic and transport control flow.
