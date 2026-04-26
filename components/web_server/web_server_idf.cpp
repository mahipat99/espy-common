// web_server_idf.cpp — ESP-IDF framework backend
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

// ---------------------------------------------------------------------------
// WiFi setup page
// ---------------------------------------------------------------------------
static const char WIFI_SETUP_HTML[] = R"RAW(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title><style>
*{box-sizing:border-box;margin:0;padding:0}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#0f1117;color:#e8eaf6;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#1a1d27;border:1px solid #2e3245;border-radius:12px;padding:28px 24px;width:100%;max-width:400px}
h1{font-size:20px;font-weight:600;margin-bottom:4px}.sub{color:#8b90b8;font-size:13px;margin-bottom:24px}
label{display:block;font-size:13px;color:#8b90b8;margin-bottom:4px}
input,select{width:100%;padding:10px 12px;background:#232736;border:1px solid #2e3245;border-radius:8px;color:#e8eaf6;font-size:14px;margin-bottom:16px;outline:none}
.btn{width:100%;padding:12px;background:#5b6ef5;border:none;border-radius:8px;color:#fff;font-size:15px;font-weight:600;cursor:pointer}
.btn:disabled{opacity:.5}#status{margin-top:12px;font-size:13px;text-align:center;color:#8b90b8}
#status.ok{color:#4ade80}#status.err{color:#fb923c}
.scan-row{display:flex;gap:8px;margin-bottom:16px}.scan-row select{margin-bottom:0;flex:1}
.scan-btn{padding:10px 14px;background:#232736;border:1px solid #2e3245;border-radius:8px;color:#8b90b8;font-size:13px;cursor:pointer}
</style></head><body><div class="card">
<h1>WiFi Setup</h1><div class="sub" id="dev-name">ESPHome device</div>
<label>Network</label><div class="scan-row">
<select id="ssid-select" onchange="onSelect(this.value)"><option value="">— Select or type below —</option></select>
<button class="scan-btn" onclick="scan()">Scan</button></div>
<label>SSID</label><input type="text" id="ssid" placeholder="Network name" autocomplete="off" autocapitalize="none">
<label>Password</label><input type="password" id="pass" placeholder="Leave blank if open">
<button class="btn" id="submit-btn" onclick="doConnect()">Connect</button>
<div id="status"></div></div>
<script>
fetch('/api/state').then(r=>r.json()).then(s=>{const n=s.friendly_name||s.device_name||'ESPHome';document.getElementById('dev-name').textContent=n;document.title='WiFi Setup — '+n;}).catch(()=>{});
function onSelect(v){if(v)document.getElementById('ssid').value=v;}
async function scan(){const sel=document.getElementById('ssid-select');sel.innerHTML='<option>Scanning…</option>';try{const r=await fetch('/api/wifi/scan');const nets=await r.json();sel.innerHTML='<option value="">— Select network —</option>'+nets.map(n=>`<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.rssi} dBm)</option>`).join('');}catch(e){sel.innerHTML='<option value="">Scan failed</option>';}}
async function doConnect(){const ssid=document.getElementById('ssid').value.trim();const pass=document.getElementById('pass').value;if(!ssid){setStatus('Enter a network name','err');return;}const btn=document.getElementById('submit-btn');btn.disabled=true;setStatus('Connecting…','');try{const r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)});if(r.ok)setStatus('Saved! Device rebooting…','ok');else{setStatus('Error','err');btn.disabled=false;}}catch(e){setStatus('Request failed','err');btn.disabled=false;}}
function setStatus(msg,cls){const el=document.getElementById('status');el.textContent=msg;el.className=cls;}
function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
</script></body></html>)RAW";

// ---------------------------------------------------------------------------
// SSE client tracking
// ---------------------------------------------------------------------------
struct SseClient {
  int fd;
};

// ---------------------------------------------------------------------------
// IDF backend
// ---------------------------------------------------------------------------
class IDFBackend : public IWebServer {
 public:
  IDFBackend(WebServerCustom *parent) : parent_(parent) {}

  void start() override {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = parent_->get_port();
    config.max_uri_handlers = 24;
    // Allow wildcard URI matching for /api/* paths
    config.uri_match_fn   = httpd_uri_match_wildcard;

    if (httpd_start(&server_, &config) != ESP_OK) {
      ESP_LOGE(TAG_IDF, "Failed to start HTTP server");
      return;
    }

    register_routes();
    ESP_LOGCONFIG(TAG_IDF, "IDF web server started on port %u", parent_->get_port());
  }

  void send_event(const char *data, const char *event_type) override {
    // Build SSE frame
    std::string frame = "event: ";
    frame += event_type;
    frame += "\ndata: ";
    frame += data;
    frame += "\n\n";

    std::lock_guard<std::mutex> lock(sse_mutex_);
    for (auto it = sse_clients_.begin(); it != sse_clients_.end(); ) {
      int ret = httpd_socket_send(server_, it->fd,
                                  frame.c_str(), frame.size(), 0);
      if (ret < 0) {
        it = sse_clients_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  WebServerCustom *parent_;
  httpd_handle_t   server_{nullptr};
  std::list<SseClient> sse_clients_;
  std::mutex           sse_mutex_;

  // ── Helpers ─────────────────────────────────────────────────────────────
  static std::string read_post_body(httpd_req_t *req) {
    std::string body(req->content_len, '\0');
    int received = httpd_req_recv(req, &body[0], req->content_len);
    if (received <= 0) return "";
    body.resize(received);
    return body;
  }

  static std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size()) {
        int v = 0;
        sscanf(s.c_str() + i + 1, "%2x", &v);
        out += (char)v;
        i += 2;
      } else if (s[i] == '+') {
        out += ' ';
      } else {
        out += s[i];
      }
    }
    return out;
  }

  static std::string get_post_param(const std::string &body, const std::string &key) {
    std::string search = key + "=";
    size_t pos = body.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = body.find('&', pos);
    std::string val = (end == std::string::npos) ? body.substr(pos) : body.substr(pos, end - pos);
    return url_decode(val);
  }

  static std::string get_entity_id_from_uri(const char *uri, const char *prefix) {
    const char *start = strstr(uri, prefix);
    if (!start) return "";
    start += strlen(prefix);
    const char *slash = strchr(start, '/');
    return slash ? std::string(start, slash - start) : std::string(start);
  }

  static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  }

  // ── Static handler functions ─────────────────────────────────────────────
  static esp_err_t h_root(httpd_req_t *req) {
    set_cors(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, reinterpret_cast<const char *>(WEB_UI_GZ), WEB_UI_GZ_LEN);
    return ESP_OK;
  }

  static esp_err_t h_setup(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, WIFI_SETUP_HTML, strlen(WIFI_SETUP_HTML));
    return ESP_OK;
  }

  static esp_err_t h_captive(httpd_req_t *req) {
    if (!wifi::global_wifi_component->is_connected()) {
      httpd_resp_set_status(req, "302 Found");
      httpd_resp_set_hdr(req, "Location", "/setup");
      httpd_resp_send(req, nullptr, 0);
    } else {
      httpd_resp_set_status(req, "204 No Content");
      httpd_resp_send(req, nullptr, 0);
    }
    return ESP_OK;
  }

  static esp_err_t h_state(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);
    JsonDocument doc;
    self->parent_->build_all_entities_json(doc);
    std::string out; serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out.c_str(), out.size());
    return ESP_OK;
  }

  static esp_err_t h_events(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);

    // Send SSE headers — keep socket open
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    // Send full state as first event
    JsonDocument doc;
    self->parent_->build_all_entities_json(doc);
    std::string payload; serializeJson(doc, payload);
    std::string initial = "event: full_state\ndata: " + payload + "\n\n";
    httpd_resp_send_chunk(req, initial.c_str(), initial.size());

    // Register this socket for future pushes
    int fd = httpd_req_to_sockfd(req);
    {
      std::lock_guard<std::mutex> lock(self->sse_mutex_);
      self->sse_clients_.push_back({fd});
    }

    // Block here — keep connection alive until client disconnects
    // Send a comment every 15s as heartbeat
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(15000));
      const char *ping = ": ping\n\n";
      int ret = httpd_socket_send(self->server_, fd, ping, strlen(ping), 0);
      if (ret < 0) break;  // client disconnected
    }

    // Clean up
    {
      std::lock_guard<std::mutex> lock(self->sse_mutex_);
      self->sse_clients_.remove_if([fd](const SseClient &c) { return c.fd == fd; });
    }
    return ESP_OK;
  }

  static esp_err_t h_wifi_scan(httpd_req_t *req) {
    set_cors(req);
    std::string json = "[";
    bool first = true;
    for (auto &net : wifi::global_wifi_component->get_scan_result()) {
      if (!first) json += ",";
      first = false;
      std::string ssid = net.get_ssid();
      // Escape quotes
      std::string escaped;
      for (char c : ssid) { if (c == '"') escaped += "\\\""; else escaped += c; }
      json += "{\"ssid\":\"" + escaped + "\",\"rssi\":" + std::to_string(net.get_rssi()) +
              ",\"secure\":" + (net.get_is_hidden() ? "false" : "true") + "}";
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
  }

  static esp_err_t h_wifi_connect(httpd_req_t *req) {
    std::string body = read_post_body(req);
    std::string ssid = get_post_param(body, "ssid");
    std::string pass = get_post_param(body, "password");
    if (ssid.empty()) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
      return ESP_OK;
    }
    wifi::WiFiAP sta{};
    sta.set_ssid(ssid.c_str());
    sta.set_password(pass.c_str());
    wifi::global_wifi_component->set_sta(sta);
    httpd_resp_send(req, "ok", 2);
    App.scheduler.set_timeout(wifi::global_wifi_component, "wifi_reboot", 500,
                              []() { App.safe_reboot(); });
    return ESP_OK;
  }

  // Generic command POST handler — entity id from URI, params from body
  static esp_err_t h_entity_cmd(httpd_req_t *req) {
    auto *self = static_cast<IDFBackend *>(req->user_ctx);
    set_cors(req);
    std::string uri(req->uri);
    std::string body = read_post_body(req);

    auto send_ok  = [&]() { httpd_resp_set_type(req, "application/json"); httpd_resp_send(req, "{\"ok\":true}", 11); };
    auto send_404 = [&]() { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); };

    if (uri.find("/api/switch/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/switch/");
#ifdef USE_SWITCH
      for (auto *sw : App.get_switches()) {
        if (WebServerCustom::make_id(sw->get_name()) == id) { sw->toggle(); send_ok(); return ESP_OK; }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/light/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/light/");
#ifdef USE_LIGHT
      for (auto *light : App.get_lights()) {
        if (WebServerCustom::make_id(light->get_name()) == id) {
          auto call = light->make_call();
          std::string state = get_post_param(body, "state");
          if (!state.empty()) call.set_state(state == "on" || state == "1" || state == "true");
          std::string bri = get_post_param(body, "brightness");
          if (!bri.empty()) call.set_brightness(std::stof(bri) / 255.0f);
          std::string ct = get_post_param(body, "color_temp");
          if (!ct.empty()) call.set_color_temperature(std::stof(ct));
          std::string r = get_post_param(body, "r"), g = get_post_param(body, "g"), b = get_post_param(body, "b");
          if (!r.empty() && !g.empty() && !b.empty())
            call.set_rgb(std::stof(r)/255.f, std::stof(g)/255.f, std::stof(b)/255.f);
          std::string eff = get_post_param(body, "effect");
          if (!eff.empty()) call.set_effect(eff.c_str());
          call.perform();
          send_ok(); return ESP_OK;
        }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/fan/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/fan/");
#ifdef USE_FAN
      for (auto *fan : App.get_fans()) {
        if (WebServerCustom::make_id(fan->get_name()) == id) {
          auto call = fan->make_call();
          std::string state = get_post_param(body, "state");
          if (!state.empty()) call.set_state(state == "on" || state == "1" || state == "true");
          std::string spd = get_post_param(body, "speed");
          if (!spd.empty()) call.set_speed(std::stoi(spd));
          call.perform(); send_ok(); return ESP_OK;
        }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/number/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/number/");
#ifdef USE_NUMBER
      for (auto *number : App.get_numbers()) {
        if (WebServerCustom::make_id(number->get_name()) == id) {
          std::string val = get_post_param(body, "value");
          if (!val.empty()) { auto call = number->make_call(); call.set_value(std::stof(val)); call.perform(); }
          send_ok(); return ESP_OK;
        }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/select/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/select/");
#ifdef USE_SELECT
      for (auto *select : App.get_selects()) {
        if (WebServerCustom::make_id(select->get_name()) == id) {
          std::string opt = get_post_param(body, "option");
          if (!opt.empty()) { auto call = select->make_call(); call.set_option(opt.c_str()); call.perform(); }
          send_ok(); return ESP_OK;
        }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/climate/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/climate/");
#ifdef USE_CLIMATE
      for (auto *climate : App.get_climates()) {
        if (WebServerCustom::make_id(climate->get_name()) == id) {
          auto call = climate->make_call();
          std::string mode = get_post_param(body, "mode");
          if (!mode.empty()) call.set_mode(mode.c_str());
          std::string temp = get_post_param(body, "target_temperature");
          if (!temp.empty()) call.set_target_temperature(std::stof(temp));
          call.perform(); send_ok(); return ESP_OK;
        }
      }
#endif
      send_404(); return ESP_OK;
    }

    if (uri.find("/api/button/") == 0) {
      std::string id = get_entity_id_from_uri(req->uri, "/api/button/");
#ifdef USE_BUTTON
      for (auto *button : App.get_buttons()) {
        if (WebServerCustom::make_id(button->get_name()) == id) { button->press(); send_ok(); return ESP_OK; }
      }
#endif
      send_404(); return ESP_OK;
    }

    send_404();
    return ESP_OK;
  }

  // ── Route registration ──────────────────────────────────────────────────
  void register_routes() {
    // Macro to register a handler cleanly
    auto reg = [&](const char *uri, httpd_method_t method, esp_err_t(*handler)(httpd_req_t *), void *ctx = nullptr) {
      httpd_uri_t h = {};
      h.uri      = uri;
      h.method   = method;
      h.handler  = handler;
      h.user_ctx = ctx ? ctx : static_cast<void *>(this);
      httpd_register_uri_handler(server_, &h);
    };

    reg("/",                       HTTP_GET,  h_root,        nullptr);
    reg("/setup",                  HTTP_GET,  h_setup,       nullptr);
    reg("/hotspot-detect.html",    HTTP_GET,  h_captive,     nullptr);
    reg("/generate_204",           HTTP_GET,  h_captive,     nullptr);
    reg("/ncsi.txt",               HTTP_GET,  h_captive,     nullptr);
    reg("/connecttest.txt",        HTTP_GET,  h_captive,     nullptr);
    reg("/redirect",               HTTP_GET,  h_captive,     nullptr);
    reg("/api/state",              HTTP_GET,  h_state);
    reg("/events",                 HTTP_GET,  h_events);
    reg("/api/wifi/scan",          HTTP_GET,  h_wifi_scan,   nullptr);
    reg("/api/wifi/connect",       HTTP_POST, h_wifi_connect, nullptr);
    // Wildcard catch-all for entity commands
    reg("/api/*",                  HTTP_POST, h_entity_cmd);
  }
};

IWebServer *make_idf_server(WebServerCustom *parent, uint16_t port) {
  (void)port;
  return new IDFBackend(parent);
}

}  // namespace web_server_custom
}  // namespace esphome

#endif  // USE_ESP_IDF or non-Arduino ESP32
