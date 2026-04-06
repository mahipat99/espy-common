# Stub OTA platform for the custom web_server external component.
#
# ESPHome 2026.x tries to load web_server.ota as a platform during OTA
# component resolution, even when no `platform: web_server` is configured.
# This stub satisfies that lookup silently.
#
# Real OTA: use `platform: esphome` in your yaml.

import esphome.config_validation as cv
import esphome.codegen as cg

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass

