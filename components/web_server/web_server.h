#pragma once

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/entity_base.h"
#include "esphome/components/wifi/wifi_component.h"
#include "web_ui_data.h"
#include "web_server_backend.h"

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

#ifdef USE_ARDUINO
  #ifdef USE_ESP32
    #include <AsyncTCP.h>
  #elif defined(USE_ESP8266)
    #include <ESPAsyncTCP.h>
  #endif
  #include <ESPAsyncWebServer.h>
#endif

#include <ArduinoJson.h>
#include <string>
#include <functional>
#include <memory>

namespace esphome {
namespace web_server_custom {

class WebServerCustom : public Component {
 public:
  WebServerCustom();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { port_ = port; }
  uint16_t get_port() const { return port_; }

  void set_auth(const std::string &user, const std::string &pass) {
    auth_user_ = user;
    auth_pass_ = pass;
  }

  void broadcast_state(const std::string &json_str);

  void build_all_entities_json(JsonDocument &doc);
  static std::string make_id(const std::string &name);
  static std::string safe_device_class(EntityBase *e);

  void register_entity_callbacks();

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

 protected:
  uint16_t port_{80};
  std::string auth_user_;
  std::string auth_pass_;

  std::unique_ptr<IWebServer> backend_;
};

}  // namespace web_server_custom
}  // namespace esphome
