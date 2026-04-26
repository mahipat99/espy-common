#include "web_server_backend.h"
#include "web_server.h"

#if defined(USE_ESP_IDF)

#include "esp_http_server.h"
#include "esp_log.h"

namespace esphome {
namespace web_server_custom {

static const char *TAG = "web_server_idf";

class IDFWebServer : public IWebServer {
 public:
  IDFWebServer(WebServerCustom *parent, uint16_t port)
      : parent_(parent), port_(port) {}

  void begin() override {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;

    ESP_LOGI(TAG, "Starting ESP-IDF web server on port %d", port_);

    if (httpd_start(&server_, &config) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to start HTTP server");
      return;
    }

    register_routes();

    ESP_LOGI(TAG, "Web server started");
  }

 private:
  WebServerCustom *parent_;
  uint16_t port_;
  httpd_handle_t server_{nullptr};

  static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    httpd_resp_send(
        req,
        reinterpret_cast<const char *>(WEB_UI_GZ),
        WEB_UI_GZ_LEN
    );

    return ESP_OK;
  }

  static esp_err_t state_handler(httpd_req_t *req) {
    auto *self = static_cast<WebServerCustom *>(req->user_ctx);

    DynamicJsonDocument doc(4096);
    self->build_all_entities_json(doc);

    std::string out;
    serializeJson(doc, out);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    httpd_resp_send(req, out.c_str(), out.length());

    return ESP_OK;
  }

  void register_routes() {
    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = root_handler;
    root.user_ctx = parent_;
    httpd_register_uri_handler(server_, &root);

    httpd_uri_t state = {};
    state.uri = "/api/state";
    state.method = HTTP_GET;
    state.handler = state_handler;
    state.user_ctx = parent_;
    httpd_register_uri_handler(server_, &state);
  }
};

}  // namespace web_server_custom
}  // namespace esphome

esphome::web_server_custom::IWebServer *
make_idf_server(esphome::web_server_custom::WebServerCustom *parent, uint16_t port) {
  return new esphome::web_server_custom::IDFWebServer(parent, port);
}

#endif