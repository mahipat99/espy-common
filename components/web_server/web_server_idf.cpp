#if defined(USE_ESP_IDF) || (!defined(USE_ARDUINO) && defined(ESP32))

#include "web_server_backend.h"
#include "web_server.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <ArduinoJson.h>
#include <list>
#include <mutex>
#include <string>

namespace esphome {
namespace web_server_custom {

static const char *TAG_IDF = "web_server_idf";

// ─────────────────────────────────────────────
// WebSocket client
// ─────────────────────────────────────────────
struct WsClient {
  int fd;
};

// ─────────────────────────────────────────────
// Backend
// ─────────────────────────────────────────────
class IDFBackend : public IWebServer {
 public:
  explicit IDFBackend(WebServerCustom *parent) : parent_(parent) {}

  void start() override {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = parent_->get_port();
    config.max_uri_handlers = 32;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) {
      ESP_LOGE(TAG_IDF, "Failed to start HTTP server");
      return;
    }

    register_routes();
    ESP_LOGCONFIG(TAG_IDF, "Web server started on port %u", parent_->get_port());
  }

  // 🔥 send event to ALL websocket clients
  void send_event(const char *data, const char *event_type) override {
    std::string payload = "{\"type\":\"";
    payload += event_type;
    payload += "\",\"data\":";
    payload += data;
    payload += "}";

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)payload.c_str();
    frame.len = payload.size();

    std::lock_guard<std::mutex> lock(ws_mutex_);

    for (auto it = clients_.begin(); it != clients_.end();) {
      int ret = httpd_ws_send_frame_async(server_, it->fd, &frame);
      if (ret != ESP_OK) {
        httpd_sess_trigger_close(server_, it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  WebServerCustom *parent_;
  httpd_handle_t server_{nullptr};

  std::list<WsClient> clients_;
  std::mutex ws_mutex_;

  // ─────────────────────────────────────────────
  static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  }

  // ─────────────────────────────────────────────
  static esp_err_t h_root(httpd_req_t *req) {
    set_cors(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)WEB_UI_GZ, WEB_UI_GZ_LEN);
    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  static esp_err_t h_state(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);

    JsonDocument doc;
    self->parent_->build_all_entities_json(doc);

    std::string out;
    serializeJson(doc, out);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out.c_str(), out.size());
    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  // 🔥 WEBSOCKET HANDLER
  // ─────────────────────────────────────────────
  static esp_err_t h_ws(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);

    if (req->method == HTTP_GET) {
      // handshake
      int fd = httpd_req_to_sockfd(req);

      {
        std::lock_guard<std::mutex> lock(self->ws_mutex_);
        if (self->clients_.size() >= 5) {
          httpd_sess_trigger_close(self->server_, fd);
          return ESP_FAIL;
        }
        self->clients_.push_back({fd});
      }

      // send initial state
      JsonDocument doc;
      self->parent_->build_all_entities_json(doc);

      std::string payload;
      serializeJson(doc, payload);

      std::string msg = "{\"type\":\"full_state\",\"data\":" + payload + "}";

      httpd_ws_frame_t frame = {};
      frame.type = HTTPD_WS_TYPE_TEXT;
      frame.payload = (uint8_t *)msg.c_str();
      frame.len = msg.size();

      httpd_ws_send_frame(req, &frame);

      return ESP_OK;
    }

    // receive frame (optional)
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = nullptr;

    httpd_ws_recv_frame(req, &frame, 0);

    if (frame.len) {
      frame.payload = (uint8_t *)malloc(frame.len + 1);
      httpd_ws_recv_frame(req, &frame, frame.len);
      frame.payload[frame.len] = 0;

      // (optional: handle commands here)

      free(frame.payload);
    }

    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  void register_routes() {
    auto reg = [&](const char *uri,
                   httpd_method_t method,
                   esp_err_t (*handler)(httpd_req_t *),
                   bool is_ws = false) {
      httpd_uri_t h = {};
      h.uri = uri;
      h.method = method;
      h.handler = handler;
      h.user_ctx = this;
      h.is_websocket = is_ws;
      httpd_register_uri_handler(server_, &h);
    };

    reg("/", HTTP_GET, h_root);
    reg("/api/state", HTTP_GET, h_state);
    reg("/ws", HTTP_GET, h_ws, true);   // 🔥 WebSocket endpoint
  }
};

// ─────────────────────────────────────────────
// Factory
// ─────────────────────────────────────────────
IWebServer *make_idf_server(WebServerCustom *parent, uint16_t port) {
  (void)port;
  return new IDFBackend(parent);
}

}  // namespace web_server_custom
}  // namespace esphome

#endif