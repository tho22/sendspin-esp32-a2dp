"""Select that offers the speakers found by a search; choosing one pairs it."""

import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from .speaker import A2DPSourceSpeaker, a2dp_source_ns

CONF_A2DP_SOURCE_ID = "a2dp_source_id"

A2DPDeviceSelect = a2dp_source_ns.class_(
    "A2DPDeviceSelect", select.Select, cg.Parented.template(A2DPSourceSpeaker)
)

CONFIG_SCHEMA = select.select_schema(
    A2DPDeviceSelect,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon="mdi:speaker-bluetooth",
).extend(
    {
        cv.GenerateID(CONF_A2DP_SOURCE_ID): cv.use_id(A2DPSourceSpeaker),
    }
)


async def to_code(config):
    # Placeholder until the first search; the speaker replaces the options at runtime
    var = await select.new_select(config, options=["Search for speakers first"])
    await cg.register_parented(var, config[CONF_A2DP_SOURCE_ID])
    parent = await cg.get_variable(config[CONF_A2DP_SOURCE_ID])
    cg.add(parent.set_device_select(var))
