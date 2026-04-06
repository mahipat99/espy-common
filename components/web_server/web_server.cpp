#include "web_server.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/util.h"

namespace esphome {
namespace web_server_custom {

static const char *TAG = "web_server_custom";

// ---------------------------------------------------------------------------
// Helper: stable ID from entity name (lowercase, spaces → underscores)
// Avoids deprecated get_object_id() which is removed in 2026.7
// ---------------------------------------------------------------------------
std::string WebServerCustom::make_id(const std::string &name) {
  std::string id = name;
  for (char &c : id) {
    if (c == ' ' || c == '-') c = '_';
    else c = tolower((unsigned char)c);
  }
  return id;
}

WebServerCustom::WebServerCustom() {}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void WebServerCustom::setup() {
  ESP_LOGCONFIG(TAG, "Setting up custom web server on port %u", port_);

  server_ = new AsyncWebServer(port_);
  events_ = new AsyncEventSource("/events");

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

  // ---- SPA ----------------------------------------------------------------
  server_->on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
    handle_index(req);
  });

  // ---- REST API -----------------------------------------------------------
  server_->on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    handle_state(req);
  });

  server_->on("/api/switch", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/switch/");
    if (s < 0) { req->send(400); return; }
    String id = path.substring(s + 12);
    int slash = id.indexOf('/'); if (slash >= 0) id = id.substring(0, slash);
    handle_switch_toggle(req, id);
  });

  server_->on("/api/light", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/light/");
    if (s < 0) { req->send(400); return; }
    handle_light_set(req, path.substring(s + 11));
  });

  server_->on("/api/fan", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/fan/");
    if (s < 0) { req->send(400); return; }
    handle_fan_set(req, path.substring(s + 9));
  });

  server_->on("/api/number", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/number/");
    if (s < 0) { req->send(400); return; }
    handle_number_set(req, path.substring(s + 12));
  });

  server_->on("/api/select", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/select/");
    if (s < 0) { req->send(400); return; }
    handle_select_set(req, path.substring(s + 12));
  });

  server_->on("/api/climate", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/climate/");
    if (s < 0) { req->send(400); return; }
    handle_climate_set(req, path.substring(s + 13));
  });

  server_->on("/api/button", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/button/");
    if (s < 0) { req->send(400); return; }
    handle_button_press(req, path.substring(s + 12));
  });

  server_->on("/", HTTP_OPTIONS, [](AsyncWebServerRequest *req) { req->send(204); });

  // ---- SSE ----------------------------------------------------------------
  events_->onConnect([this](AsyncEventSourceClient *client) {
    ESP_LOGD(TAG, "SSE client connected");
    send_full_state(client);
  });
  server_->addHandler(events_);

  // ---- 404: serve SPA for client-side routing ----------------------------
  server_->onNotFound([this](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_GET) handle_index(req);
    else req->send(404, "application/json", "{\"error\":\"not found\"}");
  });

  // ---- Entity state callbacks → SSE push ---------------------------------
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    sw->add_on_state_callback([this, sw](bool) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_switch_json(obj, sw);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    light->add_target_state_reached_listener([this, light]() {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_light_json(obj, light);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_SENSOR
  for (auto *sensor : App.get_sensors()) {
    sensor->add_on_state_callback([this, sensor](float) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_sensor_json(obj, sensor);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_BINARY_SENSOR
  for (auto *bs : App.get_binary_sensors()) {
    bs->add_on_state_callback([this, bs](bool) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_binary_sensor_json(obj, bs);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_TEXT_SENSOR
  for (auto *ts : App.get_text_sensors()) {
    ts->add_on_state_callback([this, ts](const std::string &) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_text_sensor_json(obj, ts);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

  server_->begin();
  ESP_LOGCONFIG(TAG, "Web server started on port %u", port_);
}

void WebServerCustom::loop() {}

void WebServerCustom::dump_config() {
  ESP_LOGCONFIG(TAG, "Custom Web Server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Auth: %s", auth_user_.empty() ? "disabled" : "enabled");
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
bool WebServerCustom::check_auth(AsyncWebServerRequest *request) {
  if (auth_user_.empty()) return true;
  if (!request->authenticate(auth_user_.c_str(), auth_pass_.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Serve embedded SPA
// ---------------------------------------------------------------------------
void WebServerCustom::handle_index(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginResponse_P(
      200, "text/html", WEB_UI_GZ, WEB_UI_GZ_LEN);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

// ---------------------------------------------------------------------------
// Full state snapshot (REST + SSE initial push)
// ---------------------------------------------------------------------------
void WebServerCustom::handle_state(AsyncWebServerRequest *request) {
  JsonDocument doc;
  build_all_entities_json(doc);
  String output; serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WebServerCustom::send_full_state(AsyncEventSourceClient *client) {
  JsonDocument doc;
  build_all_entities_json(doc);
  String payload; serializeJson(doc, payload);
  client->send(payload.c_str(), "full_state", millis());
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------
void WebServerCustom::handle_switch_toggle(AsyncWebServerRequest *request,
                                            const String &entity_id) {
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (make_id(sw->get_name()) == entity_id.c_str()) {
      sw->toggle();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_light_set(AsyncWebServerRequest *request,
                                        const String &entity_id) {
#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    if (make_id(light->get_name()) == entity_id.c_str()) {
      auto call = light->make_call();
      if (request->hasParam("state", true)) {
        String v = request->getParam("state", true)->value();
        call.set_state(v == "on" || v == "1" || v == "true");
      }
      if (request->hasParam("brightness", true))
        call.set_brightness(request->getParam("brightness", true)->value().toFloat() / 255.0f);
      if (request->hasParam("color_temp", true))
        call.set_color_temperature(request->getParam("color_temp", true)->value().toFloat());
      if (request->hasParam("r", true) && request->hasParam("g", true) && request->hasParam("b", true))
        call.set_rgb(request->getParam("r", true)->value().toFloat() / 255.0f,
                     request->getParam("g", true)->value().toFloat() / 255.0f,
                     request->getParam("b", true)->value().toFloat() / 255.0f);
      if (request->hasParam("effect", true))
        call.set_effect(request->getParam("effect", true)->value().c_str());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_fan_set(AsyncWebServerRequest *request,
                                      const String &entity_id) {
#ifdef USE_FAN
  for (auto *fan : App.get_fans()) {
    if (make_id(fan->get_name()) == entity_id.c_str()) {
      auto call = fan->make_call();
      if (request->hasParam("state", true)) {
        String v = request->getParam("state", true)->value();
        call.set_state(v == "on" || v == "1" || v == "true");
      }
      if (request->hasParam("speed", true))
        call.set_speed(request->getParam("speed", true)->value().toInt());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_number_set(AsyncWebServerRequest *request,
                                         const String &entity_id) {
#ifdef USE_NUMBER
  for (auto *number : App.get_numbers()) {
    if (make_id(number->get_name()) == entity_id.c_str()) {
      if (request->hasParam("value", true)) {
        auto call = number->make_call();
        call.set_value(request->getParam("value", true)->value().toFloat());
        call.perform();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_select_set(AsyncWebServerRequest *request,
                                         const String &entity_id) {
#ifdef USE_SELECT
  for (auto *select : App.get_selects()) {
    if (make_id(select->get_name()) == entity_id.c_str()) {
      if (request->hasParam("option", true)) {
        auto call = select->make_call();
        call.set_option(request->getParam("option", true)->value().c_str());
        call.perform();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_climate_set(AsyncWebServerRequest *request,
                                          const String &entity_id) {
#ifdef USE_CLIMATE
  for (auto *climate : App.get_climates()) {
    if (make_id(climate->get_name()) == entity_id.c_str()) {
      auto call = climate->make_call();
      if (request->hasParam("mode", true))
        call.set_mode(request->getParam("mode", true)->value().c_str());
      if (request->hasParam("target_temperature", true))
        call.set_target_temperature(
            request->getParam("target_temperature", true)->value().toFloat());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_button_press(AsyncWebServerRequest *request,
                                           const String &entity_id) {
#ifdef USE_BUTTON
  for (auto *button : App.get_buttons()) {
    if (make_id(button->get_name()) == entity_id.c_str()) {
      button->press();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------------------
// JSON builders  (ArduinoJson v7: no createNestedArray/createNestedObject)
// ---------------------------------------------------------------------------
void WebServerCustom::build_all_entities_json(JsonDocument &doc) {
  JsonObject root = doc.to<JsonObject>();
  root["device_name"]    = App.get_name().c_str();
  root["friendly_name"]  = App.get_friendly_name().c_str();
  root["esphome_version"]= ESPHOME_VERSION;
  root["uptime"]         = millis() / 1000;

#ifdef USE_SWITCH
  JsonArray switches = root["switches"].to<JsonArray>();
  for (auto *sw : App.get_switches()) {
    build_switch_json(switches.add<JsonObject>(), sw);
  }
#endif
#ifdef USE_LIGHT
  JsonArray lights = root["lights"].to<JsonArray>();
  for (auto *light : App.get_lights()) {
    build_light_json(lights.add<JsonObject>(), light);
  }
#endif
#ifdef USE_SENSOR
  JsonArray sensors = root["sensors"].to<JsonArray>();
  for (auto *sensor : App.get_sensors()) {
    build_sensor_json(sensors.add<JsonObject>(), sensor);
  }
#endif
#ifdef USE_BINARY_SENSOR
  JsonArray binary_sensors = root["binary_sensors"].to<JsonArray>();
  for (auto *bs : App.get_binary_sensors()) {
    build_binary_sensor_json(binary_sensors.add<JsonObject>(), bs);
  }
#endif
#ifdef USE_TEXT_SENSOR
  JsonArray text_sensors = root["text_sensors"].to<JsonArray>();
  for (auto *ts : App.get_text_sensors()) {
    build_text_sensor_json(text_sensors.add<JsonObject>(), ts);
  }
#endif
#ifdef USE_CLIMATE
  JsonArray climates = root["climates"].to<JsonArray>();
  for (auto *climate : App.get_climates()) {
    build_climate_json(climates.add<JsonObject>(), climate);
  }
#endif
#ifdef USE_FAN
  JsonArray fans = root["fans"].to<JsonArray>();
  for (auto *fan : App.get_fans()) {
    build_fan_json(fans.add<JsonObject>(), fan);
  }
#endif
#ifdef USE_NUMBER
  JsonArray numbers = root["numbers"].to<JsonArray>();
  for (auto *number : App.get_numbers()) {
    build_number_json(numbers.add<JsonObject>(), number);
  }
#endif
#ifdef USE_SELECT
  JsonArray selects = root["selects"].to<JsonArray>();
  for (auto *select : App.get_selects()) {
    build_select_json(selects.add<JsonObject>(), select);
  }
#endif
#ifdef USE_BUTTON
  JsonArray buttons = root["buttons"].to<JsonArray>();
  for (auto *button : App.get_buttons()) {
    JsonObject obj = buttons.add<JsonObject>();
    obj["id"]   = make_id(button->get_name()).c_str();
    obj["name"] = button->get_name().c_str();
    obj["type"] = "button";
  }
#endif
}

#ifdef USE_SWITCH
void WebServerCustom::build_switch_json(JsonObject obj, switch_::Switch *sw) {
  obj["id"]    = make_id(sw->get_name()).c_str();
  obj["name"]  = sw->get_name().c_str();
  obj["type"]  = "switch";
  obj["state"] = sw->state;
}
#endif

#ifdef USE_LIGHT
void WebServerCustom::build_light_json(JsonObject obj, light::LightState *light) {
  obj["id"]   = make_id(light->get_name()).c_str();
  obj["name"] = light->get_name().c_str();
  obj["type"] = "light";
  auto values = light->current_values;
  obj["state"]      = values.is_on();
  obj["brightness"] = (int)(values.get_brightness() * 255);

  auto traits = light->get_traits();

  // Color temperature
  if (traits.supports_color_mode(light::ColorMode::COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::COLD_WARM_WHITE)) {
    obj["color_temp"] = values.get_color_temperature();
    obj["min_mireds"] = traits.get_min_mireds();
    obj["max_mireds"] = traits.get_max_mireds();
  }

  // RGB
  if (traits.supports_color_mode(light::ColorMode::RGB) ||
      traits.supports_color_mode(light::ColorMode::RGB_WHITE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLD_WARM_WHITE)) {
    obj["r"] = (int)(values.get_red()   * 255);
    obj["g"] = (int)(values.get_green() * 255);
    obj["b"] = (int)(values.get_blue()  * 255);
  }

  JsonArray effects = obj["effects"].to<JsonArray>();
  for (auto &effect : light->get_effects()) {
    effects.add(effect->get_name().c_str());
  }
  obj["effect"] = light->get_effect_name().c_str();
}
#endif

#ifdef USE_SENSOR
void WebServerCustom::build_sensor_json(JsonObject obj, sensor::Sensor *sensor) {
  obj["id"]           = make_id(sensor->get_name()).c_str();
  obj["name"]         = sensor->get_name().c_str();
  obj["type"]         = "sensor";
  obj["unit"]         = sensor->get_unit_of_measurement().c_str();
  obj["device_class"] = sensor->get_device_class().c_str();
  if (sensor->has_state()) obj["state"] = sensor->get_state();
  else obj["state"] = nullptr;
}
#endif

#ifdef USE_BINARY_SENSOR
void WebServerCustom::build_binary_sensor_json(JsonObject obj,
                                                binary_sensor::BinarySensor *bs) {
  obj["id"]           = make_id(bs->get_name()).c_str();
  obj["name"]         = bs->get_name().c_str();
  obj["type"]         = "binary_sensor";
  obj["device_class"] = bs->get_device_class().c_str();
  obj["state"]        = bs->state;
}
#endif

#ifdef USE_TEXT_SENSOR
void WebServerCustom::build_text_sensor_json(JsonObject obj,
                                              text_sensor::TextSensor *ts) {
  obj["id"]           = make_id(ts->get_name()).c_str();
  obj["name"]         = ts->get_name().c_str();
  obj["type"]         = "text_sensor";
  obj["device_class"] = ts->get_device_class().c_str();
  obj["state"]        = ts->get_state().c_str();
}
#endif

#ifdef USE_CLIMATE
void WebServerCustom::build_climate_json(JsonObject obj, climate::Climate *climate) {
  obj["id"]                  = make_id(climate->get_name()).c_str();
  obj["name"]                = climate->get_name().c_str();
  obj["type"]                = "climate";
  obj["mode"]                = climate::climate_mode_to_string(climate->mode);
  obj["current_temperature"] = climate->current_temperature;
  obj["target_temperature"]  = climate->target_temperature;
  obj["action"]              = climate::climate_action_to_string(climate->action);
}
#endif

#ifdef USE_FAN
void WebServerCustom::build_fan_json(JsonObject obj, fan::Fan *fan) {
  obj["id"]    = make_id(fan->get_name()).c_str();
  obj["name"]  = fan->get_name().c_str();
  obj["type"]  = "fan";
  obj["state"] = fan->state;
  if (fan->get_traits().supports_speed()) {
    obj["speed"]       = fan->speed;
    obj["speed_count"] = fan->get_traits().supported_speed_count();
  }
  obj["oscillating"] = fan->oscillating;
}
#endif

#ifdef USE_NUMBER
void WebServerCustom::build_number_json(JsonObject obj, number::Number *number) {
  obj["id"]   = make_id(number->get_name()).c_str();
  obj["name"] = number->get_name().c_str();
  obj["type"] = "number";
  obj["min"]  = number->traits.get_min_value();
  obj["max"]  = number->traits.get_max_value();
  obj["step"] = number->traits.get_step();
  obj["unit"] = number->traits.get_unit_of_measurement().c_str();
  if (number->has_state()) obj["state"] = number->state;
  else obj["state"] = nullptr;
}
#endif

#ifdef USE_SELECT
void WebServerCustom::build_select_json(JsonObject obj, select::Select *select) {
  obj["id"]   = make_id(select->get_name()).c_str();
  obj["name"] = select->get_name().c_str();
  obj["type"] = "select";
  JsonArray options = obj["options"].to<JsonArray>();
  for (auto &opt : select->traits.get_options()) options.add(opt.c_str());
  obj["state"] = select->state.c_str();
}
#endif

}  // namespace web_server_custom
}  // namespace esphome
