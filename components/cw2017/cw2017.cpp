#include "cw2017.h"

#include <cinttypes>
#include <cstddef>
#include <cstring>
#ifdef USE_ESP32
#include "esp_attr.h"
#include "esp_system.h"
#endif

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace cw2017 {

static const char *const TAG = "cw2017";

#ifdef USE_ESP32
namespace {
// A dedicated RTC record: do not allocate from the reporter/display preferences pool.
struct RetainedPowerState {
  uint32_t magic, identity, sleeping, chip_version, checksum;
};
static constexpr uint32_t RTC_MAGIC = 0x43573231U;
static RTC_DATA_ATTR RetainedPowerState retained_power{};
uint32_t power_checksum(const RetainedPowerState &state) {
  uint32_t hash = 2166136261U;
  const auto *bytes = reinterpret_cast<const uint8_t *>(&state);
  for (size_t i = 0; i < offsetof(RetainedPowerState, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}
}  // namespace
#endif

void CW2017Component::invalidate_rtc_state_() {
#ifdef USE_ESP32
  retained_power.magic = 0;
#endif
}

void CW2017Component::save_rtc_state_() {
#ifdef USE_ESP32
  if (!this->initialized_) { this->invalidate_rtc_state_(); return; }
  RetainedPowerState next{RTC_MAGIC, this->rtc_identity_, this->sleeping_ ? 1U : 0U, this->chip_version_, 0};
  next.checksum = power_checksum(next);
  retained_power = next;
#endif
}

bool CW2017Component::try_resume_from_rtc_() {
#ifdef USE_ESP32
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || retained_power.magic != RTC_MAGIC ||
      retained_power.identity != this->rtc_identity_ || retained_power.sleeping > 1 ||
      retained_power.chip_version == 0 || retained_power.chip_version >= 0xFF ||
      retained_power.checksum != power_checksum(retained_power)) return false;
  const auto saved = retained_power;
  // A failed/partial wake must never leave a reusable success marker behind.
  this->invalidate_rtc_state_();
  uint8_t config, version;
  if (!this->read_register(CW2017_REG_CONFIG, &config)) {
    ESP_LOGW(TAG, "RTC resume: cannot read CONFIG before wake; falling back to full initialization");
    return false;
  }
  const uint8_t expected_config = saved.sleeping ? 0xF0 : 0x00;
  if (config != expected_config) {
    ESP_LOGW(TAG, "RTC resume: CONFIG mismatch (RTC expects 0x%02X, read 0x%02X); full initialization",
             expected_config, config);
    return false;
  }
  if (saved.sleeping && !this->wake_device()) {
    ESP_LOGW(TAG, "RTC resume: wake failed; falling back to full initialization");
    return false;
  }
  if (!this->read_register(CW2017_REG_CONFIG, &config)) {
    ESP_LOGW(TAG, "RTC resume: cannot read CONFIG after wake; falling back to full initialization");
    return false;
  }
  if (config != 0x00) {
    ESP_LOGW(TAG, "RTC resume: CONFIG is 0x%02X after wake, expected 0x00; full initialization", config);
    return false;
  }
  if (!this->read_version(&version)) {
    ESP_LOGW(TAG, "RTC resume: VERSION read failed; falling back to full initialization");
    return false;
  }
  if (version == 0x00 || version == 0xFF) {
    ESP_LOGW(TAG, "RTC resume: invalid VERSION 0x%02X; falling back to full initialization", version);
    return false;
  }
  if (version != saved.chip_version) {
    // Logs show 0xA0 during full initialization and 0x0F later. Do not assume the
    // early reading is a reliable invariant: retain CONFIG and full profile validation.
    if (!this->write_profile_) {
      ESP_LOGW(TAG, "RTC resume: VERSION changed (RTC=0x%02X current=0x%02X), no managed profile to verify; full initialization",
               unsigned(saved.chip_version), version);
      return false;
    }
    ESP_LOGW(TAG, "RTC resume: VERSION differs (RTC=0x%02X current=0x%02X); checking retained battery profile",
             unsigned(saved.chip_version), version);
  }
  if (this->write_profile_) {
    // The MCU's RTC cannot prove that the external gauge kept power. Read-only validation
    // detects an independently reset/lost profile without rewriting 80 bytes every wake.
    const uint8_t *expected = this->battery_profile_.size() == CW2017_PROFILE_SIZE ?
        this->battery_profile_.data() : CW2017_DEFAULT_PROFILE_4_2V;
    uint8_t actual[CW2017_PROFILE_SIZE];
    if (!this->read_registers(CW2017_REG_BATINFO, actual, sizeof(actual)) ||
        std::memcmp(actual, expected, sizeof(actual)) != 0) {
      ESP_LOGW(TAG, "RTC resume: battery profile missing/mismatched; falling back to full initialization");
      return false;
    }
  }
  this->chip_version_ = version;
  this->initialized_ = true; this->sleeping_ = false; this->fast_resume_ = true;
  this->save_rtc_state_();
  if (this->sleep_switch_) this->sleep_switch_->publish_state(false);
  ESP_LOGI(TAG, "RTC resume successful (previous sleep=%s); reset and profile write skipped", YESNO(saved.sleeping));
  // Allow ADC/SOC readiness to progress asynchronously; do not add the cold-start delay.
  if (this->soc_sensor_) this->set_timeout("soc_retry", 200, [this]() { this->schedule_soc_retry_(0); });
  return true;
#else
  return false;
#endif
}


// ============================================================================
// ESPHome 生命周期实现
// ============================================================================

void CW2017Component::setup() {
  if (this->try_resume_from_rtc_()) return;
  this->invalidate_rtc_state_();
  if (this->delay_init_ms_ > 0) {
    ESP_LOGI(TAG, "setup(): delay_init_on_boot=%" PRIu32 "ms, deferring I2C init", this->delay_init_ms_);
    // 立即返回，不阻塞其他组件启动
    // 通过set_timeout在指定延迟后执行真正的初始化
    this->set_timeout(this->delay_init_ms_, [this]() {
      this->perform_real_setup_();
    });
    return;
  }

  // 默认（delay=0ms）：立即执行初始化
  this->perform_real_setup_();
}

void CW2017Component::perform_real_setup_() {
  ESP_LOGCONFIG(TAG, "Setting up CW2017 (full initialization)...");
  this->invalidate_rtc_state_();
  this->initialized_ = false; this->sleeping_ = false; this->fast_resume_ = false;
  this->cancel_timeout("soc_retry");

  // 1. 强制复位设备（写 0xF0 到 CONFIG 完全复位所有寄存器）
  if (!this->write_register(CW2017_REG_CONFIG, 0xF0)) {
    ESP_LOGE(TAG, "Failed to reset device (write 0xF0)");
    this->mark_failed();
    return;
  }
  delay(100);  // 等待复位完成

  // 2. 写入电池配置文件
  if (this->write_profile_) {
    const uint8_t *profile = nullptr;
    size_t profile_size = 0;

    if (!this->battery_profile_.empty() && this->battery_profile_.size() == CW2017_PROFILE_SIZE) {
      profile = this->battery_profile_.data();
      profile_size = this->battery_profile_.size();
      ESP_LOGI(TAG, "Using user-provided battery profile (size=%zu)", profile_size);
    } else {
      profile = CW2017_DEFAULT_PROFILE_4_2V;
      profile_size = sizeof(CW2017_DEFAULT_PROFILE_4_2V);
      ESP_LOGI(TAG, "No battery_profile provided, using built-in 4.2V default profile (size=%zu)", profile_size);
    }

    if (!this->write_battery_profile_data(profile, profile_size)) {
      ESP_LOGE(TAG, "Failed to write battery profile");
      this->mark_failed();
      return;
    }

    // 等待设备处理配置（参考代码等待100ms）
    delay(100);
  }

  // 3. 唤醒设备（清除睡眠、重启进入正常模式）
  // Datasheet: 先写 0x30 清除睡眠，再写 0x00 触发重启
  if (!this->wake_device()) {
    ESP_LOGE(TAG, "Wake device failed");
    this->mark_failed();
    return;
  }

  // 等待 ADC 开始更新（datasheet: ADC 在 POR 后 <10ms 首次更新，之后每秒4次）
  delay(200);

  // 4. 验证通信：读取版本号
  uint8_t version;
  if (!this->read_version(&version)) {
    ESP_LOGE(TAG, "Failed to read version register - check I2C wiring");
    this->mark_failed();
    return;
  }

  this->chip_version_ = version;
  this->initialized_ = true;
  this->save_rtc_state_();
  if (this->sleep_switch_) this->sleep_switch_->publish_state(false);
  ESP_LOGI(TAG, "CW2017 initialized successfully, version: 0x%02X", version);

  // 初始化后SOC可能仍是0xFF（算法需要额外时间计算）
  // 不阻塞等待，安排延迟重试直到SOC就绪
  if (this->soc_sensor_ != nullptr) {
    this->schedule_soc_retry_(0);
  }
}

void CW2017Component::schedule_soc_retry_(int retry_count) {
  if (!this->initialized_ || this->sleeping_ || !this->soc_sensor_) return;
  const int limit = this->fast_resume_ ? 40 : 8;
  if (retry_count >= limit) {
    ESP_LOGW(TAG, "SOC still unavailable after %d retries, giving up", retry_count);
    return;
  }
  float soc;
  if (this->read_soc(&soc)) {
    this->soc_sensor_->publish_state(soc);
    ESP_LOGI(TAG, "Deferred SOC read success: %.2f%% (after %d retries)", soc, retry_count);
    // Recover voltage if an earlier acquisition occurred before the ADC became ready.
    if (this->voltage_sensor_ && !this->voltage_sensor_->has_state()) {
      float voltage;
      if (this->read_voltage(&voltage)) this->voltage_sensor_->publish_state(voltage);
    }
    return;
  }
  const uint32_t interval = this->fast_resume_ ? 200 : 1000;
  ESP_LOGD(TAG, "SOC not ready; scheduling retry %d in %ums", retry_count + 1, unsigned(interval));
  // Retained configuration resumes by read-only polling, without sleep/mode kicks.
  if (!this->fast_resume_) {
    this->write_register(CW2017_REG_INT_CONF, CW2017_MODE_SLEEP);
    delay(2);
    this->write_register(CW2017_REG_INT_CONF, 0x40);
  }
  this->set_timeout("soc_retry", interval, [this, retry_count]() {
    this->schedule_soc_retry_(retry_count + 1);
  });
}

void CW2017Component::update() {
  if (this->sleeping_) { ESP_LOGD(TAG, "Sleeping; skipping sensor acquisition"); return; }
  if (!this->initialized_) {
    ESP_LOGW(TAG, "Not initialized, skipping update");
    return;
  }

  // 单次直读，不循环重试
  // 原因: YAML时序保证调用update()时芯片已稳定
  //  touch → 指纹识别 → delay 5s → trigger_update
  //  5s足够CW2017完成ADC采样和SOC算法计算

  // 读取电压
  if (this->voltage_sensor_ != nullptr) {
    float voltage;
    if (this->read_voltage(&voltage)) {
      this->voltage_sensor_->publish_state(voltage);
      ESP_LOGI(TAG, "Voltage: %.3f V", voltage);
    } else {
      ESP_LOGW(TAG, "Failed to read voltage (VCELL=0)");
    }
  }

  // 读取SOC
  if (this->soc_sensor_ != nullptr) {
    float soc;
    if (this->read_soc(&soc)) {
      this->soc_sensor_->publish_state(soc);
      ESP_LOGI(TAG, "SOC: %.2f%%", soc);
    } else {
      ESP_LOGW(TAG, "Failed to read SOC (chip may still be waking up)");
    }
  }

  // 读取温度
  if (this->temperature_sensor_ != nullptr) {
    float temperature;
    if (this->read_temperature(&temperature)) {
      this->temperature_sensor_->publish_state(temperature);
      ESP_LOGI(TAG, "Temperature: %.1f °C", temperature);
    } else {
      ESP_LOGW(TAG, "Failed to read temperature");
    }
  }
}

void CW2017Component::dump_config() {
  ESP_LOGCONFIG(TAG, "CW2017 Fuel Gauge:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Write Profile: %s", YESNO(this->write_profile_));
  ESP_LOGCONFIG(TAG, "  RTC fast resume: %s; chip sleeping: %s", YESNO(this->fast_resume_), YESNO(this->sleeping_));
  if (this->delay_init_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Delay Init On Boot: %" PRIu32 "ms", this->delay_init_ms_);
  }
  if (!this->battery_profile_.empty()) {
    ESP_LOGCONFIG(TAG, "  Custom Battery Profile: %zu bytes", this->battery_profile_.size());
  } else {
    ESP_LOGCONFIG(TAG, "  Battery Profile: Default 4.2V");
  }
  LOG_SENSOR("  ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR("  ", "SOC", this->soc_sensor_);
  LOG_SENSOR("  ", "Temperature", this->temperature_sensor_);

  if (this->is_failed()) {
    ESP_LOGE(TAG, "  State: FAILED - check I2C wiring and VDD power");
  } else if (this->initialized_) {
    uint8_t version;
    if (this->read_version(&version)) {
      ESP_LOGCONFIG(TAG, "  Chip Version: 0x%02X", version);
    }
  }
}

float CW2017Component::get_setup_priority() const {
  // 需要在 I2C 总线初始化之后，但在大多数传感器之前
  return setup_priority::DATA;
}

// ============================================================================
// 控制接口 (由 switch/button 调用)
// ============================================================================

void CW2017Component::sleep() {
  if (!this->initialized_) { ESP_LOGW(TAG, "Not initialized, cannot sleep"); return; }
  this->cancel_timeout("soc_retry");
  this->invalidate_rtc_state_();
  ESP_LOGI(TAG, "Entering sleep mode...");
  uint8_t config;
  if (!this->write_register(CW2017_REG_CONFIG, 0xF0) ||
      !this->read_register(CW2017_REG_CONFIG, &config) || config != 0xF0) {
    ESP_LOGE(TAG, "Failed to confirm sleep mode; RTC resume marker invalidated");
    return;
  }
  this->sleeping_ = true;
  this->save_rtc_state_();
  if (this->sleep_switch_) this->sleep_switch_->publish_state(true);
  ESP_LOGI(TAG, "CW2017 entered sleep mode; RTC marker saved");
}

void CW2017Component::wake() {
  if (!this->initialized_) { ESP_LOGW(TAG, "Not initialized, cannot wake"); return; }
  if (!this->sleeping_) {
    if (this->sleep_switch_) this->sleep_switch_->publish_state(false);
    return;
  }
  this->cancel_timeout("soc_retry");
  this->invalidate_rtc_state_();
  ESP_LOGI(TAG, "Waking device from sleep...");
  uint8_t config;
  if (!this->wake_device() || !this->read_register(CW2017_REG_CONFIG, &config) || config != 0x00) {
    ESP_LOGE(TAG, "Failed to confirm wake; RTC resume marker invalidated");
    return;
  }
  this->sleeping_ = false; this->fast_resume_ = true;
  this->save_rtc_state_();
  if (this->sleep_switch_) this->sleep_switch_->publish_state(false);
  if (this->soc_sensor_) this->set_timeout("soc_retry", 200, [this]() { this->schedule_soc_retry_(0); });
  ESP_LOGI(TAG, "CW2017 wake sequence done; waiting for ADC asynchronously");
}

void CW2017Component::trigger_read() {
  if (!this->initialized_) {
    ESP_LOGW(TAG, "Not initialized, cannot trigger read");
    return;
  }

  ESP_LOGD(TAG, "Manual trigger: reading sensors...");
  this->update();
}

// ============================================================================
// 底层 I2C 操作
// ============================================================================

bool CW2017Component::read_register(uint8_t reg, uint8_t *data) {
  return this->read_byte(reg, data);
}

bool CW2017Component::write_register(uint8_t reg, uint8_t data) {
  return this->write_byte(reg, data);
}

bool CW2017Component::read_registers(uint8_t start_reg, uint8_t *data, size_t len) {
  if (len == 0) {
    return true;
  }
  return this->read_bytes(start_reg, data, len);
}

// ============================================================================
// 芯片操作
// ============================================================================

bool CW2017Component::wake_device() {
  // Datasheet Section "Power State":
  //   步骤1: 写 0x30 到 REG 0x08 清除睡眠模式
  //   步骤2: 写 0x00 到 REG 0x08 触发重启，进入正常模式
  ESP_LOGV(TAG, "Waking device...");

  if (!this->write_register(CW2017_REG_CONFIG, 0x30)) {
    ESP_LOGE(TAG, "Wake step 1 failed (write 0x30)");
    return false;
  }
  delay(5);

  if (!this->write_register(CW2017_REG_CONFIG, 0x00)) {
    ESP_LOGE(TAG, "Wake step 2 failed (write 0x00)");
    return false;
  }
  delay(15);

  return true;
}

bool CW2017Component::reset_device() {
  // Datasheet: 写 0xF0 到 CONFIG 完全复位，再写 0x30/0x00 唤醒
  ESP_LOGV(TAG, "Resetting device...");

  if (!this->write_register(CW2017_REG_CONFIG, 0xF0)) {
    ESP_LOGE(TAG, "Reset failed (write 0xF0)");
    return false;
  }
  delay(10);

  // 复位后需要重新唤醒
  return this->wake_device();
}

bool CW2017Component::read_version(uint8_t *version) {
  return this->read_register(CW2017_REG_VERSION, version);
}

bool CW2017Component::write_battery_profile_data(const uint8_t *profile, size_t len) {
  if (profile == nullptr || len == 0) {
    ESP_LOGE(TAG, "Invalid battery profile data");
    return false;
  }

  ESP_LOGI(TAG, "Writing battery profile to registers 0x%02X-0x%02X (size=%zu)",
           CW2017_REG_BATINFO, static_cast<unsigned int>(CW2017_REG_BATINFO + len - 1), len);

  // 逐字节写入电池配置信息（寄存器 0x10 ~ 0x5F）
  for (size_t i = 0; i < len; i++) {
    if (!this->write_register(CW2017_REG_BATINFO + i, profile[i])) {
      ESP_LOGE(TAG, "Failed to write profile byte %zu at register 0x%02X",
               i, static_cast<unsigned int>(CW2017_REG_BATINFO + i));
      return false;
    }
    delay(2);  // 每个寄存器写入间隔2ms，确保设备处理
  }

  // 等待配置稳定
  delay(10);

  // 写入后需要进行模式切换以使配置生效
  // 先进入睡眠模式，再切换到正常模式
  // 寄存器 0x0A (INT_CONF/MODE_CONFIG) 控制工作模式
  if (!this->write_register(CW2017_REG_INT_CONF, CW2017_MODE_SLEEP)) {
    ESP_LOGE(TAG, "Failed to enter sleep mode after profile write");
    return false;
  }
  delay(10);

  if (!this->write_register(CW2017_REG_INT_CONF, CW2017_MODE_NORMAL)) {
    ESP_LOGE(TAG, "Failed to enter normal mode after profile write");
    return false;
  }

  ESP_LOGI(TAG, "Battery profile written successfully");
  return true;
}

// ============================================================================
// 传感器读取
// ============================================================================

bool CW2017Component::read_voltage(float *voltage) {
  uint8_t data[2];

  // 读取 VCELL_H (0x02) 和 VCELL_L (0x03)
  if (!this->read_registers(CW2017_REG_VCELL_H, data, 2)) {
    ESP_LOGW(TAG, "Failed to read VCELL registers");
    return false;
  }

  // Datasheet Figure 3: VCELL 为 UNSIGNED 14bit
  // 0x02 高6位有效 (bits 13:8)，0x03 低8位 (bits 7:0)
  uint16_t vcell_raw = ((data[0] & 0x3F) << 8) | data[1];

  if (vcell_raw == 0) {
    ESP_LOGW(TAG, "VCELL raw == 0, ADC not ready yet");
    return false;
  }

  // Datasheet: V(µV) = Value(0x02 0x03 DEC) * 312.5
  // 转换为 V: voltage_V = (vcell_raw * 312.5) / 1,000,000
  // 使用整数运算四舍五入到 mV，再转为 V
  // mV = (vcell_raw * 3125 + 5000) / 10000
  uint32_t voltage_mv = ((uint32_t) vcell_raw * 3125U + 5000U) / 10000U;
  *voltage = voltage_mv / 1000.0f;

  return true;
}

bool CW2017Component::read_soc(float *soc) {
  uint8_t data[2];

  // 读取 SOC_H (0x04) 和 SOC_L (0x05)
  if (!this->read_registers(CW2017_REG_SOC_H, data, 2)) {
    ESP_LOGW(TAG, "Failed to read SOC registers");
    return false;
  }

  uint8_t soc_high = data[0];  // 整数百分比 (0~100)
  uint8_t soc_low = data[1];   // 小数部分 (LSB = 1/256%)

  // 校验异常值
  // Datasheet: SOC_H 有效范围为 0~100
  // 0xFF/0xFE 通常为未初始化/错误状态
  // (0, 0) 也可能表示未就绪
  if (soc_high > 100 || soc_high == 0xFF || soc_high == 0xFE ||
      (soc_high == 0x00 && soc_low == 0x00)) {
    ESP_LOGW(TAG, "Invalid SOC value: H=0x%02X L=0x%02X", soc_high, soc_low);

    // 有历史值时回退到历史值
    if (this->last_soc_ != 0) {
      *soc = this->last_soc_ / 100.0f;
      ESP_LOGD(TAG, "Using last valid SOC: %.2f%%", *soc);
      return true;
    }

    // 没有历史值时返回false，让update()不发布数据，避免从实际值跳变到100%
    ESP_LOGW(TAG, "No valid last SOC, returning false to skip publish");
    return false;
  }

  // Datasheet: SOC(%) = Value(0x04 DEC) + Value(0x05 DEC) / 256
  float soc_value = (float) soc_high + (float) soc_low / 256.0f;

  // 边界保护
  if (soc_value > 100.0f) {
    soc_value = 100.0f;
  }
  if (soc_value < 0.0f) {
    soc_value = 0.0f;
  }

  // 保存有效值用于异常回退（以 0.01% 为单位）
  this->last_soc_ = (uint16_t)(soc_value * 100.0f + 0.5f);

  *soc = soc_value;
  return true;
}

bool CW2017Component::read_temperature(float *temperature) {
  uint8_t temp_reg;

  // 读取 TEMP (0x06)
  if (!this->read_register(CW2017_REG_TEMP, &temp_reg)) {
    ESP_LOGW(TAG, "Failed to read TEMP register");
    return false;
  }

  // Datasheet: TEMP(°C) = -40 + Value(0x06 DEC) / 2
  // 范围: -40°C 到 87.5°C, LSB = 0.5°C
  *temperature = -40.0f + (float) temp_reg / 2.0f;

  ESP_LOGV(TAG, "Temperature raw: 0x%02X -> %.1f°C", temp_reg, *temperature);
  return true;
}

// ============================================================================
// 辅助类实现: 休眠开关
// ============================================================================

void CW2017SleepSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGW("cw2017.switch", "Parent not set");
    return;
  }

  // state=true 表示开关打开(ON) → 进入休眠
  // state=false 表示开关关闭(OFF) → 唤醒
  if (state) {
    ESP_LOGD("cw2017.switch", "Sleep switch ON -> entering sleep mode");
    this->parent_->sleep();
  } else {
    ESP_LOGD("cw2017.switch", "Sleep switch OFF -> waking device");
    this->parent_->wake();
  }

}

// ============================================================================
// 辅助类实现: 手动触发按钮
// ============================================================================

void CW2017TriggerButton::press_action() {
  if (this->parent_ == nullptr) {
    ESP_LOGW("cw2017.button", "Parent not set");
    return;
  }

  ESP_LOGD("cw2017.button", "Trigger button pressed -> reading sensors");
  this->parent_->trigger_read();
}

}  // namespace cw2017
}  // namespace esphome
