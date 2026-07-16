import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import (
    CONF_ID,
    ICON_RESTART,
)

from . import CW2017Component

cw2017_ns = cg.esphome_ns.namespace("cw2017")
CW2017TriggerButton = cw2017_ns.class_(
    "CW2017TriggerButton", button.Button, cg.Component
)

CONF_CW2017_ID = "cw2017_id"

CONFIG_SCHEMA = button.button_schema(
    CW2017TriggerButton,
    icon=ICON_RESTART,
).extend(
    {
        cv.GenerateID(CONF_CW2017_ID): cv.use_id(CW2017Component),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    hub = await cg.get_variable(config[CONF_CW2017_ID])
    cg.add(var.set_parent(hub))
