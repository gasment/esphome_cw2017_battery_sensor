# CW2017 锂电池计量芯片 ESPHome 外部组件

[![ESPHome](https://img.shields.io/badge/ESPHome-2026.6+-blue.svg)](https://esphome.io)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

## 功能实现：
| 功能 | 状态 | 说明 |
|---------|------|-----------|
| I2C通信 | ✅ | 使用ESPHome内置I2C |
| 电池电压读取 | ✅ | 3位小数准确度|
| 电池电量（SOC）读取 |✅ | --|
| 默认4.2V电池配置 | ✅ | --|
| 休眠模式 | ✅ | -- |
| 温度读取 | ❓ | 未测试 |
| ID引脚电压读取 | ❌ | 检测电池类型 |
| 中断配置 | ❌ | SOC报警、温度阈值 |
| SOC报警阈值 | ❌ | 低电量告警 |
| 温度阈值 | ❌ | 温度保护 |

## 配置说明
- 组件引用：
  ```
  external_components:
    - source:
        type: git
        url: https://github.com/gasment/esphome_cw2017_battery_sensor
        ref: main
      components: [ cw2017 ]
  ```
- i2c配置：
  ```
  i2c:
    sda: GPIOx  #任意可用gpio
    scl: GPIOx  #任意可用gpio
    scan: true         
    id: i2c_main_bus
  ```
- cw2017配置：
  ```
  cw2017:
    id: cw2017_main
    address: 0x63
    write_profile: true  #写入4.2v默认配置
    delay_init_on_boot: 5s  #开机时的初始化延迟时间
    update_interval: 60s  #更新频率
  
    voltage:  #电池电压
      id: battery_voltage
      accuracy_decimals: 3
      filters:
        - filter_out: nan
  
    soc: #电池电量
      id: battery_soc
      filters:
        - filter_out: nan
  
  
    temperature: #电池温度
      id: battery_temperature
  ```
 - switch配置：
  ```
  switch:    #睡眠模式开关（打开时进入，关闭时退出）
    - platform: cw2017
      cw2017_id: cw2017_main  #对应上方的id
      id: cw2017_sleep_mode_sw
      restore_mode: ALWAYS_OFF
  ```
 - button配置：
  ```
  button:  #手动触发cw2017组件更新
    - platform: cw2017
      cw2017_id: cw2017_main
      id: cw2017_trigger_update
  ```
