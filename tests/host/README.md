# Runtime macro host tests

`run.sh` builds and runs small C unit tests for the slot RAM store, Settings
handler, ASCII mapping, and delayable-work executor. The Settings test uses a
pthread-backed mutex stub and a deterministic backend gate to verify that
concurrent slot updates serialize RAM and persistence order without requiring
a board-specific flash backend.

The executor test drives the work state machine directly and covers the full
US ASCII map, supported controls, maximum-length snapshots, busy behavior,
and schedule/error recovery. A full ZMK/native_sim test is still needed for
end-to-end Settings/NVS persistence on a target configuration; the host tests
are intentionally limited to module logic.
