#include "bb_protocol.h"

#include <cmath>

namespace esphome {
namespace pioneer_minisplit {
namespace bb_protocol {

namespace {

inline uint8_t bit(uint8_t v, int b) { return (v >> b) & 1; }
inline void put(uint8_t &dst, int b, bool v) {
  dst = static_cast<uint8_t>((dst & ~(1 << b)) | ((v ? 1 : 0) << b));
}
inline void put_field(uint8_t &dst, int shift, uint8_t mask, uint8_t v) {
  dst = static_cast<uint8_t>((dst & ~(mask << shift)) | ((v & mask) << shift));
}

// The unit's Fahrenheit table (stock firmware, 0x40201380): every °F from 61 to 89 is a whole °C plus an "extra"
// bit that the unit reads in SET s[6] bit 2 and reports in status p[4] bit 0.
struct FEntry {
  uint8_t f;
  uint8_t c;
  uint8_t extra;
};
const FEntry F_TABLE[] = {
    {61, 16, 0}, {62, 16, 1}, {63, 17, 0}, {64, 18, 0}, {65, 18, 1}, {66, 19, 0}, {67, 19, 1}, {68, 20, 0},
    {69, 20, 1}, {70, 21, 0}, {71, 21, 1}, {72, 22, 0}, {73, 23, 0}, {74, 23, 1}, {75, 24, 0}, {76, 24, 1},
    {77, 25, 0}, {78, 25, 1}, {79, 26, 0}, {80, 26, 1}, {81, 27, 0}, {82, 28, 0}, {83, 28, 1}, {84, 29, 0},
    {85, 29, 1}, {86, 30, 0}, {87, 30, 1}, {88, 31, 0}, {89, 31, 1},
};

bool on_in_heat_or_cool(const uint8_t *s) {
  const uint8_t m = s[3] & 0x0F;
  return is_on(s) && (m == TX_MODE_HEAT || m == TX_MODE_COOL);
}

}  // namespace

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

std::array<uint8_t, ENERGY_QUERY_PACKET_LEN> make_energy_query() {
  std::array<uint8_t, ENERGY_QUERY_PACKET_LEN> packet = {
      HEADER, DIR_TO_HVAC, DIR_MODULE, CMD_ENERGY, 0x03, 0x05, 0x00, 0x00, 0x00};
  packet[8] = checksum(packet.data(), 8);
  return packet;
}

bool decode_energy(const uint8_t *e, size_t len, EnergyReading &out) {
  if (e == nullptr || len < 19) return false;
  double kwh = 0;
  // Most significant first: e[6] e[5] e[4] are whole kWh, e[3] e[16] e[15] the six decimals.
  static constexpr uint8_t DIGITS[6] = {6, 5, 4, 3, 16, 15};
  for (uint8_t idx : DIGITS) {
    const uint8_t v = e[idx];
    if ((v >> 4) > 9 || (v & 0x0F) > 9) return false;
    kwh = kwh * 100 + (v >> 4) * 10 + (v & 0x0F);
  }
  out.kwh = kwh / 1e6;
  out.ticks = uint16_t(e[17] | (uint16_t(e[18]) << 8));
  return true;
}

bool energy_power_w(const EnergyReading &prev, const EnergyReading &now, float &watts) {
  if (now.ticks <= prev.ticks || now.kwh < prev.kwh) return false;
  const uint16_t ticks = now.ticks - prev.ticks;
  if (ticks > 3600 / ENERGY_TICK_SECONDS) return false;
  watts = float((now.kwh - prev.kwh) * 3.6e6 / (double(ticks) * ENERGY_TICK_SECONDS));
  return true;
}

std::array<uint8_t, SET_PACKET_LEN> make_set_frame(const uint8_t *s) {
  std::array<uint8_t, SET_PACKET_LEN> packet{};
  packet[0] = HEADER;
  packet[1] = DIR_TO_HVAC;
  packet[2] = DIR_MODULE;
  packet[3] = CMD_SET;
  packet[4] = static_cast<uint8_t>(SET_PAYLOAD_LEN);
  for (size_t i = 0; i < SET_PAYLOAD_LEN; i++) packet[5 + i] = s[i];
  packet[SET_PACKET_LEN - 1] = checksum(packet.data(), SET_PACKET_LEN - 1);
  return packet;
}

void init_set(uint8_t *s) {
  for (size_t i = 0; i < SET_PAYLOAD_LEN; i++) s[i] = 0;
  s[2] = 0x60;                                                  // display, beep
  s[3] = TX_MODE_COOL;
  s[4] = static_cast<uint8_t>(((31 - 26) << 4) | (31 - 26));  // 26 °C, Eco 26 °C
  s[14] = 0x08;
}

void copy_status_to_set(const uint8_t *p, uint8_t *s, bool beep) {
  put(s[2], 2, bit(p[2], 4));  // power
  const uint8_t mode = tx_mode_from_rx(p[2] & 0x0F);
  if (mode != INVALID) put_field(s[3], 0, 0x0F, mode);
  const uint8_t fan = tx_fan_from_rx((p[3] >> 4) & 0x07);
  if (fan != INVALID) put_field(s[5], 0, 0x07, fan);
  put_field(s[5], 3, 0x07, bit(p[5], 6) ? 0x07 : 0x00);  // vertical swing
  put(s[6], 3, bit(p[5], 5));                            // horizontal swing
  put_field(s[14], 0, 0x07, p[14] & 0x07);               // sleep
  put(s[2], 7, bit(p[2], 6));                            // eco
  put(s[7], 7, bit(p[5], 7));                            // Fahrenheit mode
  put_field(s[4], 0, 0x0F, static_cast<uint8_t>(~p[3]) & 0x0F);  // setpoint, 31 - °C
  put(s[6], 2, bit(p[4], 0));                            // extra Fahrenheit degree
  put(s[6], 1, bit(p[4], 1));                            // half degree

  put(s[2], 3, bit(p[4], 6));
  put(s[2], 4, bit(p[4], 7));
  put(s[2], 6, bit(p[2], 5));  // display
  put(s[3], 4, bit(p[4], 2));  // health
  put(s[3], 5, bit(p[4], 3));
  put(s[5], 7, bit(p[27], 7));  // 8 °C heater
  if (p[45] != 0xFF) put(s[5], 6, bit(p[45], 5));
  put(s[6], 7, bit(p[4], 5));
  put(s[6], 0, bit(p[4], 4));
  put(s[6], 5, bit(p[28], 3));
  put_field(s[7], 2, 0x07, (p[28] >> 4) & 0x07);  // eco sub-mode
  put_field(s[7], 0, 0x03, p[5] & 0x03);
  s[9] = p[6];
  s[10] = p[7];
  s[11] = p[8];
  s[12] = p[9];
  if (p[42] != 0xFF) {
    put_field(s[13], 0, 0x03, p[42] & 0x03);
    const uint8_t x = (p[42] >> 2) & 0x07;
    put_field(s[13], 2, 0x07, x);
    if (x < 4) put_field(s[13], 5, 0x07, x);
  }
  if (p[45] != 0xFF) {
    put(s[14], 5, bit(p[45], 4));
    put(s[14], 4, bit(p[45], 3));
    put(s[14], 7, bit(p[45], 0));
    put(s[14], 3, bit(p[45], 2));
  }
  for (int i = 15; i <= 24; i++) s[i] = p[i];
  if (p[46] != 0xFF) {  // vertical louver: range in bits 3-5, fixed position in bits 0-2
    put_field(s[27], 3, 0x07, (p[46] >> 3) & 0x07);
    put_field(s[27], 0, 0x07, p[46] & 0x07);
  }
  put(s[27], 6, bit(p[27], 3));
  put(s[27], 7, bit(p[27], 4));
  if (p[47] != 0xFF) {  // horizontal louver
    put_field(s[28], 0, 0x07, p[47] & 0x07);
    put_field(s[28], 3, 0x07, (p[47] >> 3) & 0x07);
  }
  if (!bit(p[5], 3)) put(s[28], 6, bit(p[27], 5));
  put(s[28], 7, bit(p[27], 6));
  if (p[50] != 0xFF) put_field(s[4], 4, 0x0F, static_cast<uint8_t>(~p[50]) & 0x0F);
  if (p[49] != 0xFF) s[26] = p[49];
  if (p[48] != 0xFF) s[25] = p[48];
  if (beep) s[2] |= 0x20;
  if (bit(p[2], 4)) {  // turbo and mute carry over only while the unit is on
    put(s[3], 7, bit(p[28], 7));
    put(s[3], 6, bit(p[2], 7));
  }
}

bool is_on(const uint8_t *s) { return bit(s[2], 2); }
uint8_t tx_mode(const uint8_t *s) { return s[3] & 0x0F; }

void set_power(uint8_t *s, bool on) {
  put(s[2], 2, on);
  put(s[5], 7, false);  // 8 °C heater
  if (on) {
    put(s[14], 4, false);
    put(s[3], 5, false);
    return;
  }
  put(s[2], 7, false);   // eco
  put(s[3], 4, false);   // health
  put(s[5], 6, false);
  put_field(s[14], 0, 0x07, 0);  // sleep
  put_field(s[13], 0, 0x03, 0);
  s[27] &= 0x3F;
  if (bit(s[2], 3)) {
    put(s[2], 3, false);
    put(s[6], 4, true);
  }
}

uint8_t default_fan(uint8_t mode) { return mode == TX_MODE_DRY ? TX_FAN_LOW : TX_FAN_AUTO; }

// The fan change itself, without the Dry check (0x4026ca3c): Turbo, Mute and any speed but Auto also end the
// remote's comfort wind (s[27] bits 6-7), and Turbo clears s[5] bit 6.
static void apply_fan(uint8_t *s, uint8_t fan, bool turbo, bool mute) {
  if (turbo) fan = TX_FAN_HIGH;
  if (mute && !turbo) fan = TX_FAN_LOW;
  put_field(s[5], 0, 0x07, fan);
  put(s[3], 6, turbo);
  put(s[3], 7, mute && !turbo);
  if (turbo || mute || fan != TX_FAN_AUTO) s[27] &= 0x3F;
  if (turbo) put(s[5], 6, false);
}

bool set_mode(uint8_t *s, uint8_t mode, uint8_t fan, bool turbo, bool mute) {
  if ((s[3] & 0x0F) == (mode & 0x0F)) return false;
  put_field(s[3], 0, 0x0F, mode);
  s[3] &= static_cast<uint8_t>(~0x30);  // health and bit 5
  put(s[2], 7, false);                  // eco
  put_field(s[7], 2, 0x07, 0);          // eco sub-mode
  put_field(s[14], 0, 0x07, 0);         // sleep
  s[5] &= 0x3F;                         // 8 °C heater and bit 6
  apply_fan(s, fan, turbo, mute);
  return true;
}

void set_setpoint_c(uint8_t *s, int c, bool half) {
  if (c < C_MIN) c = C_MIN;
  if (c > C_MAX) c = C_MAX;
  put_field(s[4], 0, 0x0F, static_cast<uint8_t>(31 - c));
  if (s[7] & 0x80) {
    put(s[6], 2, false);  // Fahrenheit mode from the remote: the whole-°C step
  } else {
    put(s[6], 1, half);
  }
}

bool set_setpoint_f(uint8_t *s, int f) {
  for (const auto &e : F_TABLE) {
    if (e.f != f) continue;
    put_field(s[4], 0, 0x0F, static_cast<uint8_t>(31 - e.c));
    put(s[7], 7, true);  // Fahrenheit mode
    put(s[6], 2, e.extra);
    put(s[6], 1, false);
    return true;
  }
  return false;
}

bool set_fan(uint8_t *s, uint8_t fan, bool turbo, bool mute) {
  if ((s[3] & 0x0F) == TX_MODE_DRY) return false;
  apply_fan(s, fan, turbo, mute);
  return true;
}

bool set_sleep(uint8_t *s, uint8_t sleep) {
  if (!on_in_heat_or_cool(s)) return false;
  put_field(s[14], 0, 0x07, sleep);
  if (sleep != 0) put(s[5], 7, false);
  return true;
}

bool set_eco(uint8_t *s, bool on) {
  if (!on_in_heat_or_cool(s)) return false;
  put(s[2], 7, on);
  if (!on) put_field(s[7], 2, 0x07, 0);  // eco sub-mode
  return true;
}

void set_display(uint8_t *s, bool on) { put(s[2], 6, on); }

bool set_health(uint8_t *s, bool on) {
  if (!is_on(s)) return false;
  put(s[3], 4, on);
  return true;
}

void set_heater_8c(uint8_t *s, bool on) {
  put(s[5], 7, on);
  if (on) put_field(s[14], 0, 0x07, 0);  // sleep
}

void set_swing(uint8_t *s, bool vertical, bool horizontal) {
  put(s[6], 3, horizontal);
  put_field(s[5], 3, 0x07, vertical ? 0x07 : 0x00);
}

void set_vertical_range(uint8_t *s, uint8_t range) {
  put_field(s[27], 3, 0x07, range);
  if (range != 0) {
    s[5] |= 0x38;
    put_field(s[27], 0, 0x07, 0);
  } else {
    s[5] &= static_cast<uint8_t>(~0x38);
  }
}

void set_vertical_fixed(uint8_t *s, uint8_t position) {
  put_field(s[27], 0, 0x07, position);
  put_field(s[27], 3, 0x07, 0);
  s[5] &= static_cast<uint8_t>(~0x38);
}

void set_horizontal_range(uint8_t *s, uint8_t range) {
  put_field(s[28], 3, 0x07, range);
  if (range != 0) {
    put(s[6], 3, true);
    put_field(s[28], 0, 0x07, 0);
  }
}

void set_horizontal_fixed(uint8_t *s, uint8_t position) {
  put_field(s[28], 0, 0x07, position);
  put(s[28], 6, false);
  put_field(s[28], 3, 0x07, 0);
  put(s[6], 3, false);
}

float room_temperature_c(const uint8_t *p) {
  if (p[12] == 0) return NAN;
  const int raw = p[12] * 4 + ((p[13] >> 2) & 0x03);  // °C x 10 + 200
  return (raw - 200) / 10.0f;
}

bool fahrenheit_mode(const uint8_t *p) { return bit(p[5], 7); }

int setpoint_f(const uint8_t *p) {
  const uint8_t c = (p[3] & 0x0F) + 16;
  const uint8_t extra = bit(p[4], 0);
  for (const auto &e : F_TABLE) {
    if (e.c == c && e.extra == extra) return e.f;
  }
  return -1;
}

float setpoint_c(const uint8_t *p) { return (p[3] & 0x0F) + 16 + (bit(p[4], 1) ? 0.5f : 0.0f); }

uint32_t fault_bits(const uint8_t *p, size_t plen) {
  if (plen <= 51) return 0;
  uint32_t f = 0;
  auto set = [&f](int b, bool v) {
    if (v) f |= (1UL << b);
  };
  set(0, bit(p[11], 3) || bit(p[36], 0));
  set(1, bit(p[11], 6));
  set(2, bit(p[11], 7));
  for (int i = 0; i < 3; i++) set(3 + i, bit(p[36], 2 + i));
  set(6, bit(p[11], 5));
  set(7, bit(p[36], 5));
  set(8, bit(p[36], 6));
  set(9, bit(p[39], 4) || bit(p[36], 7));
  set(10, bit(p[37], 1));
  set(11, bit(p[11], 1));
  set(12, bit(p[36], 1));
  set(13, bit(p[37], 2));
  set(14, bit(p[37], 0));
  for (int i = 0; i < 3; i++) set(15 + i, bit(p[37], 3 + i));
  for (int i = 0; i < 7; i++) set(18 + i, bit(p[51], i));
  return f;
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
  if (turbo) return "Turbo";
  if (mute) return "Mute";
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
    case 4:
      return "Auto";
    default:
      return "Unknown";
  }
}

}  // namespace bb_protocol
}  // namespace pioneer_minisplit
}  // namespace esphome
