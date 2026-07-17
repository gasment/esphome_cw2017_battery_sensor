import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
    UNIT_CELSIUS,
    UNIT_VOLT,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["switch", "button"]

cw2017_ns = cg.esphome_ns.namespace("cw2017")
CW2017Component = cw2017_ns.class_(
    "CW2017Component", cg.PollingComponent, i2c.I2CDevice
)

CONF_DELAY_INIT_ON_BOOT = "delay_init_on_boot"
CONF_BATTERY_PROFILE = "battery_profile"
CONF_WRITE_PROFILE = "write_profile"
CONF_VOLTAGE = "voltage"
CONF_SOC = "soc"
CONF_TEMPERATURE = "temperature"
CONF_SLEEP_SWITCH = "sleep_switch"
CONF_TRIGGER_BUTTON = "trigger_read"


def validate_battery_profile(value):
    """验证电池配置文件是否为80字节"""
    if len(value) != 80:
        raise cv.Invalid(f"Battery profile must be exactly 80 bytes, got {len(value)}")
    return value


BATTERY_PROFILE_SCHEMA = cv.All(
    cv.ensure_list(cv.hex_uint8_t),
    validate_battery_profile,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CW2017Component),
            cv.Optional(CONF_DELAY_INIT_ON_BOOT, default="0ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BATTERY_PROFILE): BATTERY_PROFILE_SCHEMA,
            cv.Optional(CONF_WRITE_PROFILE, default=True): cv.boolean,
            cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_SOC): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_BATTERY,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x63))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    # 延迟初始化配置（毫秒）
    delay_ms = config[CONF_DELAY_INIT_ON_BOOT]
    cg.add(var.set_delay_init_ms(delay_ms))

    if CONF_BATTERY_PROFILE in config:
        profile = config[CONF_BATTERY_PROFILE]
        cg.add(var.set_battery_profile(profile))

    cg.add(var.set_write_profile(config[CONF_WRITE_PROFILE]))

    if voltage_config := config.get(CONF_VOLTAGE):
        sens = await sensor.new_sensor(voltage_config)
        cg.add(var.set_voltage_sensor(sens))

    if soc_config := config.get(CONF_SOC):
        sens = await sensor.new_sensor(soc_config)
        cg.add(var.set_soc_sensor(sens))

    if temp_config := config.get(CONF_TEMPERATURE):
        sens = await sensor.new_sensor(temp_config)
        cg.add(var.set_temperature_sensor(sens))
