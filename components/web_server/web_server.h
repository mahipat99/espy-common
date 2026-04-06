#pragma once

#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/application.h"
#include "esphome/core/entity_base.h"
#include "web_ui_data.h"  // auto-generated embedded SPA

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#include "esphome/components/light/light_color_values.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#endif
#ifdef USE_FAN
#include "esphome/components/fan/fan.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

#ifdef USE_ESP32
#include <AsyncTCP.h>
#elif defined(USE_ESP8266)
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>

namespace esphome {
namespace web_server_custom {

class WebServerCustom : public Component {
 public:
  WebServerCustom();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  void set_port(uint16_t port) { port_ = port; }
  void set_auth(const std::string &user, const std::string &pass) {
    auth_user_ = user;
    auth_pass_ = pass;
  }

 protected:
  void handle_index(AsyncWebServerRequest *request);
  void handle_state(AsyncWebServerRequest *request);
  void handle_switch_toggle(AsyncWebServerRequest *request, const String &entity_id);
  void handle_light_set(AsyncWebServerRequest *request, const String &entity_id);
  void handle_fan_set(AsyncWebServerRequest *request, const String &entity_id);
  void handle_number_set(AsyncWebServerRequest *request, const String &entity_id);
  void handle_select_set(AsyncWebServerRequest *request, const String &entity_id);
  void handle_climate_set(AsyncWebServerRequest *request, const String &entity_id);
  void handle_button_press(AsyncWebServerRequest *request, const String &entity_id);

  void send_full_state(AsyncEventSourceClient *client);
  void build_all_entities_json(JsonDocument &doc);

#ifdef USE_SWITCH
  void build_switch_json(JsonObject obj, switch_::Switch *sw);
#endif
#ifdef USE_LIGHT
  void build_light_json(JsonObject obj, light::LightState *light);
#endif
#ifdef USE_SENSOR
  void build_sensor_json(JsonObject obj, sensor::Sensor *sensor);
#endif
#ifdef USE_BINARY_SENSOR
  void build_binary_sensor_json(JsonObject obj, binary_sensor::BinarySensor *bs);
#endif
#ifdef USE_TEXT_SENSOR
  void build_text_sensor_json(JsonObject obj, text_sensor::TextSensor *ts);
#endif
#ifdef USE_CLIMATE
  void build_climate_json(JsonObject obj, climate::Climate *climate);
#endif
#ifdef USE_FAN
  void build_fan_json(JsonObject obj, fan::Fan *fan);
#endif
#ifdef USE_NUMBER
  void build_number_json(JsonObject obj, number::Number *number);
#endif
#ifdef USE_SELECT
  void build_select_json(JsonObject obj, select::Select *select);
#endif

  bool check_auth(AsyncWebServerRequest *request);
  static std::string make_id(const std::string &name);

  uint16_t port_{80};
  std::string auth_user_;
  std::string auth_pass_;

  AsyncWebServer *server_{nullptr};
  AsyncEventSource *events_{nullptr};
};

}  // namespace web_server_custom
}  // namespace esphome
