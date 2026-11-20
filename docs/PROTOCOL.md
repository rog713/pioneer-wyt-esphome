# BB Protocol Reference

Protocol documentation for mini splits using the BB serial protocol over UART (9600 8E1).

## Serial Config

```
Baud:   9600
Data:   8 bits
Parity: Even (8E1) ← not 8N1
Stop:   1 bit
```

## Packet Structure

```
[HEADER] [DIR] [DIR2] [CMD] [LEN] [PAYLOAD...] [CHECKSUM]
   BB     XX    XX     XX    XX    ...           XX
```

| Field | Size | Description |
|-------|------|-------------|
| HEADER | 1 | Always 0xBB |
| DIR | 1 | 0x00 = to HVAC, 0x01 = from HVAC |
| DIR2 | 1 | 0x01 (to HVAC), 0x00 (from HVAC) |
| CMD | 1 | Command type |
| LEN | 1 | Payload length |
| PAYLOAD | N | Variable |
| CHECKSUM | 1 | XOR of all bytes |

---

## TX Commands (ESP → HVAC)

### Heartbeat (0x04)

8 bytes. Polls the status every 2 seconds.

```
BB 00 01 04 02 01 00 [checksum]
```

**One frame at a time.** The unit silently drops a command that arrives while it is still answering the previous
frame. After any frame the component therefore waits for its reply (SET 600 ms, status and energy 400 ms) and leaves
100 ms before the next. A SET that gets no reply is sent again, up to three times, but only once the unit has been
seen replying. An early 250 ms post-command poll used to collide with replies, and that looked exactly like "odd °F
setpoints refused".

### Set Command (0x03)

35 bytes: `BB 00 01 03 1D`, a 29-byte payload, then the checksum. The unit answers with a status frame of type 0x03.

Like the stock module firmware (Tuya `esp_air_DIM_tcl` 1.3.3), the component builds every SET from the unit's latest
status, then applies one change. Settings it does not touch are sent back exactly as the unit reported them: timers,
louver positions, Fahrenheit mode and the Eco setpoint. So a command never resets them.

**Key bytes** (packet byte = payload byte + 5):

| Byte | Bits | What |
|------|------|------|
| 7 | 7 | Eco |
| 7 | 6 | Display |
| 7 | 5 | Beep |
| 7 | 2 | Power |
| 8 | 7 | Mute |
| 8 | 6 | Turbo |
| 8 | 4 | Health/Ion |
| 8 | 0-3 | Mode |
| 9 | 0-3 | Setpoint: `31 - °C` |
| 9 | 4-7 | Eco setpoint: `31 - °C` |
| 10 | 7 | 8°C heater |
| 10 | 3-5 | Up-down louver motor (all three on or off) |
| 10 | 0-2 | Fan speed |
| 11 | 3 | Left-right louver motor |
| 11 | 2 | Extra °F degree (Fahrenheit mode) |
| 11 | 1 | Half degree (°C) |
| 12 | 7 | Fahrenheit mode |
| 12 | 2-4 | Eco sub-mode |
| 19 | 0-2 | Sleep (0 off, 1 standard, 2 elderly, 3 child, 4 auto) |
| 32 | 3-5 / 0-2 | Up-down louver: swing range / fixed position |
| 32 | 6-7 | The remote's comfort wind (cleared by any fan change except Auto) |
| 33 | 0-2 / 3-5 | Left-right louver: fixed position / swing range |

**TX Mode values:**

| Value | Mode |
|-------|------|
| 0x01 | Heat |
| 0x02 | Dry |
| 0x03 | Cool |
| 0x07 | Fan Only |
| 0x08 | Auto |

**TX Fan values (byte 10 bits 0-2):**

| Value | Speed |
|-------|-------|
| 0 | Auto |
| 2 | Low |
| 3 | Medium |
| 5 | High |
| 6 | Mid-Low |
| 7 | Mid-High |

Turbo is High plus byte 8 bit 6, and Mute is Low plus byte 8 bit 7. The stock firmware's side effects are kept:
- Changing mode clears Eco, its sub-mode, Sleep and the 8°C heater, and the mode starts with the fan it last ran
  with.
- Fan changes are ignored in Dry.
- Eco and Sleep work only in Heat and Cool.
- Turning Eco off clears its sub-mode.
- The 8°C heater clears Sleep.

### Fahrenheit Mode

With byte 12 bit 7 set, the unit runs in its own Fahrenheit mode and takes every whole °F from 61 to 89. Each °F is a
°C setpoint (byte 9) plus an extra-degree bit (byte 11 bit 2):

| °F | °C | extra | °F | °C | extra | °F | °C | extra |
|----|----|-------|----|----|-------|----|----|-------|
| 61 | 16 | 0 | 71 | 21 | 1 | 81 | 27 | 0 |
| 62 | 16 | 1 | 72 | 22 | 0 | 82 | 28 | 0 |
| 63 | 17 | 0 | 73 | 23 | 0 | 83 | 28 | 1 |
| 64 | 18 | 0 | 74 | 23 | 1 | 84 | 29 | 0 |
| 65 | 18 | 1 | 75 | 24 | 0 | 85 | 29 | 1 |
| 66 | 19 | 0 | 76 | 24 | 1 | 86 | 30 | 0 |
| 67 | 19 | 1 | 77 | 25 | 0 | 87 | 30 | 1 |
| 68 | 20 | 0 | 78 | 25 | 1 | 88 | 31 | 0 |
| 69 | 20 | 1 | 79 | 26 | 0 | 89 | 31 | 1 |
| 70 | 21 | 0 | 80 | 26 | 1 | | | |

The status reports Fahrenheit mode in byte 10 bit 7 and the extra degree in byte 9 bit 0. The first SET that
switches the unit into Fahrenheit mode drops the extra degree, so the component switches at boot and sends an odd °F
again once the unit reports Fahrenheit mode.

### Energy Query (0x0A)

9 bytes, sent every third poll as the stock firmware does:

```
BB 00 01 0A 03 05 00 00 [checksum]
```

The unit answers with a cmd 0x0A frame (45-byte payload). Packet bytes 11 10 9 . 8 21 20 hold the unit's running
energy total in kWh, in BCD with six decimals (for example `00 00 26 . 70 13 00` = 26.701300 kWh). Bytes 22-23
(little-endian) count its on-time in 2-minute ticks. The unit updates both every 2 minutes, and both start again from
0 when the indoor board restarts. Checked against a whole-circuit meter: 0.0906 kWh over two heating runs on both.
Power in W is the energy used between two ticks divided by their 120 s; it lands within about 5% of the meter.

---

## RX Status (HVAC → ESP)

### Status Response (0x04)

Usually around 61-68 bytes, depending on adapter firmware. Sent in response to heartbeat.

**Key bytes:**

| Byte | Bits | What |
|------|------|------|
| 7 | 7 | Turbo |
| 7 | 6 | Eco |
| 7 | 5 | Display |
| 7 | 4 | Power |
| 7 | 0-3 | Mode |
| 8 | 4-7 | Fan speed |
| 8 | 0-3 | Temp offset (+16 = °C) |
| 9 | 6 | Timer active |
| 9 | 2 | Health/Ion |
| 10 | 6 | Swing V active |
| 10 | 5 | Swing H active |
| 17-18 | - | Current temp (BE16 / 374 = °F) |
| 19 | - | Sleep mode |
| 32 | 7 | 8°C Heater |
| 33 | 7 | Mute |
| 35 | - | Outdoor temp (byte - 20 = °C) |
| 36 | - | Condenser coil temp (°C) |
| 37 | - | Compressor discharge temp (°C) |
| 38 | - | Compressor frequency (Hz) |
| 39 | - | Outdoor fan speed |
| 40 | 6 | Heat mode active |
| 40 | 0-3 | Outdoor unit status (0x0A = running) |
| 46 | - | Current draw (A) |
| 51 | - | Swing V position |
| 52 | - | Swing H position |

**RX Mode values (different from TX!):**

| Value | Mode |
|-------|------|
| 0x01 | Cool |
| 0x02 | Fan Only |
| 0x03 | Dry |
| 0x04 | Heat |
| 0x05 | Auto |

**RX Fan values:**

| Nibble | Speed |
|--------|-------|
| 0x8 | Auto |
| 0x9 | Low |
| 0xA | Medium |
| 0xB | High |
| 0xC | Mid-Low |
| 0xD | Mid-High |

---

## Feature Mapping

Quick reference for TX vs RX encoding:

| Feature | TX Byte | TX Value | RX Byte | RX Value |
|---------|---------|----------|---------|----------|
| Power | 7 | bit 2 | 7 | bit 4 |
| Mode | 8 | bits 0-3 | 7 | bits 0-3 |
| Set Temp | 9 | 111 - rounded °C | 8 | low nibble + 16 |
| Fan | 10 | fan byte, with vertical louver bits masked separately | 8 | bits 4-7 |
| Display | 7 | bit 6 | 7 | bit 5 |
| Eco | 7 | bit 7 | 7 | bit 6 |
| Turbo | 8 | bit 6 | 7 | bit 7 |
| Mute | 8 | bit 7 | 33 | bit 7 |
| Health | 8 | bit 4 | 9 | bit 2 |
| 8°C Heater | 10 | bit 7 | 32 | bit 7 |
| Sleep | 19 | 0-3 | 19 | 0x88-0x8B or 0xB0-0xB3, depending on adapter firmware |

---

## Sleep Modes

The original Tuya firmware exposes sleep mode names `off`, `normal`, `old`, `child`, and an additional `auto` string. The ESPHome component maps the verified controllable modes:

| TX Value | TLS 1.3 RX | Name |
|----------|------------|------|
| 0 | 0xB0 | Off |
| 1 | 0xB1 | Standard |
| 2 | 0xB2 | Elderly |
| 3 | 0xB3 | Child |

The older protocol captures used `0x88-0x8B` for the same status range.

---

## Swing Positions

### Vertical (TX byte 31, RX byte 51)

RX values are provisional. On the tested TLS 1.3 board, byte 51 changed in response to horizontal commands, so the component does not currently decode nonzero vertical positions from status packets.

On the tested `WYT012GLSI20RL` / `esp_air_DIM_tcl_8M_QIO_TLS_1.3` board, byte 31 alone does not fully control vertical louver movement. The TX fan byte also carries vertical movement bits (`0x38`). To turn up-down louver off, the component sends byte 31 as `0x00` and clears `0x38` from TX byte 10 while preserving the fan-speed bits.

| TX | RX | Position |
|----|-----|----------|
| 0x00 | 0x00 | Off |
| 0x08 | 0x08 | Auto Swing |
| 0x88 | 0x08 | Swing Upper, provisional |
| 0x48 | 0x08 | Swing Lower, provisional |
| 0x20 | 0x00 | Fixed 1 (Top), provisional |
| 0x24 | 0x00 | Fixed 2, provisional |
| 0x28 | 0x00 | Fixed 3 (Middle), provisional |
| 0x2C | 0x00 | Fixed 4, provisional |
| 0x30 | 0x00 | Fixed 5 (Bottom), provisional |

### Horizontal (TX byte 32, RX byte 52)

Horizontal swing is partially mapped. On the tested `WYT012GLSI20RL` / `esp_air_DIM_tcl_8M_QIO_TLS_1.3` board, RX byte 52 also changed during fan-mode scans, so treat these values as provisional until captured from direct louver commands.

| TX | RX | Position |
|----|-----|----------|
| 0x80 | 0x00 | Off |
| 0x88 | 0x08 | Auto Swing, provisional |
| 0x90 | 0x10 | Swing Left, provisional |
| 0x98 | 0x18 | Swing Center, provisional |
| 0xA0 | 0x20 | Swing Right, provisional |
| 0x81 | 0x01 | Fixed 1 (Far Left) |
| 0x82 | 0x02 | Fixed 2 |
| 0x83 | 0x03 | Fixed 3 (Center) |
| 0x84 | 0x04 | Fixed 4 |
| 0x85 | 0x05 | Fixed 5 (Far Right) |
| 0x86 | 0x06 | Fixed 6 (Rightmost, provisional) |

---

## Temperature Formulas

```c
// Setpoint (status byte 8 bits 0-3), half degree in byte 9 bit 1
set_temp_c = (byte8 & 0x0F) + 16 + (byte9 & 0x02 ? 0.5 : 0);
// In Fahrenheit mode: look the °C and the extra degree (byte 9 bit 0) up in the Fahrenheit table above.

// Room temperature, as the stock firmware reads it (0.1 °C steps)
room_temp_c = (byte17 * 4 + ((byte18 >> 2) & 3) - 200) / 10.0f;

// Outdoor temp
outdoor_temp_c = buf[35] - 20.0f;
```

Older versions read the room temperature as `((byte17 << 8) | byte18) / 374` °F, which matched the Tuya app to
within a fraction of a degree.

## Fault Bits

The stock firmware reports a 25-bit fault bitmap (its Tuya DP 20). It is built from status bytes 16, 41, 42, 44
and 56 (see `fault_bits` in `bb_protocol.cpp`). The component publishes "None" or the set bit numbers; the fault names live
in Tuya's cloud.

---

## Checksum

XOR all bytes except the checksum itself:

```c
uint8_t checksum(uint8_t *data, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len - 1; i++) {
        result ^= data[i];
    }
    return result;
}
```

---

## Unknown Bytes

Still figuring these out:

| Byte | Observed | Guess |
|------|----------|-------|
| 30 | 0x6E idle, 0x82+ heating | Indoor coil temp (÷4 = °C) |
| 34 | 0x3C → 0x50 under load | Operating state |
| 36-37 | Drift over time | Runtime counters |
