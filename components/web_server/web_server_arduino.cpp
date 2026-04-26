#include "web_server_backend.h"
#include "web_server.h"

#if defined(ARDUINO)

#include <ESPAsyncWebServer.h>
#include <ESPAsyncTCP.h>
#include <ArduinoJson.h>

namespace esphome {
namespace web_server_custom {

class ArduinoWebServer : public IWebServer {
 public:
  ArduinoWebServer(WebServerCustom *parent, uint16_t port)
      : parent_(parent), server_(port), events_("/events") {}

  void begin() override {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    auto captive_redirect = [](AsyncWebServerRequest *req) {
      if (!wifi::global_wifi_component->is_connected()) {
        req->redirect("http://" + req->host() + "/setup");
      } else {
        req->send(204);
      }
    };

    server_.on("/hotspot-detect.html", HTTP_GET, captive_redirect);
    server_.on("/generate_204", HTTP_GET, captive_redirect);
    server_.on("/ncsi.txt", HTTP_GET, captive_redirect);
    server_.on("/connecttest.txt", HTTP_GET, captive_redirect);
    server_.on("/redirect", HTTP_GET, captive_redirect);
    server_.on("/canonical.html", HTTP_GET, captive_redirect);
    server_.on("/success.txt", HTTP_GET, captive_redirect);

    server_.on("/setup", HTTP_GET, [](AsyncWebServerRequest *req) {
      AsyncWebServerResponse *res = req->beginResponse_P(
          200, "text/html", (const uint8_t *)WIFI_SETUP_HTML, strlen_P(WIFI_SETUP_HTML));
      res->addHeader("Cache-Control", "no-cache");
      req->send(res);
    });

    server_.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
      String json = "[";
      bool first = true;
      for (auto &net : wifi::global_wifi_component->get_scan_result()) {
        if (!first) json += ",";
        first = false;
        String ssid = net.get_ssid().c_str();
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(net.get_rssi()) +
                ",\"secure\":" + (net.get_is_hidden() ? "false" : "true") + "}";
      }
      json += "]";
      req->send(200, "application/json", json);
    });

    server_.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "missing ssid");
        return;
      }
      String ssid = req->getParam("ssid", true)->value();
      String pass = req->hasParam("password", true)
                        ? req->getParam("password", true)->value()
                        : "";

      wifi::WiFiAP sta{};
      sta.set_ssid(ssid.c_str());
      sta.set_password(pass.c_str());
      wifi::global_wifi_component->set_sta(sta);

      req->send(200, "text/plain", "ok");

      App.scheduler.set_timeout(wifi::global_wifi_component, "wifi_reboot", 500, []() {
        App.safe_reboot();
      });
    });

    server_.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
      parent_->handle_index(req);
    });

    server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *req) {
      if (!parent_->check_auth(req)) return;
      parent_->handle_state(req);
    });

    server_.on("/", HTTP_OPTIONS, [](AsyncWebServerRequest *req) { req->send(204); });

    // ── SSE ────────────────────────────────────────────────
    events_.onConnect([this](AsyncEventSourceClient *client) {
      parent_->send_full_state(client);
    });

    server_.addHandler(&events_);

    // ── 404 ────────────────────────────────────────────────
    server_.onNotFound([](AsyncWebServerRequest *req) {
      if (req->url().startsWith("/api/"))
        req->send(404, "application/json", "{\"error\":\"not found\"}");
      else
        req->send(404);
    });

    server_.begin();
  }

 private:
  WebServerCustom *parent_;
  AsyncWebServer server_;
  AsyncEventSource events_;
};

}  // namespace web_server_custom
}  // namespace esphome

#endif