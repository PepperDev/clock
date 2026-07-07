# Progress

## Done ✓
1. **Network** — refactored to `/proc/self/net/dev`, musl sscanf bug fixed, throughput delta test
2. **GPU monitoring** — AMD (busy percent) + Intel (gt_cur_freq), DRM hwmon temp, mem_info_vis VRAM
3. **Motherboard fans** — `/sys/class/hwmon/hwmon*/fan*_input` enumeration
4. **Motherboard temperatures** — `/sys/class/hwmon/hwmon*/temp*_input` enumeration

## Remaining
- `--all` shortcut (enables all optional features)
- Weather (icon + min/max temp)
- Sixel mode (sixel graphics clock + info bar)
