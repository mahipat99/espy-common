#if defined(USE_ESP_IDF) || (!defined(USE_ARDUINO) && defined(ESP32))

#include "web_server_backend.h"
#include "web_server.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/tcp.h"         // TCP_NODELAY
#include "lwip/sockets.h"

#include <ArduinoJson.h>
#include <list>
#include <mutex>
#include <string>

namespace esphome {
namespace web_server_custom {

static const char *TAG_IDF = "web_server_idf";

// ─────────────────────────────────────────────
// SSE client — stores the socket fd
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
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = parent_->get_port();
    config.max_uri_handlers = 32;
    config.uri_match_fn     = httpd_uri_match_wildcard;

    // FIX 1: Give the httpd its own stack that is large enough for JSON work.
    config.stack_size       = 8192;

    // FIX 2: More worker tasks so SSE connections don't starve API calls.
    //        Each persistent SSE client occupies one task, so size this to
    //        (max_sse_clients + spare_for_normal_requests).
    config.max_open_sockets = 10;

    if (httpd_start(&server_, &config) != ESP_OK) {
      ESP_LOGE(TAG_IDF, "Failed to start HTTP server");
      return;
    }

    register_routes();
    ESP_LOGCONFIG(TAG_IDF, "Web server started on port %u", parent_->get_port());
  }

  // ─────────────────────────────────────────────
  // SEND EVENT — called from any task/thread
  // ─────────────────────────────────────────────
  void send_event(const char *data, const char *event_type) override {
    std::string frame = "event: ";
    frame += event_type;
    frame += "\ndata: ";
    frame += data;
    frame += "\n\n";

    std::lock_guard<std::mutex> lock(sse_mutex_);

    for (auto it = clients_.begin(); it != clients_.end();) {
      // FIX 3: Use raw lwip send() instead of httpd_socket_send().
      //        httpd_socket_send() is not safe to call from outside the
      //        httpd task that owns the connection.
      int ret = send(it->fd, frame.c_str(), frame.size(), MSG_DONTWAIT);

      if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Dead client — schedule close and remove from list.
        httpd_sess_trigger_close(server_, it->fd);
        it = clients_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  WebServerCustom  *parent_;
  httpd_handle_t    server_{nullptr};

  std::list<SseClient> clients_;
  std::mutex           sse_mutex_;

  // ─────────────────────────────────────────────
  // Helpers
  // ─────────────────────────────────────────────
  static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  }

  // FIX 4: Disable Nagle's algorithm so SSE frames are sent immediately
  //        instead of being buffered by the TCP stack (major source of delay).
  static void set_tcp_nodelay(httpd_req_t *req) {
    int fd  = httpd_req_to_sockfd(req);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }

  // ─────────────────────────────────────────────
  // GET /
  // ─────────────────────────────────────────────
  static esp_err_t h_root(httpd_req_t *req) {
    set_cors(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control",    "no-cache");
    httpd_resp_send(req, (const char *)WEB_UI_GZ, WEB_UI_GZ_LEN);
    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  // GET /api/state
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
  // GET /events  — SSE (Server-Sent Events)
  //
  // FIX 5: The old handler blocked its httpd worker thread forever with
  //        vTaskDelay inside a while-loop.  With a small thread pool this
  //        starved every other request.
  //
  //        New approach:
  //          • Register the client fd in the shared clients_ list.
  //          • Send the initial full_state snapshot.
  //          • Return ESP_OK immediately — the httpd thread is released.
  //          • Future events arrive via send_event() which writes directly
  //            to the raw socket fd (non-blocking).
  //          • A lightweight FreeRTOS keep-alive task handles ping frames
  //            and removes dead sockets without tying up httpd workers.
  // ─────────────────────────────────────────────
  static esp_err_t h_events(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);
    set_tcp_nodelay(req);

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection",    "keep-alive");
    // Tell the httpd layer NOT to close the connection when we return.
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    // Grab the raw socket fd before we release the request.
    int fd = httpd_req_to_sockfd(req);

    // Send "connected" comment.
    const char *hello = ": connected\n\n";
    if (httpd_resp_send_chunk(req, hello, strlen(hello)) != ESP_OK) {
      return ESP_FAIL;
    }

    // Send full state snapshot.
    {
      JsonDocument doc;
      self->parent_->build_all_entities_json(doc);
      std::string payload;
      serializeJson(doc, payload);
      std::string msg = "event: full_state\ndata: " + payload + "\n\n";
      if (httpd_resp_send_chunk(req, msg.c_str(), msg.size()) != ESP_OK) {
        return ESP_FAIL;
      }
    }

    // Register client so send_event() can push future updates.
    {
      std::lock_guard<std::mutex> lock(self->sse_mutex_);
      self->clients_.push_back(SseClient{fd});
    }

    // FIX 6: Spawn a tiny task that sends periodic pings and detects
    //        disconnects — without blocking any httpd worker thread.
    //        The task owns nothing heap-critical; it cleans itself up.
    struct PingCtx {
      IDFBackend *self;
      int         fd;
    };
    auto *ctx   = new PingCtx{self, fd};

    xTaskCreate(
      [](void *arg) {
        auto *ctx  = static_cast<PingCtx *>(arg);
        auto *self = ctx->self;
        int   fd   = ctx->fd;
        delete ctx;

        while (true) {
          vTaskDelay(pdMS_TO_TICKS(25000));  // 25-second ping interval

          const char *ping = ": ping\n\n";
          int ret = send(fd, ping, strlen(ping), MSG_DONTWAIT);

          if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Socket is dead — remove from client list.
            std::lock_guard<std::mutex> lock(self->sse_mutex_);
            self->clients_.remove_if(
              [fd](const SseClient &c) { return c.fd == fd; });
            httpd_sess_trigger_close(self->server_, fd);
            break;
          }
        }

        vTaskDelete(nullptr);
      },
      "sse_ping",
      2048,
      ctx,
      tskIDLE_PRIORITY + 1,
      nullptr
    );

    // FIX 7: Return ESP_OK without sending the terminal NULL chunk.
    //        Sending NULL chunk tells esp_httpd to close the connection.
    //        We want to keep it open — sending to the fd directly is enough.
    return ESP_OK;
  }

  // ─────────────────────────────────────────────
  void register_routes() {
    auto reg = [&](const char *uri,
                   httpd_method_t method,
                   esp_err_t (*handler)(httpd_req_t *)) {
      httpd_uri_t h = {};
      h.uri         = uri;
      h.method      = method;
      h.handler     = handler;
      h.user_ctx    = this;
      httpd_register_uri_handler(server_, &h);
    };

    reg("/",          HTTP_GET, h_root);
    reg("/api/state", HTTP_GET, h_state);
    reg("/events",    HTTP_GET, h_events);
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