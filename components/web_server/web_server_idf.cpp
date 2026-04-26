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
// SSE client
// ─────────────────────────────────────────────
struct SseClient {
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

  // ─────────────────────────────────────────────
  // SEND EVENT (ASYNC)
  // ─────────────────────────────────────────────
  void send_event(const char *data, const char *event_type) override {
    std::string frame = "event: ";
    frame += event_type;
    frame += "\ndata: ";
    frame += data;
    frame += "\n\n";

    std::lock_guard<std::mutex> lock(sse_mutex_);

    for (auto it = clients_.begin(); it != clients_.end();) {
      int ret = httpd_socket_send(server_, it->fd, frame.c_str(), frame.size(), 0);

      if (ret < 0) {
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

  std::list<SseClient> clients_;
  std::mutex sse_mutex_;

  // ─────────────────────────────────────────────
  static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
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
  // ✅ CORRECT SSE HANDLER (PERSISTENT)
  // ─────────────────────────────────────────────
  static esp_err_t h_events(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    int fd = httpd_req_to_sockfd(req);

    {
      std::lock_guard<std::mutex> lock(self->sse_mutex_);

      if (self->clients_.size() >= 5) {
        httpd_sess_trigger_close(self->server_, fd);
        return ESP_FAIL;
      }

      self->clients_.push_back({fd});
    }

    // send initial full state
    JsonDocument doc;
    self->parent_->build_all_entities_json(doc);

    std::string payload;
    serializeJson(doc, payload);

    std::string init = "event: full_state\ndata: " + payload + "\n\n";
    httpd_socket_send(self->server_, fd, init.c_str(), init.size(), 0);

    // 🔥 KEEP CONNECTION ALIVE (REQUIRED FOR SSE)
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(15000));

      const char *ping = ": ping\n\n";
      int ret = httpd_socket_send(self->server_, fd, ping, strlen(ping), 0);

      if (ret < 0) {
        httpd_sess_trigger_close(self->server_, fd);

        std::lock_guard<std::mutex> lock(self->sse_mutex_);
        self->clients_.remove_if([fd](const SseClient &c) { return c.fd == fd; });

        break;
      }
    }

    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  void register_routes() {
    auto reg = [&](const char *uri,
                   httpd_method_t method,
                   esp_err_t (*handler)(httpd_req_t *)) {
      httpd_uri_t h = {};
      h.uri = uri;
      h.method = method;
      h.handler = handler;
      h.user_ctx = this;
      httpd_register_uri_handler(server_, &h);
    };

    reg("/", HTTP_GET, h_root);
    reg("/api/state", HTTP_GET, h_state);
    reg("/events", HTTP_GET, h_events);
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