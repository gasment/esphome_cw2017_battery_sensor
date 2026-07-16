#include "cw2017.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace cw2017 {

static const char *const TAG = "cw2017";

// ============================================================================
// ESPHome 生命周期实现
// ============================================================================

void CW2017Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CW2017...");

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
      ESP_LOGI(TAG, "Using user-provided battery profile (size=%u)", profile_size);
    } else {
      profile = CW2017_DEFAULT_PROFILE_4_2V;
      profile_size = sizeof(CW2017_DEFAULT_PROFILE_4_2V);
      ESP_LOGI(TAG, "No battery_profile provided, using built-in 4.2V default profile (size=%u)", profile_size);
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
    ESP_LOGW(TAG, "Wake device failed, continuing anyway");
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

  this->initialized_ = true;
  ESP_LOGI(TAG, "CW2017 initialized successfully, version: 0x%02X", version);
}

void CW2017Component::update() {
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
  if (!this->battery_profile_.empty()) {
    ESP_LOGCONFIG(TAG, "  Custom Battery Profile: %u bytes", this->battery_profile_.size());
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
  if (!this->initialized_) {
    ESP_LOGW(TAG, "Not initialized, cannot sleep");
    return;
  }

  ESP_LOGI(TAG, "Entering sleep mode...");

  // Datasheet: 写 0xF0 到 CONFIG 使设备进入睡眠模式
  // 默认值 0xF0 中 SLEEP[1:0]=11, RESTART[3:0]=0000
  // 0xF0 = 0b11110000 → SLEEP=11(睡眠), RESTART=0000
  if (!this->write_register(CW2017_REG_CONFIG, 0xF0)) {
    ESP_LOGE(TAG, "Failed to enter sleep mode");
    return;
  }

  ESP_LOGI(TAG, "CW2017 entered sleep mode");
}

void CW2017Component::wake() {
  if (!this->initialized_) {
    ESP_LOGW(TAG, "Not initialized, cannot wake");
    return;
  }

  ESP_LOGI(TAG, "Waking device from sleep...");

  // Datasheet Power State:
  //   步骤1: 写 0x30 清除睡眠模式 (SLEEP=11→SLEEP=00, RESTART保持)
  //   步骤2: 写 0x00 触发重启进入正常模式
  if (!this->wake_device()) {
    ESP_LOGE(TAG, "Failed to wake device");
    return;
  }

  // 仅在唤醒时即刻触发ADC，不等待、不读取
  // deep sleep场景: YAML中已配置 touch→指纹→5s延迟→trigger_update
  // 5s后CW2017的ADC和SOC算法已充分稳定，单次读取即可成功
  ESP_LOGI(TAG, "CW2017 wake sequence done, ADC starting (data available in ~1.5s)");
  // 注意: 不调用update()，不阻塞ESP32启动流程
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

  ESP_LOGI(TAG, "Writing battery profile to registers 0x%02X-0x%02X (size=%u)",
           CW2017_REG_BATINFO, CW2017_REG_BATINFO + len - 1, len);

  // 逐字节写入电池配置信息（寄存器 0x10 ~ 0x5F）
  for (size_t i = 0; i < len; i++) {
    if (!this->write_register(CW2017_REG_BATINFO + i, profile[i])) {
      ESP_LOGE(TAG, "Failed to write profile byte %u at register 0x%02X",
               i, CW2017_REG_BATINFO + i);
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

  this->publish_state(state);
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
