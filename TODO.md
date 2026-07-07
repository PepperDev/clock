# TODO

- [ ] `try_refetch_weather` must call `io_cancel_all(…, CANCEL_WEATHER)` before closing the weather connection fd, so the I/O thread does not poll on a recycled file descriptor.
- [ ] add assertion `slot->state == DNS_IDLE` in `start_dns` before overwriting slot state and thread handle.
- [x] guard CPU temp display so `-1°C` is not shown when the sensor is unavailable.
- [x] aggregate motherboard fans/temps across all hwmon dirs into one fan line and one temp line instead of per-dir blocks.
- [ ] change GPU temp sentinel so `0°C` (a valid reading) is not silently hidden as "no sensor".
- [ ] remove unused `sto_pct` field from `struct clock_state`.
- [ ] remove dead `sto_temp` data flow (written to `clock_state` from disk.c, never consumed by display).
- [ ] add `wc > 0` guard in `sixel_emit_overlay` so the sidebar is not positioned when the widget list is empty (ascii path already has this guard).
- [ ] prepend 🌐 icon (`\xf0\x9f\x8c\x90`) to the first NIC line of the NET widget in TTY modes, matching the spec's icon column for NET.
