#include "pioneer_minisplit.h"
#include "bb_protocol.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include <cmath>

namespace esphome {
namespace pioneer_minisplit {

namespace bb = bb_protocol;

static const char *const TAG = "pioneer_minisplit";

void PioneerMinisplit::update_history_(uint8_t *history, uint8_t value) {
  for (int i = 4; i > 0; i--) {
    history[i] = history[i - 1];
  }
  history[0] = value;
}

std::string PioneerMinisplit::format_history_(uint8_t *history) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X",
           history[0], history[1], history[2], history[3], history[4]);
  return std::string(buf);
}

void PioneerMinisplit::setup() {
  ESP_LOGI(TAG, "Pioneer Mini Split Controller Started");
  ESP_LOGI(TAG, "Protocol: 9600 8E1, XOR checksum");
  if (this->beep_switch_) this->beep_switch_->publish_state(true);
  bb::init_set(this->set_);

#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 4, 0)
  // ESPHome 2026.4.0+: Set custom modes on entity (new API)
  this->set_supported_custom_fan_modes({"Mute", "Low-Mid", "Mid-High", "Turbo"});
#endif

  this->send_heartbeat_();
}

void PioneerMinisplit::dump_config() {
  ESP_LOGCONFIG(TAG, "Pioneer Mini Split:");
  ESP_LOGCONFIG(TAG, "  Protocol: 9600 8E1 (Even parity), XOR checksum");
  ESP_LOGCONFIG(TAG, "  Setpoints: %s", this->fahrenheit_ ? "whole °F (unit in Fahrenheit mode)" : "°C, half degrees");
  ESP_LOGCONFIG(TAG, "  Energy query: %s", YESNO(this->energy_query_));
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_EVEN, 8);
}

void PioneerMinisplit::loop() {
  uint32_t now = millis();

  // Read incoming data
  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);

    if (this->rx_index_ == 0 && byte != 0xBB) {
      continue;
    }

    this->rx_buffer_[this->rx_index_++] = byte;
    this->last_rx_time_ = now;

    if (this->rx_index_ >= 5) {
      uint8_t payload_len = this->rx_buffer_[4];
      size_t expected = 5 + payload_len + 1;

      if (expected > sizeof(this->rx_buffer_)) {
        ESP_LOGW(TAG, "RX invalid length byte: len=%u expected=%u, dropping: %s",
                 payload_len, expected, this->hex_string_(this->rx_buffer_, this->rx_index_).c_str());
        this->rx_index_ = 0;
        continue;
      }

      if (this->rx_index_ >= expected) {
        this->process_packet_(this->rx_buffer_, this->rx_index_);
        this->rx_index_ = 0;
      }
    }

    if (this->rx_index_ >= sizeof(this->rx_buffer_)) {
      ESP_LOGW(TAG, "RX buffer overflow, dropping: %s",
               this->hex_string_(this->rx_buffer_, this->rx_index_).c_str());
      this->rx_index_ = 0;
    }
  }

  if (this->rx_index_ > 0 && (now - this->last_rx_time_) > RX_TIMEOUT) {
    ESP_LOGD(TAG, "RX timeout, discarding %d bytes", this->rx_index_);
    this->rx_index_ = 0;
  }

  // One frame in flight at a time, as the stock firmware does: the unit drops a command that arrives while it is
  // still applying the previous one or answering a poll (it takes about 250-330 ms to answer a SET).
  if (this->awaiting_cmd_ != 0) {
    if ((now - this->awaiting_since_) < this->awaiting_timeout_) return;
    const uint8_t timed_out = this->awaiting_cmd_;
    this->awaiting_cmd_ = 0;
    if (timed_out == bb::CMD_SET) this->on_set_timeout_();
  }
  if ((int32_t) (now - this->tx_block_until_) < 0) return;

  if (this->command_pending_ && (now - this->last_command_) >= COMMAND_COOLDOWN) {
    this->send_command_();
    this->command_pending_ = false;
    this->last_command_ = now;
    return;
  }
  if (this->resend_set_) {
    this->resend_set_ = false;
    ESP_LOGW(TAG, "No reply to the last command; sending it again (attempt %u)", this->set_attempts_);
    this->send_set_frame_();
    this->last_command_ = now;
    return;
  }
  if (this->post_command_status_pending_ || (now - this->last_heartbeat_) >= HEARTBEAT_INTERVAL) {
    this->post_command_status_pending_ = false;
    this->send_heartbeat_();
    this->last_heartbeat_ = now;
    if (this->energy_query_ && ++this->heartbeat_count_ >= 3) {
      this->heartbeat_count_ = 0;
      this->energy_query_pending_ = true;
    }
    return;
  }
  if (this->energy_query_pending_) {
    this->energy_query_pending_ = false;
    this->send_energy_query_();
  }
}

void PioneerMinisplit::expect_reply_(uint8_t cmd, uint32_t timeout) {
  this->awaiting_cmd_ = cmd;
  this->awaiting_since_ = millis();
  this->awaiting_timeout_ = timeout;
}

void PioneerMinisplit::on_set_timeout_() {
  if (this->set_replies_seen_ && this->set_attempts_ < SET_MAX_ATTEMPTS) {
    this->set_attempts_++;
    this->resend_set_ = true;
    return;
  }
  if (this->set_replies_seen_) ESP_LOGW(TAG, "No reply to the last command after %u attempts", SET_MAX_ATTEMPTS);
  this->awaiting_set_reply_ = false;
  this->replies_expected_ = 0;
  // Learn what the unit did from a status poll instead.
  this->post_command_status_pending_ = true;
}

uint8_t PioneerMinisplit::calculate_checksum_(const uint8_t *data, size_t len) {
  return bb_protocol::checksum(data, len);
}

std::string PioneerMinisplit::hex_string_(uint8_t *buf, size_t len) {
  char hex[3 * sizeof(this->rx_buffer_) + 1];
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < sizeof(hex) - 4; i++) {
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
  }
  hex[pos] = '\0';
  return std::string(hex);
}

void PioneerMinisplit::send_heartbeat_() {
  auto packet = bb_protocol::make_heartbeat();

  this->write_array(packet.data(), packet.size());
  this->flush();
  this->expect_reply_(bb::CMD_STATUS, STATUS_REPLY_TIMEOUT);

  this->tx_count_++;
  if (this->packets_tx_sensor_) this->packets_tx_sensor_->publish_state(this->tx_count_);
  if (this->last_tx_sensor_) this->last_tx_sensor_->publish_state(this->hex_string_(packet.data(), packet.size()));

  ESP_LOGD(TAG, "Sent heartbeat");
}

void PioneerMinisplit::send_energy_query_() {
  auto packet = bb_protocol::make_energy_query();
  this->write_array(packet.data(), packet.size());
  this->flush();
  this->expect_reply_(bb::CMD_ENERGY, STATUS_REPLY_TIMEOUT);
  this->tx_count_++;
  if (this->packets_tx_sensor_) this->packets_tx_sensor_->publish_state(this->tx_count_);
  ESP_LOGD(TAG, "Sent energy query");
}

void PioneerMinisplit::send_command_() {
  // In Fahrenheit, the first command also puts the unit in its Fahrenheit mode at the °F of its current setpoint.
  if (this->fahrenheit_ && !(this->set_[7] & 0x80)) {
    const float c = (31 - (this->set_[4] & 0x0F)) + ((this->set_[6] & 0x02) ? 0.5f : 0.0f);
    int f = (int) lroundf(c * 1.8f + 32.0f);
    if (f < bb::F_MIN) f = bb::F_MIN;
    if (f > bb::F_MAX) f = bb::F_MAX;
    bb::set_setpoint_f(this->set_, f);
  }
  this->last_set_frame_ = bb::make_set_frame(this->set_);
  this->set_attempts_ = 1;
  this->replies_expected_ = (this->awaiting_set_reply_ ? this->replies_expected_ : 0) + 1;
  this->awaiting_set_reply_ = true;
  this->send_set_frame_();
  ESP_LOGI(TAG, "TX SET: %s", this->hex_string_(this->last_set_frame_.data(), this->last_set_frame_.size()).c_str());
}

void PioneerMinisplit::send_set_frame_() {
  this->write_array(this->last_set_frame_.data(), this->last_set_frame_.size());
  this->flush();
  this->expect_reply_(bb::CMD_SET, SET_REPLY_TIMEOUT);

  this->tx_count_++;
  if (this->packets_tx_sensor_) this->packets_tx_sensor_->publish_state(this->tx_count_);
  if (this->last_tx_sensor_)
    this->last_tx_sensor_->publish_state(this->hex_string_(this->last_set_frame_.data(), this->last_set_frame_.size()));
}

void PioneerMinisplit::process_packet_(uint8_t *buf, size_t len) {
  uint8_t calc_xor = this->calculate_checksum_(buf, len - 1);
  bool checksum_ok = (calc_xor == buf[len - 1]);

  ESP_LOGD(TAG, "RX: %d bytes, cmd=0x%02X%s", len, buf[3], checksum_ok ? "" : " (BAD CSUM)");

  this->rx_count_++;
  if (this->packets_rx_sensor_) this->packets_rx_sensor_->publish_state(this->rx_count_);
  if (this->last_rx_sensor_) this->last_rx_sensor_->publish_state(this->hex_string_(buf, len));

  if (!checksum_ok || buf[1] != bb::DIR_FROM_HVAC || buf[2] != bb::DIR_HVAC) return;

  const bool answered_set = this->awaiting_cmd_ == bb::CMD_SET;
  if (this->awaiting_cmd_ == buf[3]) this->awaiting_cmd_ = 0;
  this->tx_block_until_ = millis() + FRAME_GAP;

  switch (buf[3]) {
    case bb::CMD_SET:
      // The unit's answer to a SET: a full status with the command applied.
      this->set_replies_seen_ = true;
      if (this->replies_expected_ > 1) {
        // Answers an older SET; the newest one is still out (and keeps its retry and confirm poll).
        this->replies_expected_--;
        ESP_LOGD(TAG, "Skipping a reply to an earlier command");
        return;
      }
      this->replies_expected_ = 0;
      this->awaiting_set_reply_ = false;
      if (this->command_pending_) {
        ESP_LOGD(TAG, "Skipping a reply while a newer command waits");
        return;
      }
      this->post_command_status_pending_ = false;
      this->decode_rx_packet_(buf, len);
      break;
    case bb::CMD_STATUS:
      // While a command waits to be sent, or has been sent but neither its reply nor the status read after it has
      // arrived, any status received answers an earlier request and still shows the old settings. Decoding it would
      // publish the old state and could carry it into the next command.
      if (this->command_pending_ || this->post_command_status_pending_ || answered_set) {
        ESP_LOGD(TAG, "Skipping status from before the last command");
        return;
      }
      this->decode_rx_packet_(buf, len);
      break;
    case bb::CMD_ENERGY:
      this->handle_energy_packet_(buf, len);
      break;
    default:
      ESP_LOGD(TAG, "Ignoring frame type 0x%02X", buf[3]);
      break;
  }
}

void PioneerMinisplit::handle_energy_packet_(uint8_t *buf, size_t len) {
  if (this->energy_raw_sensor_) this->energy_raw_sensor_->publish_state(this->hex_string_(buf, len));
  bb::EnergyReading now;
  if (len < 6 || !bb::decode_energy(buf + 5, len - 6, now)) {
    ESP_LOGW(TAG, "Energy reply not understood (%u bytes): %s", (unsigned) len, this->hex_string_(buf, len).c_str());
    return;
  }
  ESP_LOGD(TAG, "Energy: %.6f kWh, tick %u", now.kwh, (unsigned) now.ticks);
  if (this->have_energy_ && now.ticks == this->last_energy_.ticks && now.kwh == this->last_energy_.kwh) return;

  if (this->energy_sensor_ && (!this->have_energy_ || now.kwh != this->last_energy_.kwh))
    this->energy_sensor_->publish_state(float(now.kwh));
  if (this->have_energy_ && now.ticks != this->last_energy_.ticks) {
    float watts;
    if (bb::energy_power_w(this->last_energy_, now, watts)) {
      if (this->power_usage_sensor_) this->power_usage_sensor_->publish_state(watts);
    } else {
      ESP_LOGI(TAG, "Energy counters restarted (%.6f kWh, tick %u -> %.6f kWh, tick %u)", this->last_energy_.kwh,
               (unsigned) this->last_energy_.ticks, now.kwh, (unsigned) now.ticks);
    }
  }
  this->last_energy_ = now;
  this->have_energy_ = true;
}

void PioneerMinisplit::decode_rx_packet_(uint8_t *buf, size_t len) {
  if (len < 18) return;

  // Payload: p[N] = packet byte N + 5, up to the length the frame declares.
  const uint8_t *p = buf + 5;
  size_t plen = len - 6;
  if (buf[4] < plen) plen = buf[4];

  // The first status, and any status whose power/mode, fan/setpoint or flag bytes (7-10) changed, go to the log in
  // full: they show what the remote or a command changed.
  {
    const uint32_t key = (uint32_t(buf[7]) << 24) | (uint32_t(buf[8]) << 16) | (uint32_t(buf[9]) << 8) | buf[10];
    if (!this->first_status_logged_ || key != this->last_status_key_) {
      ESP_LOGI(TAG, "Status (%u bytes): %s", (unsigned) len, this->hex_string_(buf, len).c_str());
      this->first_status_logged_ = true;
      this->last_status_key_ = key;
    }
  }

  uint8_t power_mode = buf[7];
  uint8_t fan_temp = buf[8];
  uint8_t byte9 = buf[9];
  uint8_t byte10 = buf[10];
  uint8_t byte11 = len > 11 ? buf[11] : 0;
  uint8_t byte12 = len > 12 ? buf[12] : 0;
  uint8_t byte18 = len > 18 ? buf[18] : 0;
  uint8_t byte19 = len > 19 ? buf[19] : 0;
  uint8_t byte30 = len > 30 ? buf[30] : 0;
  uint8_t byte32 = len > 32 ? buf[32] : 0;
  uint8_t byte33 = len > 33 ? buf[33] : 0;
  uint8_t byte34 = len > 34 ? buf[34] : 0;
  uint8_t byte35 = len > 35 ? buf[35] : 0;
  uint8_t byte36 = len > 36 ? buf[36] : 0;
  uint8_t byte37 = len > 37 ? buf[37] : 0;
  uint8_t byte50 = len > 51 ? buf[51] : 0;
  uint8_t byte51 = len > 52 ? buf[52] : 0;

  bool power = (power_mode & 0x10) != 0;
  bool turbo = (power_mode & 0x80) != 0;
  bool eco = (power_mode & 0x40) != 0;
  bool display = (power_mode & 0x20) != 0;
  uint8_t mode = power_mode & 0x0F;

  // Fan: bits 4-6 (bit 7 is a protocol-version flag, not part of the speed).
  uint8_t fan = (fan_temp >> 4) & 0x07;

  bool timer_active = (byte9 & 0x40) != 0;
  bool health = (byte9 & 0x04) != 0;

  bool swing_v_active = (byte10 & 0x40) != 0;
  bool swing_h_active = (byte10 & 0x20) != 0;

  uint8_t sleep_mode = byte19 & 0x07;

  bool heater_8c = (byte32 & 0x80) != 0;
  bool mute_flag = (byte33 & 0x80) != 0;

  // Setpoint: in the unit's Fahrenheit mode the °C code plus an extra bit selects a whole °F; otherwise °C with an
  // optional half degree.
  const bool unit_fahrenheit = bb::fahrenheit_mode(p);
  this->unit_fahrenheit_ = unit_fahrenheit;
  const int set_f = unit_fahrenheit ? bb::setpoint_f(p) : -1;
  const float set_temp = set_f > 0 ? (set_f - 32) / 1.8f : bb::setpoint_c(p);

  // Room temperature, as the stock firmware reads it: (byte17 x 4 + byte18 bits 2-3 - 200) / 10 °C. Only move the
  // published value once it changes, so equal readings write nothing new.
  const float current_temp = bb::room_temperature_c(p);
  if (!std::isnan(current_temp) && (std::isnan(this->published_temp_c_) ||
                                    std::fabs(current_temp - this->published_temp_c_) >= TEMP_HYSTERESIS_C)) {
    this->published_temp_c_ = current_temp;
  }

  ESP_LOGD(TAG, "RX: %s %s %.1fC %.1fC fan=%X", power ? "ON" : "OFF", this->mode_str_(mode), set_temp,
           current_temp, fan);

  // Update sensors (publish Celsius - HA will convert to user's preferred unit)
  if (this->set_temp_sensor_) this->set_temp_sensor_->publish_state(set_temp);
  if (!std::isnan(this->published_temp_c_)) {
    if (this->current_temp_sensor_) this->current_temp_sensor_->publish_state(this->published_temp_c_);
  }

  if (len > 30) {
    float coil_temp_c = byte30 / 4.0f;
    if (this->coil_temp_sensor_) this->coil_temp_sensor_->publish_state(coil_temp_c);
  }

  if (this->power_sensor_) this->power_sensor_->publish_state(power ? "ON" : "OFF");
  if (this->mode_sensor_) this->mode_sensor_->publish_state(this->mode_str_(mode));
  if (this->fan_sensor_) this->fan_sensor_->publish_state(this->fan_full_str_(fan, turbo, mute_flag));

  if (this->swing_sensor_) {
    if (swing_v_active && swing_h_active) this->swing_sensor_->publish_state("Both");
    else if (swing_v_active) this->swing_sensor_->publish_state("Vertical");
    else if (swing_h_active) this->swing_sensor_->publish_state("Horizontal");
    else this->swing_sensor_->publish_state("Off");
  }

  if (this->eco_sensor_) this->eco_sensor_->publish_state(eco ? "ON" : "OFF");
  if (this->turbo_sensor_) this->turbo_sensor_->publish_state(turbo ? "ON" : "OFF");
  if (this->display_sensor_) this->display_sensor_->publish_state(display ? "ON" : "OFF");
  if (this->sleep_sensor_) this->sleep_sensor_->publish_state(this->sleep_str_(sleep_mode));
  if (this->health_sensor_) this->health_sensor_->publish_state(health ? "ON" : "OFF");
  if (this->timer_sensor_) this->timer_sensor_->publish_state(timer_active ? "ON" : "OFF");
  if (this->timer_time_sensor_) {
    if (timer_active && (byte11 > 0 || byte12 > 0)) {
      char time_buf[16];
      snprintf(time_buf, sizeof(time_buf), "%dh %dm", byte11, byte12);
      this->timer_time_sensor_->publish_state(time_buf);
    } else {
      this->timer_time_sensor_->publish_state("--");
    }
  }
  if (this->heater_8c_sensor_) this->heater_8c_sensor_->publish_state(heater_8c ? "ON" : "OFF");
  if (this->mute_sensor_) this->mute_sensor_->publish_state(mute_flag ? "ON" : "OFF");
  if (this->beep_sensor_) this->beep_sensor_->publish_state("N/A (TX only)");

  if (this->swing_v_sensor_) this->swing_v_sensor_->publish_state(this->swing_v_rx_str_(byte50, swing_v_active));
  if (this->swing_h_sensor_) this->swing_h_sensor_->publish_state(this->swing_h_rx_str_(byte51, swing_h_active));

  // Fault bits, as the stock firmware reports them (its DP 20); their names live in Tuya's cloud.
  if (this->fault_sensor_) {
    const uint32_t faults = bb::fault_bits(p, plen);
    if (faults == 0) {
      this->fault_sensor_->publish_state("None");
    } else {
      std::string out = "Fault bits:";
      for (int i = 0; i < 25; i++) {
        if (faults & (1UL << i)) out += " " + to_string(i);
      }
      this->fault_sensor_->publish_state(out);
    }
  }

  // Raw byte sensors
  char hex_buf[8];
  if (this->byte9_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte9); this->byte9_sensor_->publish_state(hex_buf); }
  if (this->byte10_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte10); this->byte10_sensor_->publish_state(hex_buf); }
  if (this->byte18_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte18); this->byte18_sensor_->publish_state(hex_buf); }
  if (this->byte19_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte19); this->byte19_sensor_->publish_state(hex_buf); }
  if (this->byte30_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte30); this->byte30_sensor_->publish_state(hex_buf); }
  if (this->byte32_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte32); this->byte32_sensor_->publish_state(hex_buf); }
  if (this->byte33_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte33); this->byte33_sensor_->publish_state(hex_buf); }
  if (this->byte34_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte34); this->byte34_sensor_->publish_state(hex_buf); }
  if (this->byte35_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte35); this->byte35_sensor_->publish_state(hex_buf); }
  if (this->byte36_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte36); this->byte36_sensor_->publish_state(hex_buf); }
  if (this->byte37_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte37); this->byte37_sensor_->publish_state(hex_buf); }
  if (this->byte50_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte50); this->byte50_sensor_->publish_state(hex_buf); }
  if (this->byte51_sensor_) { snprintf(hex_buf, sizeof(hex_buf), "0x%02X", byte51); this->byte51_sensor_->publish_state(hex_buf); }

  // History
  this->update_history_(this->byte30_history_, byte30);
  this->update_history_(this->byte34_history_, byte34);
  this->update_history_(this->byte35_history_, byte35);
  this->update_history_(this->byte36_history_, byte36);
  this->update_history_(this->byte37_history_, byte37);

  if (this->byte30_history_sensor_) this->byte30_history_sensor_->publish_state(this->format_history_(this->byte30_history_));
  if (this->byte34_history_sensor_) this->byte34_history_sensor_->publish_state(this->format_history_(this->byte34_history_));
  if (this->byte35_history_sensor_) this->byte35_history_sensor_->publish_state(this->format_history_(this->byte35_history_));
  if (this->byte36_history_sensor_) this->byte36_history_sensor_->publish_state(this->format_history_(this->byte36_history_));
  if (this->byte37_history_sensor_) this->byte37_history_sensor_->publish_state(this->format_history_(this->byte37_history_));

  // Debug sensors
  if (len > 30) {
    if (this->debug_byte30_f_div4_) this->debug_byte30_f_div4_->publish_state(this->c_to_f_(byte30 / 4.0f));
    if (this->debug_byte30_state_) this->debug_byte30_state_->publish_state((float)byte30);
  }
  if (len > 34) {
    if (this->debug_byte34_f_div4_) this->debug_byte34_f_div4_->publish_state(this->c_to_f_(byte34 / 4.0f));
    if (this->debug_byte34_state_) this->debug_byte34_state_->publish_state((float)byte34);
  }
  if (len > 35) {
    uint16_t p3435 = (uint16_t(buf[34]) << 8) | buf[35];
    if (this->debug_pair_34_35_f_374_) this->debug_pair_34_35_f_374_->publish_state(p3435 / 374.0f);
    if (this->debug_pair_34_35_state_) this->debug_pair_34_35_state_->publish_state((float)p3435);
    if (this->debug_byte35_f_div4_) this->debug_byte35_f_div4_->publish_state(this->c_to_f_(byte35 / 4.0f));
    if (this->debug_byte35_state_) this->debug_byte35_state_->publish_state((float)byte35);
  }
  if (len > 36) {
    uint16_t p3536 = (uint16_t(buf[35]) << 8) | buf[36];
    if (this->debug_pair_35_36_f_374_) this->debug_pair_35_36_f_374_->publish_state(p3536 / 374.0f);
    if (this->debug_pair_35_36_state_) this->debug_pair_35_36_state_->publish_state((float)p3536);
    if (this->debug_byte36_f_div4_) this->debug_byte36_f_div4_->publish_state(this->c_to_f_(byte36 / 4.0f));
    if (this->debug_byte36_state_) this->debug_byte36_state_->publish_state((float)byte36);
  }
  if (len > 37) {
    uint16_t p3637 = (uint16_t(buf[36]) << 8) | buf[37];
    if (this->debug_pair_36_37_f_374_) this->debug_pair_36_37_f_374_->publish_state(p3637 / 374.0f);
    if (this->debug_pair_36_37_state_) this->debug_pair_36_37_state_->publish_state((float)p3637);
    if (this->debug_byte37_f_div4_) this->debug_byte37_f_div4_->publish_state(this->c_to_f_(byte37 / 4.0f));
    if (this->debug_byte37_state_) this->debug_byte37_state_->publish_state((float)byte37);
  }

  // Outdoor temp: byte35 - 20 = °C (checked against the weather on the tested unit; the stock firmware ignores it)
  if (len > 35) {
    float outdoor_c = buf[35] - 20.0f;
    if (this->outdoor_temp_sensor_) this->outdoor_temp_sensor_->publish_state(outdoor_c);
  }

  // Condenser coil temp: byte36 = °C
  if (len > 36) {
    float condenser_c = (float)buf[36];
    if (this->condenser_coil_temp_sensor_) this->condenser_coil_temp_sensor_->publish_state(condenser_c);
  }

  // Compressor discharge temp: byte37 = °C
  if (len > 37) {
    float discharge_c = (float)buf[37];
    if (this->discharge_temp_sensor_) this->discharge_temp_sensor_->publish_state(discharge_c);
  }

  // Compressor frequency: byte38 = Hz
  if (len > 38) {
    if (this->compressor_freq_sensor_) this->compressor_freq_sensor_->publish_state((float)buf[38]);
  }

  // Indoor fan RPM: byte34 (0=off, 60=low, 85=med, 98=high)
  if (len > 34) {
    if (this->indoor_fan_rpm_sensor_) this->indoor_fan_rpm_sensor_->publish_state((float)byte34);
  }

  // Outdoor fan speed: byte39
  if (len > 39) {
    if (this->outdoor_fan_speed_sensor_) this->outdoor_fan_speed_sensor_->publish_state((float)buf[39]);
  }

  // Current amps: byte46
  if (len > 46) {
    if (this->current_amps_sensor_) this->current_amps_sensor_->publish_state((float)buf[46]);
  }

  // Heat mode active: byte40 bit 6
  if (len > 40) {
    bool heat_mode_active = (buf[40] & 0x40) != 0;
    if (this->heat_mode_active_sensor_) this->heat_mode_active_sensor_->publish_state(heat_mode_active ? "ON" : "OFF");

    // Outdoor unit status: byte40 bits 0-3 (0x00 = Idle, 0x0A = Running)
    uint8_t outdoor_status = buf[40] & 0x0F;
    if (this->outdoor_running_sensor_) {
      if (outdoor_status == 0x0A) {
        this->outdoor_running_sensor_->publish_state("Running");
      } else if (outdoor_status == 0x00) {
        this->outdoor_running_sensor_->publish_state("Idle");
      } else {
        char buf_str[8];
        snprintf(buf_str, sizeof(buf_str), "0x%02X", outdoor_status);
        this->outdoor_running_sensor_->publish_state(buf_str);
      }
    }
  }

  // Update climate entity from RX data ONLY.
  // The action follows the outdoor unit (byte 40 bits 0-3: 0x00 idle, 0x0A running; bit 6 set while heating), so
  // the entity says Idle once the room has reached the setpoint. A short packet without byte 40 counts as running.
  const bool outdoor_known = len > 40;
  const bool outdoor_running = !outdoor_known || (buf[40] & 0x0F) != 0x00;
  const bool outdoor_heating = outdoor_known && (buf[40] & 0x40) != 0;
  if (!power) {
    this->mode = climate::CLIMATE_MODE_OFF;
    this->action = climate::CLIMATE_ACTION_OFF;
  } else {
    // RX mode: 01=Cool, 02=Fan, 03=Dry, 04=Heat, 05=Auto
    switch (mode) {
      case 0x01:
        this->mode = climate::CLIMATE_MODE_COOL;
        this->action = outdoor_running ? climate::CLIMATE_ACTION_COOLING : climate::CLIMATE_ACTION_IDLE;
        break;
      case 0x02:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        this->action = climate::CLIMATE_ACTION_FAN;
        break;
      case 0x03:
        this->mode = climate::CLIMATE_MODE_DRY;
        this->action = outdoor_running ? climate::CLIMATE_ACTION_DRYING : climate::CLIMATE_ACTION_IDLE;
        break;
      case 0x04:
        this->mode = climate::CLIMATE_MODE_HEAT;
        this->action = outdoor_running ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE;
        break;
      case 0x05:
        this->mode = climate::CLIMATE_MODE_HEAT_COOL;
        if (!outdoor_known || !outdoor_running) {
          this->action = climate::CLIMATE_ACTION_IDLE;
        } else {
          this->action = outdoor_heating ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_COOLING;
        }
        break;
      default: this->mode = climate::CLIMATE_MODE_OFF; break;
    }
  }

  // Fan mode - use custom modes for Turbo/Mute and intermediate speeds.
  if (turbo) {
    this->set_custom_fan_mode_("Turbo");
  } else if (mute_flag) {
    this->set_custom_fan_mode_("Mute");
  } else if (fan == bb::RX_FAN_MID_LOW) {
    this->set_custom_fan_mode_("Low-Mid");
  } else if (fan == bb::RX_FAN_MID_HIGH) {
    this->set_custom_fan_mode_("Mid-High");
  } else {
    this->clear_custom_fan_mode_();
    switch (fan) {
      case bb::RX_FAN_AUTO: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
      case bb::RX_FAN_LOW: this->fan_mode = climate::CLIMATE_FAN_LOW; break;
      case bb::RX_FAN_MEDIUM: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
      case bb::RX_FAN_HIGH: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
      default: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
    }
  }

  this->preset = climate::CLIMATE_PRESET_NONE;

  // Swing
  if (swing_v_active && swing_h_active) this->swing_mode = climate::CLIMATE_SWING_BOTH;
  else if (swing_v_active) this->swing_mode = climate::CLIMATE_SWING_VERTICAL;
  else if (swing_h_active) this->swing_mode = climate::CLIMATE_SWING_HORIZONTAL;
  else this->swing_mode = climate::CLIMATE_SWING_OFF;

  this->target_temperature = set_temp;
  if (!std::isnan(this->published_temp_c_)) this->current_temperature = this->published_temp_c_;
  this->publish_state();

  // Remember the fan this mode runs with, for the next change into it.
  {
    const uint8_t tx_mode = bb::tx_mode_from_rx(mode);
    const uint8_t tx_fan = bb::tx_fan_from_rx(fan);
    if (power && tx_mode != bb::INVALID && tx_fan != bb::INVALID) {
      this->mode_fan_[tx_mode & 0x0F] = {tx_fan, turbo, mute_flag, true};
    }
  }

  // The next command starts from this status, as in the stock firmware. (Decoding is skipped while a command
  // waits, so a change made here is never overwritten before it is sent.)
  if (!this->command_pending_ && plen >= bb::STATUS_MIN_PAYLOAD) {
    bb::copy_status_to_set(p, this->set_, true);
    this->set_valid_ = true;

    if (this->fahrenheit_) {
      if (unit_fahrenheit && this->pending_f_ > 0) {
        // The unit has switched to Fahrenheit; send the °F that the switch dropped.
        if (set_f != this->pending_f_) {
          ESP_LOGI(TAG, "Unit now in Fahrenheit; sending %d F again", this->pending_f_);
          bb::set_setpoint_f(this->set_, this->pending_f_);
          this->command_pending_ = true;
        }
        this->pending_f_ = -1;
      } else if (!unit_fahrenheit && !this->fahrenheit_switch_sent_ && !this->command_pending_) {
        // Put the unit in its Fahrenheit mode once at start (send_command_ keeps the setpoint).
        ESP_LOGI(TAG, "Unit in Celsius; switching it to Fahrenheit");
        this->fahrenheit_switch_sent_ = true;
        this->command_pending_ = true;
      }
    }

    if (this->display_switch_) this->display_switch_->publish_state(display);
    if (this->health_switch_) this->health_switch_->publish_state(health);
    if (this->heater_8c_switch_) this->heater_8c_switch_->publish_state(heater_8c);
    if (this->eco_switch_) this->eco_switch_->publish_state(eco);
    if (this->turbo_switch_) this->turbo_switch_->publish_state(turbo);
    if (this->mute_switch_) this->mute_switch_->publish_state(mute_flag);
    if (this->sleep_switch_) this->sleep_switch_->publish_state(sleep_mode > 0);
    if (this->swing_v_switch_) this->swing_v_switch_->publish_state(swing_v_active);
    if (this->swing_h_switch_) this->swing_h_switch_->publish_state(swing_h_active);
    // Beep is TX-only and intentionally always enabled for outgoing commands.
    if (this->beep_switch_) this->beep_switch_->publish_state(true);
  } else if (plen < bb::STATUS_MIN_PAYLOAD) {
    ESP_LOGW(TAG, "Status too short to build commands from (%u payload bytes)", (unsigned) plen);
  }

  // Update select states from RX data
  if (this->swing_v_select_) {
    this->swing_v_select_->publish_state(this->swing_v_rx_str_(byte50, swing_v_active));
  }
  if (this->swing_h_select_) {
    this->swing_h_select_->publish_state(this->swing_h_rx_str_(byte51, swing_h_active));
  }
  if (this->sleep_select_) {
    this->sleep_select_->publish_state(this->sleep_str_(sleep_mode));
  }
}

const char* PioneerMinisplit::mode_str_(uint8_t mode) {
  return bb_protocol::rx_mode_name(mode);
}

const char* PioneerMinisplit::fan_full_str_(uint8_t fan, bool turbo, bool mute) {
  return bb_protocol::rx_fan_name(fan, turbo, mute);
}

const char* PioneerMinisplit::sleep_str_(uint8_t sleep) {
  return bb_protocol::sleep_name(sleep);
}

// Louver status bytes: while swinging, bits 3-5 hold the range (0 counts as 1); otherwise bits 0-2 hold the fixed
// position. Range and position names on this unit are not verified.
const char* PioneerMinisplit::swing_v_rx_str_(uint8_t byte50, bool swing_active) {
  if (swing_active) {
    switch ((byte50 >> 3) & 0x07) {
      case 0:
      case 1: return "Auto Swing";
      case 2: return "Swing Upper";
      case 3: return "Swing Lower";
      default: return "Unknown";
    }
  }
  switch (byte50 & 0x07) {
    case 0: return "Off";
    case 1: return "Fixed 1 (Top)";
    case 2: return "Fixed 2 (Upper)";
    case 3: return "Fixed 3 (Middle)";
    case 4: return "Fixed 4 (Mid-Low)";
    case 5: return "Fixed 5 (Bottom)";
    default: return "Unknown";
  }
}

const char* PioneerMinisplit::swing_h_rx_str_(uint8_t byte51, bool swing_active) {
  if (swing_active) {
    switch ((byte51 >> 3) & 0x07) {
      case 0:
      case 1: return "Auto Swing";
      case 2: return "Swing Left";
      case 3: return "Swing Center";
      case 4: return "Swing Right";
      default: return "Unknown";
    }
  }
  switch (byte51 & 0x07) {
    case 0: return "Off";
    case 1: return "Fixed 1 (Far Left)";
    case 2: return "Fixed 2 (Left)";
    case 3: return "Fixed 3 (Center)";
    case 4: return "Fixed 4 (Right)";
    case 5: return "Fixed 5 (Far Right)";
    case 6: return "Fixed 6 (Rightmost)";
    default: return "Unknown";
  }
}

void PioneerMinisplit::control(const climate::ClimateCall &call) {
  // Commands are built from the unit's own status, so wait for one.
  if (!this->set_valid_) {
    ESP_LOGW(TAG, "Ignoring command - waiting for initial state sync from HVAC");
    return;
  }
  uint8_t *s = this->set_;
  bool changed = false;

  if (call.get_mode().has_value()) {
    climate::ClimateMode new_mode = *call.get_mode();
    if (new_mode == climate::CLIMATE_MODE_OFF) {
      bb::set_power(s, false);
      this->action = climate::CLIMATE_ACTION_OFF;
    } else {
      uint8_t tx_mode = bb::INVALID;
      switch (new_mode) {
        case climate::CLIMATE_MODE_HEAT: tx_mode = bb::TX_MODE_HEAT; break;
        case climate::CLIMATE_MODE_DRY: tx_mode = bb::TX_MODE_DRY; break;
        case climate::CLIMATE_MODE_COOL: tx_mode = bb::TX_MODE_COOL; break;
        case climate::CLIMATE_MODE_FAN_ONLY: tx_mode = bb::TX_MODE_FAN; break;
        case climate::CLIMATE_MODE_HEAT_COOL: tx_mode = bb::TX_MODE_AUTO; break;
        default: break;
      }
      if (!bb::is_on(s)) bb::set_power(s, true);
      if (tx_mode != bb::INVALID) {
        const ModeFan &mf = this->mode_fan_[tx_mode & 0x0F];
        if (mf.seen) bb::set_mode(s, tx_mode, mf.fan, mf.turbo, mf.mute);
        else bb::set_mode(s, tx_mode, bb::default_fan(tx_mode), false, false);
      }
    }
    this->mode = new_mode;
    changed = true;
  }

  if (call.get_target_temperature().has_value()) {
    const float c = *call.get_target_temperature();
    if (this->fahrenheit_) {
      int f = (int) lroundf(c * 1.8f + 32.0f);
      if (f < bb::F_MIN) f = bb::F_MIN;
      if (f > bb::F_MAX) f = bb::F_MAX;
      bb::set_setpoint_f(s, f);
      this->pending_f_ = this->unit_fahrenheit_ ? -1 : f;
      this->target_temperature = (f - 32) / 1.8f;
    } else {
      float half_steps = roundf(c * 2.0f) / 2.0f;
      if (half_steps < bb::C_MIN) half_steps = bb::C_MIN;
      if (half_steps > bb::C_MAX) half_steps = bb::C_MAX;
      const int whole = (int) floorf(half_steps);
      bb::set_setpoint_c(s, whole, (half_steps - whole) > 0.25f);
      this->target_temperature = half_steps;
    }
    if (this->set_temp_sensor_) this->set_temp_sensor_->publish_state(this->target_temperature);
    changed = true;
  }

  // Standard fan modes
  if (call.get_fan_mode().has_value()) {
    climate::ClimateFanMode fan = *call.get_fan_mode();
    uint8_t tx_fan = bb::TX_FAN_AUTO;
    switch (fan) {
      case climate::CLIMATE_FAN_LOW: tx_fan = bb::TX_FAN_LOW; break;
      case climate::CLIMATE_FAN_MEDIUM: tx_fan = bb::TX_FAN_MEDIUM; break;
      case climate::CLIMATE_FAN_HIGH: tx_fan = bb::TX_FAN_HIGH; break;
      default: break;
    }
    if (bb::set_fan(s, tx_fan, false, false)) {
      this->clear_custom_fan_mode_();
      this->fan_mode = fan;
      changed = true;
    } else {
      ESP_LOGW(TAG, "Fan speed is fixed in Dry");
    }
  }

  // Custom fan modes (Turbo, Mute, and intermediate speeds)
  if (call.has_custom_fan_mode()) {
    auto custom_fan = call.get_custom_fan_mode();
    bool ok = false;
    if (custom_fan == "Turbo" || custom_fan == "Strong") {
      ok = bb::set_fan(s, bb::TX_FAN_HIGH, true, false);
      if (ok) {
        bb::set_eco(s, false);
        bb::set_sleep(s, 0);
        this->set_custom_fan_mode_("Turbo");
      }
    } else if (custom_fan == "Mute") {
      ok = bb::set_fan(s, bb::TX_FAN_LOW, false, true);
      if (ok) this->set_custom_fan_mode_("Mute");
    } else if (custom_fan == "Low-Mid" || custom_fan == "Mid-Low") {
      ok = bb::set_fan(s, bb::TX_FAN_MID_LOW, false, false);
      if (ok) this->set_custom_fan_mode_("Low-Mid");
    } else if (custom_fan == "Mid-High") {
      ok = bb::set_fan(s, bb::TX_FAN_MID_HIGH, false, false);
      if (ok) this->set_custom_fan_mode_("Mid-High");
    }
    if (ok) changed = true;
    else ESP_LOGW(TAG, "Fan mode not available now");
  }

  // Presets are not declared in traits, so calls carry none; Eco and Sleep are switches.

  if (changed) {
    this->command_pending_ = true;
    this->publish_state();
  }
}

climate::ClimateTraits PioneerMinisplit::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION | climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(bb::C_MIN);
  if (this->fahrenheit_) {
    // 61-89 °F, one degree at a time (Home Assistant shows the range and steps in °F).
    traits.set_visual_max_temperature((bb::F_MAX - 32) / 1.8f + 0.05f);
    traits.set_visual_target_temperature_step(1.0f);
  } else {
    traits.set_visual_max_temperature(bb::C_MAX);
    traits.set_visual_target_temperature_step(0.5f);
  }
  traits.set_visual_current_temperature_step(0.1f);

  traits.set_supported_modes({
    climate::CLIMATE_MODE_OFF,
    climate::CLIMATE_MODE_COOL,
    climate::CLIMATE_MODE_HEAT,
    climate::CLIMATE_MODE_DRY,
    climate::CLIMATE_MODE_FAN_ONLY,
    climate::CLIMATE_MODE_HEAT_COOL
  });

  // Standard fan modes
  traits.set_supported_fan_modes({
    climate::CLIMATE_FAN_AUTO,
    climate::CLIMATE_FAN_LOW,
    climate::CLIMATE_FAN_MEDIUM,
    climate::CLIMATE_FAN_HIGH,
  });

#if ESPHOME_VERSION_CODE < VERSION_CODE(2026, 4, 0)
  // Pre-2026.4.0: Set custom fan modes on traits (old API)
  traits.set_supported_custom_fan_modes({"Mute", "Low-Mid", "Mid-High", "Turbo"});
#endif
  // 2026.4.0+: Custom fan modes are set in setup() and wired automatically

  // Swing modes removed from climate - use separate select entities instead

  return traits;
}

// Switch implementation
void PioneerSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) return;
  const bool ok = this->parent_->set_feature(static_cast<SwitchType>(this->type_), state);
  if (this->type_ == SWITCH_BEEP) {
    this->publish_state(true);
    return;
  }
  // A change the unit's current mode does not allow snaps back at once.
  this->publish_state(ok ? state : this->state);
}

// Select implementation
void PioneerSelect::control(const std::string &value) {
  if (this->parent_ == nullptr) return;
  this->parent_->set_swing_position(static_cast<SelectType>(this->type_), value);
  this->publish_state(value);
}

// Feature control from switches
bool PioneerMinisplit::set_feature(SwitchType type, bool state) {
  if (!this->set_valid_) {
    ESP_LOGW(TAG, "Ignoring switch - waiting for initial state sync from HVAC");
    return false;
  }
  uint8_t *s = this->set_;
  const uint8_t current_fan = s[5] & 0x07;
  const bool swing_v = ((s[5] >> 3) & 0x07) != 0;
  const bool swing_h = (s[6] & 0x08) != 0;
  bool ok = true;

  switch (type) {
    case SWITCH_DISPLAY:
      bb::set_display(s, state);
      break;
    case SWITCH_BEEP:
      // Always on: every command asks the unit to beep.
      return true;
    case SWITCH_HEALTH:
      ok = bb::set_health(s, state);
      break;
    case SWITCH_HEATER_8C:
      bb::set_heater_8c(s, state);
      break;
    case SWITCH_ECO:
      ok = bb::set_eco(s, state);
      if (ok && state) {
        bb::set_fan(s, current_fan, false, false);  // turbo off
        bb::set_sleep(s, 0);
      }
      break;
    case SWITCH_TURBO:
      if (state) {
        ok = bb::set_fan(s, bb::TX_FAN_HIGH, true, false);
        if (ok) {
          bb::set_eco(s, false);
          bb::set_sleep(s, 0);
        }
      } else {
        ok = bb::set_fan(s, current_fan, false, false);
      }
      break;
    case SWITCH_MUTE:
      ok = state ? bb::set_fan(s, bb::TX_FAN_LOW, false, true) : bb::set_fan(s, current_fan, false, false);
      break;
    case SWITCH_SLEEP:
      ok = bb::set_sleep(s, state ? 1 : 0);
      if (ok && state) {
        bb::set_eco(s, false);
        bb::set_fan(s, current_fan, false, false);  // turbo and mute off
      }
      break;
    case SWITCH_SWING_V:
      bb::set_swing(s, state, swing_h);
      break;
    case SWITCH_SWING_H:
      bb::set_swing(s, swing_v, state);
      break;
  }
  if (!ok) {
    ESP_LOGW(TAG, "Switch %u not available in the unit's current mode", (unsigned) type);
    return false;
  }
  this->command_pending_ = true;
  return true;
}

// Louver position and sleep control from selects (positions unverified on this unit).
void PioneerMinisplit::set_swing_position(SelectType type, const std::string &value) {
  if (!this->set_valid_) {
    ESP_LOGW(TAG, "Ignoring select - waiting for initial state sync from HVAC");
    return;
  }
  uint8_t *s = this->set_;
  bool ok = true;

  if (type == SELECT_SLEEP) {
    uint8_t n = 0;
    if (value == "Standard") n = 1;
    else if (value == "Elderly") n = 2;
    else if (value == "Child") n = 3;
    else if (value == "Auto") n = 4;
    ok = bb::set_sleep(s, n);
    if (ok && n != 0) {
      bb::set_eco(s, false);
      bb::set_fan(s, s[5] & 0x07, false, false);
    }
  } else if (type == SELECT_SWING_V) {
    if (value == "Off") bb::set_vertical_range(s, 0);
    else if (value == "Auto Swing") bb::set_vertical_range(s, 1);
    else if (value == "Swing Upper") bb::set_vertical_range(s, 2);
    else if (value == "Swing Lower") bb::set_vertical_range(s, 3);
    else if (value == "Fixed 1 (Top)") bb::set_vertical_fixed(s, 1);
    else if (value == "Fixed 2 (Upper)") bb::set_vertical_fixed(s, 2);
    else if (value == "Fixed 3 (Middle)") bb::set_vertical_fixed(s, 3);
    else if (value == "Fixed 4 (Mid-Low)") bb::set_vertical_fixed(s, 4);
    else if (value == "Fixed 5 (Bottom)") bb::set_vertical_fixed(s, 5);
    else ok = false;
  } else if (type == SELECT_SWING_H) {
    if (value == "Off") bb::set_swing(s, ((s[5] >> 3) & 0x07) != 0, false);
    else if (value == "Auto Swing") bb::set_horizontal_range(s, 1);
    else if (value == "Swing Left") bb::set_horizontal_range(s, 2);
    else if (value == "Swing Center") bb::set_horizontal_range(s, 3);
    else if (value == "Swing Right") bb::set_horizontal_range(s, 4);
    else if (value == "Fixed 1 (Far Left)") bb::set_horizontal_fixed(s, 1);
    else if (value == "Fixed 2 (Left)") bb::set_horizontal_fixed(s, 2);
    else if (value == "Fixed 3 (Center)") bb::set_horizontal_fixed(s, 3);
    else if (value == "Fixed 4 (Right)") bb::set_horizontal_fixed(s, 4);
    else if (value == "Fixed 5 (Far Right)") bb::set_horizontal_fixed(s, 5);
    else if (value == "Fixed 6 (Rightmost)") bb::set_horizontal_fixed(s, 6);
    else ok = false;
  }
  if (!ok) {
    ESP_LOGW(TAG, "Select value '%s' not available now", value.c_str());
    return;
  }
  this->command_pending_ = true;
}

}  // namespace pioneer_minisplit
}  // namespace esphome
