// web_server.cpp — shared logic: JSON builders, entity callbacks, setup dispatch
#include "web_server.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace web_server_custom {

static const char *TAG = "web_server_custom";

// ---------------------------------------------------------------------------
// Forward declarations for platform backends
// ---------------------------------------------------------------------------
#ifdef USE_ARDUINO
IWebServer *make_arduino_server(WebServerCustom *parent, uint16_t port);
#endif
#if defined(USE_ESP_IDF) || (!defined(USE_ARDUINO) && defined(ESP32))
IWebServer *make_idf_server(WebServerCustom *parent, uint16_t port);
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string WebServerCustom::make_id(const std::string &name) {
  std::string id = name;
  for (char &c : id) {
    if (c == ' ' || c == '-') c = '_';
    else c = tolower((unsigned char) c);
  }
  return id;
}

std::string WebServerCustom::safe_device_class(EntityBase *e) {
  char buf[48] = {};
  e->get_device_class_to(buf);
  return std::string(buf);
}

#ifdef USE_LIGHT
class LightChangeListener : public light::LightTargetStateReachedListener {
 public:
  explicit LightChangeListener(std::function<void()> cb) : cb_(std::move(cb)) {}
  void on_light_target_state_reached() override { cb_(); }
 private:
  std::function<void()> cb_;
};
#endif

// ---------------------------------------------------------------------------
// broadcast_state — called by entity callbacks, delegates to backend SSE
// ---------------------------------------------------------------------------
void WebServerCustom::broadcast_state(const std::string &json_str) {
  if (backend_) backend_->send_event(json_str.c_str(), "state_change");
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
WebServerCustom::WebServerCustom() {}

void WebServerCustom::setup() {
  ESP_LOGCONFIG(TAG, "Setting up custom web server on port %u", port_);

#ifdef USE_ARDUINO
  backend_.reset(make_arduino_server(this, port_));
#elif defined(USE_ESP_IDF) || (!defined(USE_ARDUINO) && defined(ESP32))
  backend_.reset(make_idf_server(this, port_));
#else
  ESP_LOGE(TAG, "No supported framework!");
  return;
#endif

  backend_->start();
  register_entity_callbacks();
}

void WebServerCustom::loop() {}

void WebServerCustom::dump_config() {
  ESP_LOGCONFIG(TAG, "Custom Web Server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Auth: %s", auth_user_.empty() ? "disabled" : "enabled");
}

// ---------------------------------------------------------------------------
// Entity state callbacks → SSE push
// ---------------------------------------------------------------------------
void WebServerCustom::register_entity_callbacks() {
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal()) continue;
    sw->add_on_state_callback([this, sw](bool) {
      JsonDocument doc;
      build_switch_json(doc.to<JsonObject>(), sw);
      std::string out; serializeJson(doc, out);
      broadcast_state(out);
    });
  }
#endif

#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    if (light->is_internal()) continue;
    auto *listener = new LightChangeListener([this, light]() {
      JsonDocument doc;
      build_light_json(doc.to<JsonObject>(), light);
      std::string out; serializeJson(doc, out);
      broadcast_state(out);
    });
    light->add_target_state_reached_listener(listener);
  }
#endif

#ifdef USE_SENSOR
  for (auto *sensor : App.get_sensors()) {
    if (sensor->is_internal()) continue;
    sensor->add_on_state_callback([this, sensor](float) {
      JsonDocument doc;
      build_sensor_json(doc.to<JsonObject>(), sensor);
      std::string out; serializeJson(doc, out);
      broadcast_state(out);
    });
  }
#endif

#ifdef USE_BINARY_SENSOR
  for (auto *bs : App.get_binary_sensors()) {
    if (bs->is_internal()) continue;
    bs->add_on_state_callback([this, bs](bool) {
      JsonDocument doc;
      build_binary_sensor_json(doc.to<JsonObject>(), bs);
      std::string out; serializeJson(doc, out);
      broadcast_state(out);
    });
  }
#endif

#ifdef USE_TEXT_SENSOR
  for (auto *ts : App.get_text_sensors()) {
    if (ts->is_internal()) continue;
    ts->add_on_state_callback([this, ts](const std::string &) {
      JsonDocument doc;
      build_text_sensor_json(doc.to<JsonObject>(), ts);
      std::string out; serializeJson(doc, out);
      broadcast_state(out);
    });
  }
#endif
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------
void WebServerCustom::build_all_entities_json(JsonDocument &doc) {
  JsonObject root = doc.to<JsonObject>();
  root["device_name"]     = App.get_name().c_str();
  root["friendly_name"]   = App.get_friendly_name().c_str();
  root["esphome_version"] = ESPHOME_VERSION;
  root["uptime"]          = millis() / 1000;

#ifdef USE_SWITCH
  JsonArray switches = root["switches"].to<JsonArray>();
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal()) continue;
    build_switch_json(switches.add<JsonObject>(), sw);
  }
#endif
#ifdef USE_LIGHT
  JsonArray lights = root["lights"].to<JsonArray>();
  for (auto *light : App.get_lights()) {
    if (light->is_internal()) continue;
    build_light_json(lights.add<JsonObject>(), light);
  }
#endif
#ifdef USE_SENSOR
  JsonArray sensors = root["sensors"].to<JsonArray>();
  for (auto *sensor : App.get_sensors()) {
    if (sensor->is_internal()) continue;
    build_sensor_json(sensors.add<JsonObject>(), sensor);
  }
#endif
#ifdef USE_BINARY_SENSOR
  JsonArray binary_sensors = root["binary_sensors"].to<JsonArray>();
  for (auto *bs : App.get_binary_sensors()) {
    if (bs->is_internal()) continue;
    build_binary_sensor_json(binary_sensors.add<JsonObject>(), bs);
  }
#endif
#ifdef USE_TEXT_SENSOR
  JsonArray text_sensors = root["text_sensors"].to<JsonArray>();
  for (auto *ts : App.get_text_sensors()) {
    if (ts->is_internal()) continue;
    build_text_sensor_json(text_sensors.add<JsonObject>(), ts);
  }
#endif
#ifdef USE_CLIMATE
  JsonArray climates = root["climates"].to<JsonArray>();
  for (auto *climate : App.get_climates()) {
    if (climate->is_internal()) continue;
    build_climate_json(climates.add<JsonObject>(), climate);
  }
#endif
#ifdef USE_FAN
  JsonArray fans = root["fans"].to<JsonArray>();
  for (auto *fan : App.get_fans()) {
    if (fan->is_internal()) continue;
    build_fan_json(fans.add<JsonObject>(), fan);
  }
#endif
#ifdef USE_NUMBER
  JsonArray numbers = root["numbers"].to<JsonArray>();
  for (auto *number : App.get_numbers()) {
    if (number->is_internal()) continue;
    build_number_json(numbers.add<JsonObject>(), number);
  }
#endif
#ifdef USE_SELECT
  JsonArray selects = root["selects"].to<JsonArray>();
  for (auto *select : App.get_selects()) {
    if (select->is_internal()) continue;
    build_select_json(selects.add<JsonObject>(), select);
  }
#endif
#ifdef USE_BUTTON
  JsonArray buttons = root["buttons"].to<JsonArray>();
  for (auto *button : App.get_buttons()) {
    if (button->is_internal()) continue;
    JsonObject obj = buttons.add<JsonObject>();
    obj["id"]   = make_id(button->get_name()).c_str();
    obj["name"] = button->get_name().c_str();
    obj["type"] = "button";
  }
#endif
}

#ifdef USE_SWITCH
void WebServerCustom::build_switch_json(JsonObject obj, switch_::Switch *sw) {
  obj["id"] = make_id(sw->get_name()).c_str();
  obj["name"] = sw->get_name().c_str();
  obj["type"] = "switch";
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
  if (traits.supports_color_mode(light::ColorMode::COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::COLD_WARM_WHITE)) {
    obj["color_temp"] = values.get_color_temperature();
    obj["min_mireds"] = traits.get_min_mireds();
    obj["max_mireds"] = traits.get_max_mireds();
  }
  if (traits.supports_color_mode(light::ColorMode::RGB) ||
      traits.supports_color_mode(light::ColorMode::RGB_WHITE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLD_WARM_WHITE)) {
    obj["r"] = (int)(values.get_red()   * 255);
    obj["g"] = (int)(values.get_green() * 255);
    obj["b"] = (int)(values.get_blue()  * 255);
  }
  JsonArray effects = obj["effects"].to<JsonArray>();
  for (auto &effect : light->get_effects()) effects.add(effect->get_name().c_str());
  obj["effect"] = light->get_effect_name().c_str();
}
#endif

#ifdef USE_SENSOR
void WebServerCustom::build_sensor_json(JsonObject obj, sensor::Sensor *sensor) {
  obj["id"]           = make_id(sensor->get_name()).c_str();
  obj["name"]         = sensor->get_name().c_str();
  obj["type"]         = "sensor";
  obj["unit"]         = sensor->get_unit_of_measurement().c_str();
  obj["device_class"] = safe_device_class(sensor).c_str();
  if (sensor->has_state()) obj["state"] = sensor->get_state();
  else obj["state"] = nullptr;
}
#endif

#ifdef USE_BINARY_SENSOR
void WebServerCustom::build_binary_sensor_json(JsonObject obj, binary_sensor::BinarySensor *bs) {
  obj["id"]           = make_id(bs->get_name()).c_str();
  obj["name"]         = bs->get_name().c_str();
  obj["type"]         = "binary_sensor";
  obj["device_class"] = safe_device_class(bs).c_str();
  obj["state"]        = bs->state;
}
#endif

#ifdef USE_TEXT_SENSOR
void WebServerCustom::build_text_sensor_json(JsonObject obj, text_sensor::TextSensor *ts) {
  obj["id"]           = make_id(ts->get_name()).c_str();
  obj["name"]         = ts->get_name().c_str();
  obj["type"]         = "text_sensor";
  obj["device_class"] = safe_device_class(ts).c_str();
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
