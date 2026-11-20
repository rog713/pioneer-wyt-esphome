#pragma once

// The indoor unit's "BB" UART protocol, as the stock module firmware (Tuya esp_air_DIM_tcl 1.3.3) speaks it.
//
// Frames: BB <dir> <dir> <cmd> <len> <payload...> <xor of every earlier byte>; module -> unit is BB 00 01,
// unit -> module is BB 01 00. Commands: 0x04 status poll (the unit answers with a status frame), 0x03 SET (the unit
// answers with a status frame of type 0x03), 0x0A energy counters.
//
// Indexing: p[N] is status payload byte N and s[N] is SET payload byte N; both are packet byte N + 5.
// The stock firmware builds every SET from the unit's latest status (copy_status_to_set) and then applies one change,
// so settings it does not touch (timers, louvers, Fahrenheit, ...) are sent back as the unit reported them.

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
static constexpr uint8_t CMD_ENERGY = 0x0A;
static constexpr size_t SET_PAYLOAD_LEN = 29;
static constexpr size_t SET_PACKET_LEN = 5 + SET_PAYLOAD_LEN + 1;
static constexpr size_t HEARTBEAT_PACKET_LEN = 8;
static constexpr size_t ENERGY_QUERY_PACKET_LEN = 9;
// Status payload bytes copy_status_to_set reads (p[0] to p[54]).
static constexpr size_t STATUS_MIN_PAYLOAD = 55;
static constexpr uint8_t INVALID = 0xFF;

// Setpoint range in Fahrenheit mode (the unit's own table, every degree).
static constexpr int F_MIN = 61;
static constexpr int F_MAX = 89;
static constexpr int C_MIN = 16;
static constexpr int C_MAX = 31;

// Mode codes: SET s[3] bits 0-3 and status p[2] bits 0-3 use different numbers.
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

// Fan codes: SET s[5] bits 0-2 and status p[3] bits 4-6.
enum TxFan : uint8_t {
  TX_FAN_AUTO = 0,
  TX_FAN_LOW = 2,
  TX_FAN_MEDIUM = 3,
  TX_FAN_HIGH = 5,
  TX_FAN_MID_LOW = 6,
  TX_FAN_MID_HIGH = 7,
};

enum RxFan : uint8_t {
  RX_FAN_AUTO = 0,
  RX_FAN_LOW = 1,
  RX_FAN_MEDIUM = 2,
  RX_FAN_HIGH = 3,
  RX_FAN_MID_LOW = 4,
  RX_FAN_MID_HIGH = 5,
};

uint8_t checksum(const uint8_t *data, size_t len);
std::array<uint8_t, HEARTBEAT_PACKET_LEN> make_heartbeat();
// Asks for the energy counters (cmd 0x0A, payload 05 00 00), as the stock firmware does every third poll.
std::array<uint8_t, ENERGY_QUERY_PACKET_LEN> make_energy_query();

// The energy reply (cmd 0x0A), with e[N] its payload byte N: the unit's running total in kWh, BCD,
// e[6] e[5] e[4] . e[3] e[16] e[15] (six decimals), and its on-time in 2-minute ticks, e[17] | e[18] << 8. The unit
// updates both every 2 minutes. They live in the indoor board's memory, so both start again from 0 when it restarts.
// Checked against an Emporia meter on the circuit: the same 0.0906 kWh over two heating runs.
struct EnergyReading {
  double kwh{0};
  uint16_t ticks{0};
};
static constexpr uint16_t ENERGY_TICK_SECONDS = 120;
// False when the payload is too short or a counter digit is not BCD.
bool decode_energy(const uint8_t *e, size_t len, EnergyReading &out);
// The average power in W between two readings, from the energy used over the ticks between them. False when no tick
// has passed, or the counters went back or jumped more than an hour (the unit restarted, or replies were missed).
bool energy_power_w(const EnergyReading &prev, const EnergyReading &now, float &watts);
std::array<uint8_t, SET_PACKET_LEN> make_set_frame(const uint8_t *s);

// The stock firmware's starting SET payload: Cool, fan Auto, 26 °C (and 26 °C for Eco), display and beep on. The
// status copy overwrites most of it; fields the unit reports as 0xFF keep these values.
void init_set(uint8_t *s);

// Copies the unit's status into the SET payload s (SET_PAYLOAD_LEN bytes, kept between commands), as the stock
// firmware does after every status. p must hold at least STATUS_MIN_PAYLOAD bytes.
void copy_status_to_set(const uint8_t *p, uint8_t *s, bool beep);

// Changes applied on top of the copied status, with the stock firmware's side effects.
void set_power(uint8_t *s, bool on);
// Returns false when the unit is already in that mode (nothing changes, as in the stock firmware). Otherwise it
// also applies the fan the unit last used in that mode (fan code plus Turbo/Mute), as the stock firmware does;
// the setpoint is kept (the stock firmware would recall that mode's last setpoint too).
bool set_mode(uint8_t *s, uint8_t tx_mode, uint8_t fan, bool turbo, bool mute);
// In the unit's Fahrenheit mode (set from the remote) the half degree is ignored, as in the stock firmware.
void set_setpoint_c(uint8_t *s, int c, bool half);
// Puts the unit in Fahrenheit mode with a setpoint of f (F_MIN to F_MAX). Returns false if f is out of range.
bool set_setpoint_f(uint8_t *s, int f);
// Returns false in Dry, where the stock firmware ignores fan changes.
bool set_fan(uint8_t *s, uint8_t tx_fan, bool turbo, bool mute);
// The fan a mode starts with before the unit has been seen in it: Low in Dry, Auto otherwise.
uint8_t default_fan(uint8_t tx_mode);
// Sleep 0-4 (off, standard, elderly, child, auto); returns false unless the unit is on in Heat or Cool.
bool set_sleep(uint8_t *s, uint8_t sleep);
// Returns false unless the unit is on in Heat or Cool.
bool set_eco(uint8_t *s, bool on);
void set_display(uint8_t *s, bool on);
// Returns false unless the unit is on.
bool set_health(uint8_t *s, bool on);
void set_heater_8c(uint8_t *s, bool on);
// Louver motors on or off, both at once (the stock firmware's swing bitmask).
void set_swing(uint8_t *s, bool vertical, bool horizontal);
// Louver ranges (1-7, 0 stops the swing) and fixed positions (0-7).
void set_vertical_range(uint8_t *s, uint8_t range);
void set_vertical_fixed(uint8_t *s, uint8_t position);
void set_horizontal_range(uint8_t *s, uint8_t range);
void set_horizontal_fixed(uint8_t *s, uint8_t position);

bool is_on(const uint8_t *s);
uint8_t tx_mode(const uint8_t *s);

// Status decoding.
// Room temperature in °C (0.1 °C steps).
float room_temperature_c(const uint8_t *p);
bool fahrenheit_mode(const uint8_t *p);
// Setpoint in °F from the Fahrenheit table, or -1.
int setpoint_f(const uint8_t *p);
// Setpoint in °C, with the half degree.
float setpoint_c(const uint8_t *p);
// The stock firmware's 25-bit fault bitmap (its DP 20); needs plen > 51.
uint32_t fault_bits(const uint8_t *p, size_t plen);

uint8_t tx_mode_from_rx(uint8_t rx_mode);
uint8_t tx_fan_from_rx(uint8_t rx_fan);

const char *rx_mode_name(uint8_t rx_mode);
const char *rx_fan_name(uint8_t rx_fan, bool turbo, bool mute);
const char *sleep_name(uint8_t sleep);

}  // namespace bb_protocol
}  // namespace pioneer_minisplit
}  // namespace esphome
