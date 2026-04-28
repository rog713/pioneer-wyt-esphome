#include "bb_protocol.h"

namespace esphome {
namespace pioneer_minisplit {
namespace bb_protocol {

uint8_t checksum(const uint8_t *data, size_t len) {
  uint8_t xor_sum = 0;
  for (size_t i = 0; i < len; i++) {
    xor_sum ^= data[i];
  }
  return xor_sum;
}

std::array<uint8_t, HEARTBEAT_PACKET_LEN> make_heartbeat() {
  std::array<uint8_t, HEARTBEAT_PACKET_LEN> packet = {
      HEADER, DIR_TO_HVAC, DIR_MODULE, CMD_STATUS, 0x02, 0x01, 0x00, 0x00};
  packet[7] = checksum(packet.data(), 7);
  return packet;
}

std::array<uint8_t, SET_PACKET_LEN> make_set_command(const TxState &state) {
  std::array<uint8_t, SET_PACKET_LEN> packet{};

  packet[0] = HEADER;
  packet[1] = DIR_TO_HVAC;
  packet[2] = DIR_MODULE;
  packet[3] = CMD_SET;
  packet[4] = SET_PAYLOAD_LEN;

  uint8_t flags1 = 0;
  if (state.eco) flags1 |= 0x80;
  if (state.display) flags1 |= 0x40;
  if (state.beep) flags1 |= 0x20;
  if (state.power) flags1 |= 0x04;
  packet[7] = flags1;

  uint8_t mode_flags = state.mode & 0x0F;
  if (state.health) mode_flags |= 0x10;
  if (state.turbo) mode_flags |= 0x40;
  if (state.mute) mode_flags |= 0x80;
  packet[8] = mode_flags;

  packet[9] = 111 - state.set_temp_c;

  uint8_t fan_byte = state.fan;
  if (state.swing_v_active) {
    fan_byte |= 0x38;
  } else {
    fan_byte &= 0xC7;
  }
  if (state.heater_8c) fan_byte |= 0x80;
  packet[10] = fan_byte;

  if (state.swing_h_dirty && state.swing_h != 0x80) {
    packet[11] = 0x08;
  }

  packet[19] = state.sleep;

  if (state.swing_v_dirty) packet[31] = state.swing_v;
  if (state.swing_h_dirty) packet[32] = state.swing_h;

  packet[33] = checksum(packet.data(), packet.size() - 1);
  return packet;
}

uint8_t tx_mode_from_rx(uint8_t rx_mode) {
  switch (rx_mode) {
    case RX_MODE_COOL:
      return TX_MODE_COOL;
    case RX_MODE_FAN:
      return TX_MODE_FAN;
    case RX_MODE_DRY:
      return TX_MODE_DRY;
    case RX_MODE_HEAT:
      return TX_MODE_HEAT;
    case RX_MODE_AUTO:
      return TX_MODE_AUTO;
    default:
      return INVALID;
  }
}

uint8_t tx_fan_from_rx(uint8_t rx_fan) {
  switch (rx_fan) {
    case RX_FAN_AUTO:
      return TX_FAN_AUTO;
    case RX_FAN_LOW:
      return TX_FAN_LOW;
    case RX_FAN_MEDIUM:
      return TX_FAN_MEDIUM;
    case RX_FAN_HIGH:
      return TX_FAN_HIGH;
    case RX_FAN_MID_LOW:
      return TX_FAN_MID_LOW;
    case RX_FAN_MID_HIGH:
      return TX_FAN_MID_HIGH;
    default:
      return INVALID;
  }
}

uint8_t sleep_from_rx(uint8_t rx_sleep) {
  if (rx_sleep == 0x89 || rx_sleep == 0xB1) return 1;
  if (rx_sleep == 0x8A || rx_sleep == 0xB2) return 2;
  if (rx_sleep == 0x8B || rx_sleep == 0xB3) return 3;
  return 0;
}

const char *rx_mode_name(uint8_t rx_mode) {
  switch (rx_mode) {
    case RX_MODE_COOL:
      return "Cool";
    case RX_MODE_FAN:
      return "Fan";
    case RX_MODE_DRY:
      return "Dry";
    case RX_MODE_HEAT:
      return "Heat";
    case RX_MODE_AUTO:
      return "Auto";
    default:
      return "Unknown";
  }
}

const char *rx_fan_name(uint8_t rx_fan, bool turbo, bool mute) {
  if (rx_fan == RX_FAN_HIGH && turbo) return "Turbo";
  if (rx_fan == RX_FAN_LOW && mute) return "Mute";
  switch (rx_fan) {
    case RX_FAN_AUTO:
      return "Auto";
    case RX_FAN_LOW:
      return "Low";
    case RX_FAN_MEDIUM:
      return "Medium";
    case RX_FAN_HIGH:
      return "High";
    case RX_FAN_MID_LOW:
      return "Low-Mid";
    case RX_FAN_MID_HIGH:
      return "Mid-High";
    default:
      return "Unknown";
  }
}

const char *sleep_name(uint8_t sleep) {
  switch (sleep) {
    case 0:
      return "Off";
    case 1:
      return "Standard";
    case 2:
      return "Elderly";
    case 3:
      return "Child";
    default:
      return "Unknown";
  }
}

}  // namespace bb_protocol
}  // namespace pioneer_minisplit
}  // namespace esphome
