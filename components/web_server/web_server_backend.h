#pragma once

#include <stdint.h>

namespace esphome {
namespace web_server_custom {

class WebServerCustom;

class IWebServer {
 public:
  virtual ~IWebServer() = default;

  virtual void start() = 0;

  virtual void send_event(const char *data, const char *event_type) = 0;
};

}  // namespace web_server_custom
}  // namespace esphome

#if defined(USE_ESP_IDF)

namespace esphome {
namespace web_server_custom {

IWebServer *make_idf_server(WebServerCustom *parent, uint16_t port);

}  // namespace web_server_custom
}  // namespace esphome

#endif


#if defined(ARDUINO)

namespace esphome {
namespace web_server_custom {

IWebServer *make_arduino_server(WebServerCustom *parent, uint16_t port);

}  // namespace web_server_custom
}  // namespace esphome

#endif