// web_server_arduino.cpp — Arduino framework backend (ESP8266 + ESP32 Arduino)
#ifdef USE_ARDUINO

#include "web_server.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

#ifdef USE_ESP32
#include <AsyncTCP.h>
#elif defined(USE_ESP8266)
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

namespace esphome {
namespace web_server_custom {

static const char *TAG_ARD = "web_server_arduino";

// ---------------------------------------------------------------------------
// WiFi setup page — shown when device is in AP / fallback mode
// ---------------------------------------------------------------------------
static const char WIFI_SETUP_HTML[] PROGMEM = R"RAW(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title><style>
*{box-sizing:border-box;margin:0;padding:0}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f1117;color:#e8eaf6;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#1a1d27;border:1px solid #2e3245;border-radius:12px;padding:28px 24px;width:100%;max-width:400px}
h1{font-size:20px;font-weight:600;margin-bottom:4px}.sub{color:#8b90b8;font-size:13px;margin-bottom:24px}
label{display:block;font-size:13px;color:#8b90b8;margin-bottom:4px}
input,select{width:100%;padding:10px 12px;background:#232736;border:1px solid #2e3245;border-radius:8px;color:#e8eaf6;font-size:14px;margin-bottom:16px;outline:none}
input:focus,select:focus{border-color:#5b6ef5}
.btn{width:100%;padding:12px;background:#5b6ef5;border:none;border-radius:8px;color:#fff;font-size:15px;font-weight:600;cursor:pointer}
.btn:disabled{opacity:.5;cursor:not-allowed}
#status{margin-top:12px;font-size:13px;text-align:center;color:#8b90b8;min-height:20px}
#status.ok{color:#4ade80}#status.err{color:#fb923c}
.scan-row{display:flex;gap:8px;margin-bottom:16px}.scan-row select{margin-bottom:0;flex:1}
.scan-btn{padding:10px 14px;background:#232736;border:1px solid #2e3245;border-radius:8px;color:#8b90b8;font-size:13px;cursor:pointer}
</style></head><body><div class="card">
<h1>WiFi Setup</h1><div class="sub" id="dev-name">ESPHome device</div>
<label>Network</label><div class="scan-row">
<select id="ssid-select" onchange="onSelect(this.value)"><option value="">— Select or type below —</option></select>
<button class="scan-btn" onclick="scan()">Scan</button></div>
<label>SSID</label><input type="text" id="ssid" placeholder="Network name" autocomplete="off" autocorrect="off" autocapitalize="none">
<label>Password</label><input type="password" id="pass" placeholder="Leave blank if open">
<button class="btn" id="submit-btn" onclick="doConnect()">Connect</button>
<div id="status"></div></div>
<script>
fetch('/api/state').then(r=>r.json()).then(s=>{const n=s.friendly_name||s.device_name||'ESPHome device';document.getElementById('dev-name').textContent=n;document.title='WiFi Setup — '+n;}).catch(()=>{});
function onSelect(v){if(v)document.getElementById('ssid').value=v;}
async function scan(){const sel=document.getElementById('ssid-select');sel.innerHTML='<option>Scanning…</option>';try{const r=await fetch('/api/wifi/scan');const nets=await r.json();sel.innerHTML='<option value="">— Select network —</option>'+nets.map(n=>`<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.rssi} dBm)${n.secure?' 🔒':''}</option>`).join('');}catch(e){sel.innerHTML='<option value="">Scan failed</option>';}}
async function doConnect(){const ssid=document.getElementById('ssid').value.trim();const pass=document.getElementById('pass').value;if(!ssid){setStatus('Enter a network name','err');return;}const btn=document.getElementById('submit-btn');btn.disabled=true;setStatus('Connecting…','');try{const fd=new FormData();fd.append('ssid',ssid);fd.append('password',pass);const r=await fetch('/api/wifi/connect',{method:'POST',body:fd});if(r.ok){setStatus('Saved! Device rebooting…','ok');}else{setStatus('Error: '+await r.text(),'err');btn.disabled=false;}}catch(e){setStatus('Request failed','err');btn.disabled=false;}}
function setStatus(msg,cls){const el=document.getElementById('status');el.textContent=msg;el.className=cls;}
function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
</script></body></html>)RAW";

// ---------------------------------------------------------------------------
// Arduino backend implementation
// ---------------------------------------------------------------------------
class ArduinoBackend : public IWebServerBackend {
 public:
  ArduinoBackend(WebServerCustom *parent)
      : parent_(parent), server_(parent->port_), events_("/events") {}

  void start() override {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

    // ── Captive portal probes ────────────────────────────────────────────
    auto captive = [](AsyncWebServerRequest *req) {
      if (!wifi::global_wifi_component->is_connected())
        req->redirect("http://" + req->host() + "/setup");
      else
        req->send(204);
    };
    server_.on("/hotspot-detect.html", HTTP_GET, captive);
    server_.on("/generate_204",        HTTP_GET, captive);
    server_.on("/ncsi.txt",            HTTP_GET, captive);
    server_.on("/connecttest.txt",     HTTP_GET, captive);
    server_.on("/redirect",            HTTP_GET, captive);
    server_.on("/canonical.html",      HTTP_GET, captive);
    server_.on("/success.txt",         HTTP_GET, captive);

    // ── WiFi setup page ──────────────────────────────────────────────────
    server_.on("/setup", HTTP_GET, [](AsyncWebServerRequest *req) {
      AsyncWebServerResponse *res = req->beginResponse_P(
          200, "text/html",
          reinterpret_cast<const uint8_t *>(WIFI_SETUP_HTML),
          strlen_P(WIFI_SETUP_HTML));
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
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + net.get_rssi() +
                ",\"secure\":" + (net.get_is_hidden() ? "false" : "true") + "}";
      }
      json += "]";
      req->send(200, "application/json", json);
    });

    server_.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
      if (!req->hasParam("ssid", true)) { req->send(400, "text/plain", "missing ssid"); return; }
      String ssid = req->getParam("ssid", true)->value();
      String pass = req->hasParam("password", true) ? req->getParam("password", true)->value() : "";
      wifi::WiFiAP sta{};
      sta.set_ssid(ssid.c_str());
      sta.set_password(pass.c_str());
      wifi::global_wifi_component->set_sta(sta);
      req->send(200, "text/plain", "ok");
      App.scheduler.set_timeout(wifi::global_wifi_component, "wifi_reboot", 500,
                                []() { App.safe_reboot(); });
    });

    // ── SPA ─────────────────────────────────────────────────────────────
    server_.on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
      AsyncWebServerResponse *response = req->beginResponse_P(
          200, "text/html", WEB_UI_GZ, WEB_UI_GZ_LEN);
      response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "no-cache");
      req->send(response);
    });

    // ── REST: state ──────────────────────────────────────────────────────
    server_.on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      JsonDocument doc;
      parent_->build_all_entities_json(doc);
      String out; serializeJson(doc, out);
      req->send(200, "application/json", out);
    });

    // ── REST: switch ─────────────────────────────────────────────────────
    server_.on("/api/switch", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/switch/");
#ifdef USE_SWITCH
      for (auto *sw : App.get_switches()) {
        if (WebServerCustom::make_id(sw->get_name()) == id.c_str()) {
          sw->toggle();
          req->send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: light ──────────────────────────────────────────────────────
    server_.on("/api/light", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/light/");
#ifdef USE_LIGHT
      for (auto *light : App.get_lights()) {
        if (WebServerCustom::make_id(light->get_name()) == id.c_str()) {
          auto call = light->make_call();
          if (req->hasParam("state", true)) {
            String v = req->getParam("state", true)->value();
            call.set_state(v == "on" || v == "1" || v == "true");
          }
          if (req->hasParam("brightness", true))
            call.set_brightness(req->getParam("brightness", true)->value().toFloat() / 255.0f);
          if (req->hasParam("color_temp", true))
            call.set_color_temperature(req->getParam("color_temp", true)->value().toFloat());
          if (req->hasParam("r", true) && req->hasParam("g", true) && req->hasParam("b", true))
            call.set_rgb(req->getParam("r", true)->value().toFloat() / 255.0f,
                         req->getParam("g", true)->value().toFloat() / 255.0f,
                         req->getParam("b", true)->value().toFloat() / 255.0f);
          if (req->hasParam("effect", true))
            call.set_effect(req->getParam("effect", true)->value().c_str());
          call.perform();
          req->send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: fan ────────────────────────────────────────────────────────
    server_.on("/api/fan", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/fan/");
#ifdef USE_FAN
      for (auto *fan : App.get_fans()) {
        if (WebServerCustom::make_id(fan->get_name()) == id.c_str()) {
          auto call = fan->make_call();
          if (req->hasParam("state", true)) {
            String v = req->getParam("state", true)->value();
            call.set_state(v == "on" || v == "1" || v == "true");
          }
          if (req->hasParam("speed", true))
            call.set_speed(req->getParam("speed", true)->value().toInt());
          call.perform();
          req->send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: number ─────────────────────────────────────────────────────
    server_.on("/api/number", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/number/");
#ifdef USE_NUMBER
      for (auto *number : App.get_numbers()) {
        if (WebServerCustom::make_id(number->get_name()) == id.c_str()) {
          if (req->hasParam("value", true)) {
            auto call = number->make_call();
            call.set_value(req->getParam("value", true)->value().toFloat());
            call.perform();
            req->send(200, "application/json", "{\"ok\":true}");
            return;
          }
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: select ─────────────────────────────────────────────────────
    server_.on("/api/select", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/select/");
#ifdef USE_SELECT
      for (auto *select : App.get_selects()) {
        if (WebServerCustom::make_id(select->get_name()) == id.c_str()) {
          if (req->hasParam("option", true)) {
            auto call = select->make_call();
            call.set_option(req->getParam("option", true)->value().c_str());
            call.perform();
            req->send(200, "application/json", "{\"ok\":true}");
            return;
          }
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: climate ────────────────────────────────────────────────────
    server_.on("/api/climate", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/climate/");
#ifdef USE_CLIMATE
      for (auto *climate : App.get_climates()) {
        if (WebServerCustom::make_id(climate->get_name()) == id.c_str()) {
          auto call = climate->make_call();
          if (req->hasParam("mode", true))
            call.set_mode(req->getParam("mode", true)->value().c_str());
          if (req->hasParam("target_temperature", true))
            call.set_target_temperature(req->getParam("target_temperature", true)->value().toFloat());
          call.perform();
          req->send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    // ── REST: button ─────────────────────────────────────────────────────
    server_.on("/api/button", HTTP_POST, [this](AsyncWebServerRequest *req) {
      if (!check_auth(req)) return;
      String id = extract_id(req->url(), "/api/button/");
#ifdef USE_BUTTON
      for (auto *button : App.get_buttons()) {
        if (WebServerCustom::make_id(button->get_name()) == id.c_str()) {
          button->press();
          req->send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
#endif
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    });

    server_.on("/", HTTP_OPTIONS, [](AsyncWebServerRequest *req) { req->send(204); });

    // ── SSE ──────────────────────────────────────────────────────────────
    events_.onConnect([this](AsyncEventSourceClient *client) {
      ESP_LOGD(TAG_ARD, "SSE client connected");
      JsonDocument doc;
      parent_->build_all_entities_json(doc);
      String payload; serializeJson(doc, payload);
      client->send(payload.c_str(), "full_state", millis());
    });
    server_.addHandler(&events_);

    // ── 404 ──────────────────────────────────────────────────────────────
    server_.onNotFound([](AsyncWebServerRequest *req) {
      if (req->url().startsWith("/api/"))
        req->send(404, "application/json", "{\"error\":\"not found\"}");
      else
        req->send(404);
    });

    server_.begin();
    ESP_LOGCONFIG(TAG_ARD, "Arduino web server started on port %u", parent_->port_);
  }

  void send_event(const char *data, const char *event_type) override {
    events_.send(data, event_type, millis());
  }

 private:
  WebServerCustom *parent_;
  AsyncWebServer server_;
  AsyncEventSource events_;

  bool check_auth(AsyncWebServerRequest *req) {
    if (parent_->auth_user_.empty()) return true;
    if (!req->authenticate(parent_->auth_user_.c_str(), parent_->auth_pass_.c_str())) {
      req->requestAuthentication();
      return false;
    }
    return true;
  }

  static String extract_id(const String &url, const char *prefix) {
    int s = url.indexOf(prefix);
    if (s < 0) return "";
    String id = url.substring(s + strlen(prefix));
    int slash = id.indexOf('/');
    if (slash >= 0) id = id.substring(0, slash);
    return id;
  }
};

IWebServerBackend *make_arduino_backend(WebServerCustom *parent) {
  return new ArduinoBackend(parent);
}

}  // namespace web_server_custom
}  // namespace esphome

#endif  // USE_ARDUINO
