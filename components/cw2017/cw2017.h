#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"

#include <vector>

namespace esphome {
namespace cw2017 {

/// CW2017 默认 I2C 地址 (7位)
static constexpr uint8_t CW2017_I2C_ADDR = 0x63;

/// 电池配置文件大小 (80字节)
static constexpr size_t CW2017_PROFILE_SIZE = 80;

/// CW2017 寄存器地址定义 (参考 CW2017BAAD Datasheet V1.1 Table 2)
enum CW2017Register : uint8_t {
  CW2017_REG_VERSION = 0x00,    ///< IC版本号 (默认值: 0xA0)
  CW2017_REG_VCELL_H = 0x02,    ///< 电池电压高字节 [13:8]
  CW2017_REG_VCELL_L = 0x03,    ///< 电池电压低字节 [7:0]
  CW2017_REG_SOC_H = 0x04,      ///< SOC高字节 [15:8] (整数百分比 0~100)
  CW2017_REG_SOC_L = 0x05,      ///< SOC低字节 [7:0] (小数部分 LSB=1/256%)
  CW2017_REG_TEMP = 0x06,       ///< 电池温度 (温度°C = -40 + value/2, LSB=0.5°C)
  CW2017_REG_CONFIG = 0x08,     ///< IC配置寄存器 (默认值: 0xF0)
  CW2017_REG_INT_CONF = 0x0A,   ///< 中断配置/模式配置寄存器 (默认值: 0x40)
  CW2017_REG_SOC_ALERT = 0x0B,  ///< SOC报警阈值寄存器 (默认值: 0x14)
  CW2017_REG_TEMP_MAX = 0x0C,   ///< 最高温度阈值 (默认值: 0xAA)
  CW2017_REG_TEMP_MIN = 0x0D,   ///< 最低温度阈值 (默认值: 0x50)
  CW2017_REG_VOLT_ID_H = 0x0E,  ///< ID引脚电压高字节 [13:8]
  CW2017_REG_VOLT_ID_L = 0x0F,  ///< ID引脚电压低字节 [7:0]
  CW2017_REG_BATINFO = 0x10,    ///< 电池配置信息起始寄存器 (80字节)
  CW2017_REG_T_HOST_H = 0xA0,   ///< 主机报告温度高字节 (仅CW2017BAAD支持)
  CW2017_REG_T_HOST_L = 0xA1,   ///< 主机报告温度低字节 (仅CW2017BAAD支持)
};

/// CONFIG 寄存器 (0x08) 控制位
enum CW2017ConfigBits : uint8_t {
  CW2017_CONFIG_SLEEP_MASK = 0x30,    ///< SLEEP[1:0] 睡眠模式位
  CW2017_CONFIG_RESTART_MASK = 0x0F,  ///< RESTART[3:0] 重启位
  CW2017_CONFIG_CLEAR_SLEEP = 0x30,   ///< 写0x30清除睡眠模式
  CW2017_CONFIG_RESTART = 0x00,       ///< 写0x00触发重启进入正常模式
};

/// CW2017 工作模式
enum CW2017Mode : uint8_t {
  CW2017_MODE_SLEEP = 0x00,    ///< 睡眠模式
  CW2017_MODE_NORMAL = 0x01,   ///< 正常模式
};

/// 4.2V 锂电池默认配置文件 (80字节)
static const uint8_t CW2017_DEFAULT_PROFILE_4_2V[80] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB7, 0xC2, 0xB6, 0xA9, 0x9E, 0x99, 0xED, 0xDF,
    0xD9, 0xCB, 0xC1, 0xAD, 0x9D, 0x80, 0x66, 0x55,
    0x4B, 0x4B, 0x4B, 0x85, 0x7F, 0xD3, 0x73, 0xFF,
    0xF9, 0x5C, 0x63, 0x81, 0xBD, 0xEC, 0xE0, 0xD0,
    0xC7, 0xD7, 0xD6, 0xD9, 0xE5, 0xDF, 0xDA, 0xD6,
    0xCD, 0xCF, 0xD3, 0xD3, 0xD5, 0xF4, 0xFF, 0x43,
    0x00, 0x00, 0xAB, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38,
};

// 前置声明
class CW2017Component;

/**
 * @brief CW2017 休眠模式开关
 *
 * ON  → 进入休眠 (调用 parent_->sleep())
 * OFF → 唤醒正常 (调用 parent_->wake())
 */
class CW2017SleepSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(CW2017Component *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;

  CW2017Component *parent_{nullptr};
};

/**
 * @brief CW2017 手动触发读取按钮
 *
 * 按下后立即触发一次传感器读取 (调用 parent_->trigger_read())
 */
class CW2017TriggerButton : public button::Button, public Component {
 public:
  void set_parent(CW2017Component *parent) { this->parent_ = parent; }

 protected:
  void press_action() override;

  CW2017Component *parent_{nullptr};
};

/**
 * @brief CW2017 电池电量计组件
 *
 * 支持读取电池电压、SOC（电量百分比）和温度。
 * 兼容 CW2017BAAD 型号，支持主机报告温度功能。
 */
class CW2017Component : public PollingComponent, public i2c::I2CDevice {
 public:
  CW2017Component() = default;

  /// ========== ESPHome 生命周期 ==========

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override;

  /// ========== 传感器设置接口 ==========

  void set_voltage_sensor(sensor::Sensor *voltage_sensor) {
    this->voltage_sensor_ = voltage_sensor;
  }
  void set_soc_sensor(sensor::Sensor *soc_sensor) {
    this->soc_sensor_ = soc_sensor;
  }
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) {
    this->temperature_sensor_ = temperature_sensor;
  }

  /// ========== 配置接口 ==========

  /// 设置延迟初始化毫秒数（冷启动/deep sleep唤醒时延迟初始化，加快其他组件启动速度）
  void set_delay_init_ms(uint32_t delay_ms) {
    this->delay_init_ms_ = delay_ms;
  }

  void set_battery_profile(const std::vector<uint8_t> &profile) {
    this->battery_profile_ = profile;
  }
  void set_write_profile(bool write_profile) {
    this->write_profile_ = write_profile;
  }

  /// ========== 控制接口 (由 switch/button 调用) ==========

  /** @brief 进入休眠模式 (写 0x7F 到 CONFIG) */
  void sleep();

  /** @brief 唤醒设备进入正常模式并自动读取传感器 */
  void wake();

  /** @brief 手动触发一次传感器数据读取 */
  void trigger_read();

  /// ========== Switch/Button 设置接口 ==========

  void set_sleep_switch(switch_::Switch *sw) {
    this->sleep_switch_ = sw;
  }

  void set_trigger_button(button::Button *btn) {
    this->trigger_button_ = btn;
  }

 protected:
  /// ========== 传感器对象 ==========
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *soc_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};

  /// ========== Switch/Button 对象 ==========
  switch_::Switch *sleep_switch_{nullptr};
  button::Button *trigger_button_{nullptr};

  /// ========== 配置参数 ==========
  std::vector<uint8_t> battery_profile_;
  bool write_profile_{true};
  uint32_t delay_init_ms_{0};  ///< 冷启动/deep sleep唤醒时延迟初始化的毫秒数

  /// ========== 状态变量 ==========
  bool initialized_{false};
  uint16_t last_soc_{0};

  /// ========== 底层 I2C 操作 ==========

  bool read_register(uint8_t reg, uint8_t *data);
  bool write_register(uint8_t reg, uint8_t data);
  bool read_registers(uint8_t start_reg, uint8_t *data, size_t len);

  /// ========== 延迟初始化 ==========
  /** 延迟初始化的真实执行方法（被setup或set_timeout调用） */
  void perform_real_setup_();

  /// ========== SOC延迟就绪重试 ==========
  /** 初始化后SOC可能为0xFF（算法需额外时间），定时重试直到就绪 */
  void schedule_soc_retry_(int retry_count);

  /// ========== 芯片操作 ==========

  bool wake_device();
  bool reset_device();
  bool read_version(uint8_t *version);
  bool write_battery_profile_data(const uint8_t *profile, size_t len);

  /// ========== 传感器读取 ==========

  bool read_voltage(float *voltage);
  bool read_soc(float *soc);
  bool read_temperature(float *temperature);
};

}  // namespace cw2017
}  // namespace esphome
