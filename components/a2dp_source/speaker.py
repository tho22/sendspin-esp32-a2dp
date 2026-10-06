"""Speaker platform that streams audio to a Bluetooth Classic (A2DP) sink, e.g. a Bluetooth speaker."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_BUFFER_DURATION,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)
from esphome.types import ConfigType

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["esp32"]

CONF_DEVICE_NAME = "device_name"
CONF_KEEP_ALIVE = "keep_alive"
CONF_TEST_TONE = "test_tone"
CONF_ON_PLAY_PAUSE = "on_play_pause"
CONF_ON_STOP = "on_stop"
CONF_ON_NEXT = "on_next"
CONF_ON_PREVIOUS = "on_previous"
CONF_ON_VOLUME = "on_volume"
CONF_LOCAL_NAME = "local_name"
CONF_PIN_CODE = "pin_code"
CONF_RECONNECT_INTERVAL = "reconnect_interval"

# The legacy A2DP source data path always encodes SBC at 44.1 kHz, 16 bit, stereo.
A2DP_SAMPLE_RATE = 44100
A2DP_CHANNELS = 2
A2DP_BITS_PER_SAMPLE = 16

a2dp_source_ns = cg.esphome_ns.namespace("a2dp_source")
A2DPSourceSpeaker = a2dp_source_ns.class_(
    "A2DPSourceSpeaker", cg.Component, speaker.Speaker
)


def _set_stream_limits(config: ConfigType) -> ConfigType:
    audio.set_stream_limits(
        min_bits_per_sample=A2DP_BITS_PER_SAMPLE,
        max_bits_per_sample=A2DP_BITS_PER_SAMPLE,
        min_channels=A2DP_CHANNELS,
        max_channels=A2DP_CHANNELS,
        min_sample_rate=A2DP_SAMPLE_RATE,
        max_sample_rate=A2DP_SAMPLE_RATE,
    )(config)
    return config


def _validate_variant(config: ConfigType) -> ConfigType:
    variant = esp32.get_esp32_variant()
    if variant != esp32.const.VARIANT_ESP32:
        raise cv.Invalid(
            f"A2DP requires Bluetooth Classic, which is only available on the original ESP32 (not {variant})"
        )
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(A2DPSourceSpeaker),
            cv.Optional(CONF_ADDRESS): cv.mac_address,
            cv.Optional(CONF_DEVICE_NAME): cv.string_strict,
            cv.Optional(CONF_LOCAL_NAME, default="ESPHome A2DP"): cv.All(
                cv.string_strict, cv.Length(max=32)
            ),
            cv.Optional(CONF_PIN_CODE, default="0000"): cv.All(
                cv.string_strict, cv.Length(min=4, max=16)
            ),
            cv.Optional(
                CONF_BUFFER_DURATION, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_RECONNECT_INTERVAL, default="15s"
            ): cv.positive_time_period_milliseconds,
            # Stream silence while idle so sinks that power off without audio stay on
            cv.Optional(CONF_KEEP_ALIVE, default=True): cv.boolean,
            # Diagnostic: play a 441 Hz tone instead of idle silence (needs keep_alive)
            cv.Optional(CONF_TEST_TONE, default=False): cv.boolean,
            # Speaker buttons (AVRCP)
            cv.Optional(CONF_ON_PLAY_PAUSE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_STOP): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_NEXT): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_PREVIOUS): automation.validate_automation(single=True),
            # Volume changed on the speaker, x = 0..1
            cv.Optional(CONF_ON_VOLUME): automation.validate_automation(single=True),
            cv.Optional(CONF_SAMPLE_RATE, default=A2DP_SAMPLE_RATE): cv.one_of(
                A2DP_SAMPLE_RATE, int=True
            ),
            cv.Optional(CONF_NUM_CHANNELS, default=A2DP_CHANNELS): cv.one_of(
                A2DP_CHANNELS, int=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.has_at_least_one_key(CONF_ADDRESS, CONF_DEVICE_NAME),
    cv.only_on_esp32,
    _validate_variant,
    _set_stream_limits,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    if address := config.get(CONF_ADDRESS):
        cg.add(var.set_address(address.as_hex))
    if device_name := config.get(CONF_DEVICE_NAME):
        cg.add(var.set_device_name(device_name))
    cg.add(var.set_local_name(config[CONF_LOCAL_NAME]))
    cg.add(var.set_pin_code(config[CONF_PIN_CODE]))
    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))
    cg.add(var.set_keep_alive(config[CONF_KEEP_ALIVE]))
    cg.add(var.set_test_tone(config[CONF_TEST_TONE]))

    for key, getter in (
        (CONF_ON_PLAY_PAUSE, var.get_play_pause_trigger),
        (CONF_ON_STOP, var.get_stop_trigger),
        (CONF_ON_NEXT, var.get_next_trigger),
        (CONF_ON_PREVIOUS, var.get_previous_trigger),
    ):
        if conf := config.get(key):
            await automation.build_automation(getter(), [], conf)
    if conf := config.get(CONF_ON_VOLUME):
        await automation.build_automation(var.get_volume_trigger(), [(cg.float_, "x")], conf)

    # Bluetooth Classic with Bluedroid and the A2DP source profile
    esp32.request_bluetooth()
    esp32.request_software_coexistence()
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_A2DP_ENABLE", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_SSP_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    # BR/EDR only controller: frees the BLE controller memory
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
    # Keep internal RAM free for WiFi + Bluetooth by moving Bluedroid allocations to PSRAM when available
    esp32.add_idf_sdkconfig_option("CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST", True)
