# Protocol Status

What's working, what's not, and what's still being figured out.

## Working

| Feature | RX Byte | Notes |
|---------|---------|-------|
| Power | 7 bit 4 | |
| Mode | 7 bits 0-3 | Cool=1, Fan=2, Dry=3, Heat=4, Auto=5 |
| Display | 7 bit 5 | |
| Eco | 7 bit 6 | |
| Turbo | 7 bit 7 | |
| Fan Speed | 8 bits 4-7 | Auto=8, Low=9, Med=A, High=B, Low-Mid=C, Mid-High=D |
| Set Temp | 8 bits 0-3 | value + 16 = °C |
| Health/Ion | 9 bit 2 | |
| Timer Active | 9 bit 6 | |
| Swing V Active | 10 bit 6 | |
| Swing H Active | 10 bit 5 | |
| Current Temp | 17-18 | BE16 / 374 → °F |
| Sleep Mode | 19 | Older captures: 0x88=off, 0x89-8B=modes. TLS 1.3 board: 0xB0=off, 0xB1-B3=modes |
| 8°C Heater | 32 bit 7 | |
| Mute | 33 bit 7 | |
| Swing V Position | 51 | Unverified on TLS 1.3 board |
| Swing H Position | 52 | Unverified on TLS 1.3 board; values also changed during fan/feature tests |
| Outdoor Running | 40 bits 0-3 | 0x0A=running, 0x00=idle |
| Heat Mode Active | 40 bit 6 | |

## Rewrite Direction

The component is being moved away from one large climate implementation and toward a capture-driven protocol layer:

| Layer | File | Purpose |
|-------|------|---------|
| BB packet/mapping layer | `esphome/components/pioneer_minisplit/bb_protocol.*` | Owns checksum, heartbeat/set packet construction, and firmware-backed TX/RX mode, fan, and sleep mappings |
| ESPHome entity layer | `pioneer_minisplit.*` | Owns climate/select/switch state, publishes sensors, and calls the protocol layer |
| Debug YAML | `esphome/example-debug.yaml` | Keeps raw bytes and last packets visible while unverified fields are mapped |

This keeps the verified control path separate from guessed status decoding. New features should first land as raw/status captures, then move into `bb_protocol.*` once the byte mapping is proven on the TLS 1.3 board.

## Still Investigating

| Byte | What I've Seen | Best Guess |
|------|----------------|------------|
| 30 | 0x6E idle, rises to 0x82-0x8C when heating | Indoor coil temp (÷4 = °C) |
| 34 | 0x3C idle, jumps to 0x50 under load | Operating state, not a temp |
| 35 | 0x1F-0x20 | Minor flags |
| 36-37 | Drift over time | Runtime counters or outdoor feedback |
| 51 | Changed when sending horizontal swing commands | Not vertical louver position on TLS 1.3 board |
| 52 | 0x08-0x0F changed while scanning fan and feature TX bytes | Not just horizontal louver position on TLS 1.3 board |

## Notes

### The "room temperature" isn't room temperature

The indoor temp sensor (bytes 17-18) matches what the Tuya app shows, but it's measuring return air at the unit - not actual room temp. If you put a thermometer across the room, it'll read different. That's normal.

### Beep is TX-only

The HVAC doesn't report beep state back. Production exposes a `Beep` switch because the WYT012GLSI20RL manual calls out audible command acknowledgement, but this is TX-only: the switch can send the beep flag and publish the requested state, while the HVAC cannot confirm its current beep setting.

### TCL-style fan modes

Starlight and Daizuki Tuya-local profiles expose the same fan names: `auto`, `mute`, `low`, `mid_low`, `mid`, `mid_high`, `high`, and `strong`. On the tested `WYT012GLSI20RL` / `esp_air_DIM_tcl_8M_QIO_TLS_1.3` board, `Low-Mid` and `Mid-High` are verified over the BB protocol as TX `0x3E`/`0x3F` and RX nibbles `0x0C`/`0x0D`.

### Sleep mode names

The dumped Tuya firmware contains `off`, `normal`, `old`, `child`, and `auto` sleep strings. The WYT012GLSI20RL manual exposes Sleep as a simple on/off feature, so the production config uses a `Sleep` switch. The debug config can still expose the broader sleep select for protocol testing.

### Production feature surface

Production intentionally follows the WYT012GLSI20RL manual: mode, set temperature, fan including Mute/Low-Mid/Mid-High/Turbo, Eco, Sleep, 46°F freeze protection, Display, Beep, up-down/left-right louver motor toggles, and indoor/outdoor temperature. Remote-only features such as Timer, I Feel, Memory, and Child Lock are left to Home Assistant automations or the physical remote. Health/Ion is not exposed because it is not present in the provided manual. Current draw and compressor frequency are not exposed in production because those status bytes are not verified on this unit.

### Tuya features not yet mapped to BB serial

The Starlight and Daizuki Tuya profiles also include DPS-backed features such as soft wind, anti-mildew, filter status, and fault codes. Those DPS IDs do not directly identify the BB UART bytes, so they should not be exposed in ESPHome until their serial command/status bytes are captured.

### Louver position status is not decoded on TLS 1.3

A debug probe on `esp_air_DIM_tcl_8M_QIO_TLS_1.3` showed that bytes 51 and 52 cannot be treated as direct vertical/horizontal louver positions. Horizontal commands changed byte 51, and byte 52 changed during non-louver feature commands. The component now reports nonzero louver position bytes as `Unknown` on this board instead of showing misleading fixed positions. Use the raw byte sensors in `example-debug.yaml` for further mapping.

Vertical louver on/off is verified separately from position decoding. The tested board requires byte 31 plus the vertical movement bits in TX byte 10. Clearing the `0x38` bits from byte 10 while sending byte 31 as `0x00` changed RX byte 10 from `0xC0` to `0x80` and the production `Up-Down Louver` switch then reported off.

### TX and RX mode values are different

Heat is 0x01 in TX but 0x04 in RX. See PROTOCOL.md for the full mapping.
