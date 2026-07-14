# clock

A real-time terminal dashboard with a large digital clock and machine monitoring sidebar.

No external binaries or runtime dependencies — all data is gathered from `/proc` and `/sys` via direct syscalls. The binary is statically linked.

## Features

- Large digital clock (ASCII block art or sixel graphics)
- Machine monitoring sidebar with 11 widgets (see below)

All widgets are always-on by default. Use `-w` to select a custom set.

## Installation

### Pre-built binaries

Download a static binary for your architecture from the
[releases page](https://github.com/PepperDev/clock/releases).

### Build from source

```
git clone https://github.com/PepperDev/clock
cd clock
make
```

Requires a C99 compiler and pthreads. A static build is produced by default.
If `musl-gcc` is available on `$PATH` it is used automatically; otherwise
falls back to `gcc`.

## Usage

```
clock [OPTIONS] [auto|text|ascii|sixel]
```

### Display modes

| Mode | Description |
|------|-------------|
| `auto` (default) | Probe sixel support; fall back to ascii if unavailable |
| `text` | Machine-parseable plain text, no ANSI escapes |
| `ascii` | Unicode block-art clock with sidebar |
| `sixel` | Bitmapped clock rendered via sixel graphics |

### Options

| Flag | Description |
|------|-------------|
| `-h`, `--help` | Print usage and exit |
| `-o`, `--once` | Print output once and exit (no 1-second loop) |
| `-S`, `--sunday-start` | Calendar week starts on Sunday |
| `-w`, `--widgets` `<list>` | Comma-separated widget list (e.g. `-w CPU,MEM,NET`). Empty string disables the sidebar for a full-screen clock. |
| `-I`, `--ip-refresh` `<sec>` | Public IP refresh interval (default 86400) |
| `-W`, `--weather-refresh` `<sec>` | Weather refresh interval (default 1800) |

Flags may be combined after a single dash: `-oS` ≡ `-o -S`.

### Widget list

| Widget | Content |
|--------|---------|
| `DATE` | Current date |
| `CPU` | Usage, load, iowait, frequency, temperature, governor |
| `GPU` | Usage, core/memory clocks, VRAM, temperature, fan |
| `MEM` | Host + container memory usage |
| `FAN` | Motherboard fan RPMs and temperatures |
| `BAT` | Percentage, status, time-to-full/empty estimate |
| `UP` | Uptime |
| `STO` | Per-device throughput, per-mount usage, NVMe temperature |
| `NET` | Per-interface throughput, link speed, wireless signal/SSID, local + public IPs |
| `WEATHER` | Current conditions, temperature with min/max |
| `CAL` | Month calendar grid with today highlighted |

### Examples

```
clock                      # auto mode, continuous
clock -o text              # once, plain text
clock -w CPU,MEM,NET       # sidebar with only CPU, MEM, NET
clock -w ''                # full-screen clock, no sidebar
clock -o -S                 # once, Sunday-start calendar
clock -W 600                # continuous, weather refresh every 10m
```

## License

MIT
