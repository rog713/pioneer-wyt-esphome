#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace pioneer_minisplit {
namespace bb_protocol {

static constexpr uint8_t HEADER = 0xBB;
static constexpr uint8_t DIR_TO_HVAC = 0x00;
static constexpr uint8_t DIR_FROM_HVAC = 0x01;
static constexpr uint8_t DIR_MODULE = 0x01;
static constexpr uint8_t DIR_HVAC = 0x00;
static constexpr uint8_t CMD_SET = 0x03;
static constexpr uint8_t CMD_STATUS = 0x04;
static constexpr uint8_t SET_PAYLOAD_LEN = 0x1C;
static constexpr size_t SET_PACKET_LEN = 34;
static constexpr size_t HEARTBEAT_PACKET_LEN = 8;
static constexpr uint8_t INVALID = 0xFF;

enum TxMode : uint8_t {
  TX_MODE_HEAT = 0x01,
  TX_MODE_DRY = 0x02,
  TX_MODE_COOL = 0x03,
  TX_MODE_FAN = 0x07,
  TX_MODE_AUTO = 0x08,
};

enum RxMode : uint8_t {
  RX_MODE_COOL = 0x01,
  RX_MODE_FAN = 0x02,
  RX_MODE_DRY = 0x03,
  RX_MODE_HEAT = 0x04,
  RX_MODE_AUTO = 0x05,
};

enum TxFan : uint8_t {
  TX_FAN_AUTO = 0x38,
  TX_FAN_LOW = 0x3A,
  TX_FAN_MEDIUM = 0x3B,
  TX_FAN_HIGH = 0x3D,
  TX_FAN_MID_LOW = 0x3E,
  TX_FAN_MID_HIGH = 0x3F,
};

enum RxFan : uint8_t {
  RX_FAN_AUTO = 0x08,
  RX_FAN_LOW = 0x09,
  RX_FAN_MEDIUM = 0x0A,
  RX_FAN_HIGH = 0x0B,
  RX_FAN_MID_LOW = 0x0C,
  RX_FAN_MID_HIGH = 0x0D,
};

struct TxState {
  bool power{false};
  uint8_t mode{TX_MODE_COOL};
  uint8_t fan{TX_FAN_AUTO};
  uint8_t set_temp_c{24};
  bool eco{false};
  bool display{true};
  bool beep{false};
  bool turbo{false};
  bool health{false};
  bool mute{false};
  bool heater_8c{false};
  uint8_t sleep{0};
  bool swing_v_active{false};
  bool swing_v_dirty{false};
  uint8_t swing_v{0x08};
  bool swing_h_dirty{false};
  uint8_t swing_h{0x80};
};

uint8_t checksum(const uint8_t *data, size_t len);
std::array<uint8_t, HEARTBEAT_PACKET_LEN> make_heartbeat();
std::array<uint8_t, SET_PACKET_LEN> make_set_command(const TxState &state);

uint8_t tx_mode_from_rx(uint8_t rx_mode);
uint8_t tx_fan_from_rx(uint8_t rx_fan);
uint8_t sleep_from_rx(uint8_t rx_sleep);

const char *rx_mode_name(uint8_t rx_mode);
const char *rx_fan_name(uint8_t rx_fan, bool turbo, bool mute);
const char *sleep_name(uint8_t sleep);

}  // namespace bb_protocol
}  // namespace pioneer_minisplit
}  // namespace esphome
