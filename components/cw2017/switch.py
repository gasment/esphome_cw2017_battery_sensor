import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import (
    CONF_ID,
    ICON_POWER,
)

from . import CW2017Component

cw2017_ns = cg.esphome_ns.namespace("cw2017")
CW2017SleepSwitch = cw2017_ns.class_(
    "CW2017SleepSwitch", switch.Switch, cg.Component
)

CONF_CW2017_ID = "cw2017_id"

CONFIG_SCHEMA = switch.switch_schema(
    CW2017SleepSwitch,
    icon=ICON_POWER,
).extend(
    {
        cv.GenerateID(CONF_CW2017_ID): cv.use_id(CW2017Component),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    hub = await cg.get_variable(config[CONF_CW2017_ID])
    cg.add(var.set_parent(hub))
