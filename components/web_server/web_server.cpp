#if defined(ESP32) || defined(ESP8266)

#include "web_server.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/util.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace web_server_custom {

static const char *TAG = "web_server_custom";

std::string WebServerCustom::make_id(const std::string &name) {
  std::string id = name;
  for (char &c : id) {
    if (c == ' ' || c == '-') c = '_';
    else c = tolower((unsigned char)c);
  }
  return id;
}

static std::string safe_device_class(EntityBase *e) {
  char buf[48] = {};
  e->get_device_class_to(buf);
  return std::string(buf);
}

#ifdef USE_LIGHT
class LightChangeListener : public light::LightTargetStateReachedListener {
 public:
  explicit LightChangeListener(std::function<void()> cb) : cb_(std::move(cb)) {}
  void on_light_target_state_reached() override { cb_(); }
 private:
  std::function<void()> cb_;
};
#endif

// ---------------------------------------------------------------------------
// WiFi setup page (shown when device is in AP / fallback mode)
// ---------------------------------------------------------------------------
static const char WIFI_SETUP_HTML[] PROGMEM = R"RAW(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
background:#0f1117;color:#e8eaf6;min-height:100vh;
display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#1a1d27;border:1px solid #2e3245;border-radius:12px;
padding:28px 24px;width:100%;max-width:400px}
h1{font-size:20px;font-weight:600;margin-bottom:4px}
.sub{color:#8b90b8;font-size:13px;margin-bottom:24px}
label{display:block;font-size:13px;color:#8b90b8;margin-bottom:4px}
input,select{width:100%;padding:10px 12px;background:#232736;
border:1px solid #2e3245;border-radius:8px;color:#e8eaf6;
font-size:14px;margin-bottom:16px;outline:none}
input:focus,select:focus{border-color:#5b6ef5}
.btn{width:100%;padding:12px;background:#5b6ef5;border:none;
border-radius:8px;color:#fff;font-size:15px;font-weight:600;
cursor:pointer;transition:opacity .15s}
.btn:hover{opacity:.85}
.btn:disabled{opacity:.5;cursor:not-allowed}
#status{margin-top:12px;font-size:13px;text-align:center;
color:#8b90b8;min-height:20px}
#status.ok{color:#4ade80}
#status.err{color:#fb923c}
.scan-row{display:flex;gap:8px;margin-bottom:16px}
.scan-row select{margin-bottom:0;flex:1}
.scan-btn{padding:10px 14px;background:#232736;border:1px solid #2e3245;
border-radius:8px;color:#8b90b8;font-size:13px;cursor:pointer;
white-space:nowrap;transition:background .15s}
.scan-btn:hover{background:#2e3245}
</style>
</head>
<body>
<div class="card">
  <h1>WiFi Setup</h1>
  <div class="sub" id="dev-name">ESPHome device</div>

  <label>Network</label>
  <div class="scan-row">
    <select id="ssid-select" onchange="onSelect(this.value)">
      <option value="">— Select or type below —</option>
    </select>
    <button class="scan-btn" onclick="scan()">Scan</button>
  </div>

  <label>SSID</label>
  <input type="text" id="ssid" placeholder="Network name" autocomplete="off" autocorrect="off" autocapitalize="none">

  <label>Password</label>
  <input type="password" id="pass" placeholder="Leave blank if open">

  <button class="btn" id="submit-btn" onclick="doConnect()">Connect</button>
  <div id="status"></div>
</div>
<script>
fetch('/api/state').then(r=>r.json()).then(s=>{
  const n = s.friendly_name||s.device_name||'ESPHome device';
  document.getElementById('dev-name').textContent = n;
  document.title = 'WiFi Setup — '+n;
}).catch(()=>{});

function onSelect(v){ if(v) document.getElementById('ssid').value=v; }

async function scan(){
  const sel = document.getElementById('ssid-select');
  sel.innerHTML = '<option>Scanning…</option>';
  try {
    const r = await fetch('/api/wifi/scan');
    const nets = await r.json();
    sel.innerHTML = '<option value="">— Select network —</option>' +
      nets.map(n=>`<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.rssi} dBm)${n.secure?' 🔒':''}</option>`).join('');
  } catch(e) {
    sel.innerHTML = '<option value="">Scan failed</option>';
  }
}

async function doConnect(){
  const ssid = document.getElementById('ssid').value.trim();
  const pass = document.getElementById('pass').value;
  if(!ssid){ setStatus('Enter a network name','err'); return; }
  const btn = document.getElementById('submit-btn');
  btn.disabled = true;
  setStatus('Connecting…','');
  try {
    const fd = new FormData();
    fd.append('ssid', ssid);
    fd.append('password', pass);
    const r = await fetch('/api/wifi/connect', {method:'POST', body:fd});
    if(r.ok){
      setStatus('Saved! Device is rebooting to connect…','ok');
    } else {
      setStatus('Error: '+await r.text(),'err');
      btn.disabled = false;
    }
  } catch(e) {
    setStatus('Request failed','err');
    btn.disabled = false;
  }
}

function setStatus(msg, cls){
  const el = document.getElementById('status');
  el.textContent = msg;
  el.className = cls;
}

function esc(s){
  return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
</script>
</body>
</html>)RAW";

WebServerCustom::WebServerCustom() {}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void WebServerCustom::setup() {
  ESP_LOGCONFIG(TAG, "Setting up custom web server on port %u", port_);

  server_ = new AsyncWebServer(port_);
  events_ = new AsyncEventSource("/events");

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

  // ── Captive portal probe URLs — redirect to /setup when in AP mode ──────
  // iOS: /hotspot-detect.html, Android: /generate_204, Windows: /ncsi.txt
  auto captive_redirect = [](AsyncWebServerRequest *req) {
    if (!wifi::global_wifi_component->is_connected()) {
      req->redirect("http://" + req->host() + "/setup");
    } else {
      req->send(204);
    }
  };
  server_->on("/hotspot-detect.html", HTTP_GET, captive_redirect);
  server_->on("/generate_204",        HTTP_GET, captive_redirect);
  server_->on("/ncsi.txt",            HTTP_GET, captive_redirect);
  server_->on("/connecttest.txt",     HTTP_GET, captive_redirect);
  server_->on("/redirect",            HTTP_GET, captive_redirect);
  server_->on("/canonical.html",      HTTP_GET, captive_redirect);
  server_->on("/success.txt",         HTTP_GET, captive_redirect);

  // ── WiFi setup page ──────────────────────────────────────────────────────
  server_->on("/setup", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse_P(
        200, "text/html", (const uint8_t *)WIFI_SETUP_HTML, strlen_P(WIFI_SETUP_HTML));
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });

  // WiFi scan — returns JSON array of networks
  server_->on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    // Trigger a scan and return cached results
    // ESPHome's wifi component handles scanning internally;
    // we return what it has already scanned
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

  // WiFi connect — saves credentials and reboots
  server_->on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("ssid", true)) {
      req->send(400, "text/plain", "missing ssid");
      return;
    }
    String ssid = req->getParam("ssid", true)->value();
    String pass = req->hasParam("password", true)
                  ? req->getParam("password", true)->value()
                  : "";
    // Save to ESPHome's wifi component and schedule reboot
    // Store credentials via ESPHome preferences and reboot
    wifi::WiFiAP sta{};
    sta.set_ssid(ssid.c_str());
    sta.set_password(pass.c_str());
    wifi::global_wifi_component->set_sta(sta);
    req->send(200, "text/plain", "ok");
    App.scheduler.set_timeout(wifi::global_wifi_component, "wifi_reboot", 500, []() {
      App.safe_reboot();
    });
  });

  // ── SPA (main dashboard) ─────────────────────────────────────────────────
  server_->on("/", HTTP_GET, [this](AsyncWebServerRequest *req) {
    handle_index(req);
  });

  // ── REST API ─────────────────────────────────────────────────────────────
  server_->on("/api/state", HTTP_GET, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    handle_state(req);
  });

  server_->on("/api/switch", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/switch/");
    if (s < 0) { req->send(400); return; }
    String id = path.substring(s + 12);
    int slash = id.indexOf('/'); if (slash >= 0) id = id.substring(0, slash);
    handle_switch_toggle(req, id);
  });

  server_->on("/api/light", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/light/");
    if (s < 0) { req->send(400); return; }
    handle_light_set(req, path.substring(s + 11));
  });

  server_->on("/api/fan", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/fan/");
    if (s < 0) { req->send(400); return; }
    handle_fan_set(req, path.substring(s + 9));
  });

  server_->on("/api/number", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/number/");
    if (s < 0) { req->send(400); return; }
    handle_number_set(req, path.substring(s + 12));
  });

  server_->on("/api/select", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/select/");
    if (s < 0) { req->send(400); return; }
    handle_select_set(req, path.substring(s + 12));
  });

  server_->on("/api/climate", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/climate/");
    if (s < 0) { req->send(400); return; }
    handle_climate_set(req, path.substring(s + 13));
  });

  server_->on("/api/button", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!check_auth(req)) return;
    String path = req->url();
    int s = path.indexOf("/api/button/");
    if (s < 0) { req->send(400); return; }
    handle_button_press(req, path.substring(s + 12));
  });

  server_->on("/", HTTP_OPTIONS, [](AsyncWebServerRequest *req) { req->send(204); });

  // ── SSE ──────────────────────────────────────────────────────────────────
  events_->onConnect([this](AsyncEventSourceClient *client) {
    ESP_LOGD(TAG, "SSE client connected");
    send_full_state(client);
  });
  server_->addHandler(events_);

  // ── 404: /api/* → JSON, everything else → plain 404 ─────────────────────
  server_->onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/api/"))
      req->send(404, "application/json", "{\"error\":\"not found\"}");
    else
      req->send(404);
  });

  // ── Entity callbacks → SSE push ──────────────────────────────────────────
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal()) continue;
    sw->add_on_state_callback([this, sw](bool) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_switch_json(obj, sw);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    if (light->is_internal()) continue;
    auto *listener = new LightChangeListener([this, light]() {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_light_json(obj, light);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
    light->add_target_state_reached_listener(listener);
  }
#endif

#ifdef USE_SENSOR
  for (auto *sensor : App.get_sensors()) {
    if (sensor->is_internal()) continue;
    sensor->add_on_state_callback([this, sensor](float) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_sensor_json(obj, sensor);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_BINARY_SENSOR
  for (auto *bs : App.get_binary_sensors()) {
    if (bs->is_internal()) continue;
    bs->add_on_state_callback([this, bs](bool) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_binary_sensor_json(obj, bs);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

#ifdef USE_TEXT_SENSOR
  for (auto *ts : App.get_text_sensors()) {
    if (ts->is_internal()) continue;
    ts->add_on_state_callback([this, ts](const std::string &) {
      JsonDocument doc;
      JsonObject obj = doc.to<JsonObject>();
      build_text_sensor_json(obj, ts);
      String out; serializeJson(doc, out);
      events_->send(out.c_str(), "state_change", millis());
    });
  }
#endif

  server_->begin();
  ESP_LOGCONFIG(TAG, "Web server started on port %u", port_);
}

void WebServerCustom::loop() {}

void WebServerCustom::dump_config() {
  ESP_LOGCONFIG(TAG, "Custom Web Server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  Auth: %s", auth_user_.empty() ? "disabled" : "enabled");
}

bool WebServerCustom::check_auth(AsyncWebServerRequest *request) {
  if (auth_user_.empty()) return true;
  if (!request->authenticate(auth_user_.c_str(), auth_pass_.c_str())) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

void WebServerCustom::handle_index(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginResponse_P(
      200, "text/html", WEB_UI_GZ, WEB_UI_GZ_LEN);
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
}

void WebServerCustom::handle_state(AsyncWebServerRequest *request) {
  JsonDocument doc;
  build_all_entities_json(doc);
  String output; serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WebServerCustom::send_full_state(AsyncEventSourceClient *client) {
  JsonDocument doc;
  build_all_entities_json(doc);
  String payload; serializeJson(doc, payload);
  client->send(payload.c_str(), "full_state", millis());
}

void WebServerCustom::handle_switch_toggle(AsyncWebServerRequest *request,
                                            const String &entity_id) {
#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (make_id(sw->get_name()) == entity_id.c_str()) {
      sw->toggle();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_light_set(AsyncWebServerRequest *request,
                                        const String &entity_id) {
#ifdef USE_LIGHT
  for (auto *light : App.get_lights()) {
    if (make_id(light->get_name()) == entity_id.c_str()) {
      auto call = light->make_call();
      if (request->hasParam("state", true)) {
        String v = request->getParam("state", true)->value();
        call.set_state(v == "on" || v == "1" || v == "true");
      }
      if (request->hasParam("brightness", true))
        call.set_brightness(request->getParam("brightness", true)->value().toFloat() / 255.0f);
      if (request->hasParam("color_temp", true))
        call.set_color_temperature(request->getParam("color_temp", true)->value().toFloat());
      if (request->hasParam("r", true) && request->hasParam("g", true) && request->hasParam("b", true))
        call.set_rgb(request->getParam("r", true)->value().toFloat() / 255.0f,
                     request->getParam("g", true)->value().toFloat() / 255.0f,
                     request->getParam("b", true)->value().toFloat() / 255.0f);
      if (request->hasParam("effect", true))
        call.set_effect(request->getParam("effect", true)->value().c_str());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_fan_set(AsyncWebServerRequest *request,
                                      const String &entity_id) {
#ifdef USE_FAN
  for (auto *fan : App.get_fans()) {
    if (make_id(fan->get_name()) == entity_id.c_str()) {
      auto call = fan->make_call();
      if (request->hasParam("state", true)) {
        String v = request->getParam("state", true)->value();
        call.set_state(v == "on" || v == "1" || v == "true");
      }
      if (request->hasParam("speed", true))
        call.set_speed(request->getParam("speed", true)->value().toInt());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_number_set(AsyncWebServerRequest *request,
                                         const String &entity_id) {
#ifdef USE_NUMBER
  for (auto *number : App.get_numbers()) {
    if (make_id(number->get_name()) == entity_id.c_str()) {
      if (request->hasParam("value", true)) {
        auto call = number->make_call();
        call.set_value(request->getParam("value", true)->value().toFloat());
        call.perform();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_select_set(AsyncWebServerRequest *request,
                                         const String &entity_id) {
#ifdef USE_SELECT
  for (auto *select : App.get_selects()) {
    if (make_id(select->get_name()) == entity_id.c_str()) {
      if (request->hasParam("option", true)) {
        auto call = select->make_call();
        call.set_option(request->getParam("option", true)->value().c_str());
        call.perform();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_climate_set(AsyncWebServerRequest *request,
                                          const String &entity_id) {
#ifdef USE_CLIMATE
  for (auto *climate : App.get_climates()) {
    if (make_id(climate->get_name()) == entity_id.c_str()) {
      auto call = climate->make_call();
      if (request->hasParam("mode", true))
        call.set_mode(request->getParam("mode", true)->value().c_str());
      if (request->hasParam("target_temperature", true))
        call.set_target_temperature(
            request->getParam("target_temperature", true)->value().toFloat());
      call.perform();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

void WebServerCustom::handle_button_press(AsyncWebServerRequest *request,
                                           const String &entity_id) {
#ifdef USE_BUTTON
  for (auto *button : App.get_buttons()) {
    if (make_id(button->get_name()) == entity_id.c_str()) {
      button->press();
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
#endif
  request->send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------
void WebServerCustom::build_all_entities_json(JsonDocument &doc) {
  JsonObject root = doc.to<JsonObject>();
  root["device_name"]     = App.get_name().c_str();
  root["friendly_name"]   = App.get_friendly_name().c_str();
  root["esphome_version"] = ESPHOME_VERSION;
  root["uptime"]          = millis() / 1000;

#ifdef USE_SWITCH
  JsonArray switches = root["switches"].to<JsonArray>();
  for (auto *sw : App.get_switches()) {
    if (sw->is_internal()) continue;
    build_switch_json(switches.add<JsonObject>(), sw);
  }
#endif
#ifdef USE_LIGHT
  JsonArray lights = root["lights"].to<JsonArray>();
  for (auto *light : App.get_lights()) {
    if (light->is_internal()) continue;
    build_light_json(lights.add<JsonObject>(), light);
  }
#endif
#ifdef USE_SENSOR
  JsonArray sensors = root["sensors"].to<JsonArray>();
  for (auto *sensor : App.get_sensors()) {
    if (sensor->is_internal()) continue;
    build_sensor_json(sensors.add<JsonObject>(), sensor);
  }
#endif
#ifdef USE_BINARY_SENSOR
  JsonArray binary_sensors = root["binary_sensors"].to<JsonArray>();
  for (auto *bs : App.get_binary_sensors()) {
    if (bs->is_internal()) continue;
    build_binary_sensor_json(binary_sensors.add<JsonObject>(), bs);
  }
#endif
#ifdef USE_TEXT_SENSOR
  JsonArray text_sensors = root["text_sensors"].to<JsonArray>();
  for (auto *ts : App.get_text_sensors()) {
    if (ts->is_internal()) continue;
    build_text_sensor_json(text_sensors.add<JsonObject>(), ts);
  }
#endif
#ifdef USE_CLIMATE
  JsonArray climates = root["climates"].to<JsonArray>();
  for (auto *climate : App.get_climates()) {
    if (climate->is_internal()) continue;
    build_climate_json(climates.add<JsonObject>(), climate);
  }
#endif
#ifdef USE_FAN
  JsonArray fans = root["fans"].to<JsonArray>();
  for (auto *fan : App.get_fans()) {
    if (fan->is_internal()) continue;
    build_fan_json(fans.add<JsonObject>(), fan);
  }
#endif
#ifdef USE_NUMBER
  JsonArray numbers = root["numbers"].to<JsonArray>();
  for (auto *number : App.get_numbers()) {
    if (number->is_internal()) continue;
    build_number_json(numbers.add<JsonObject>(), number);
  }
#endif
#ifdef USE_SELECT
  JsonArray selects = root["selects"].to<JsonArray>();
  for (auto *select : App.get_selects()) {
    if (select->is_internal()) continue;
    build_select_json(selects.add<JsonObject>(), select);
  }
#endif
#ifdef USE_BUTTON
  JsonArray buttons = root["buttons"].to<JsonArray>();
  for (auto *button : App.get_buttons()) {
    if (button->is_internal()) continue;
    JsonObject obj = buttons.add<JsonObject>();
    obj["id"]   = make_id(button->get_name()).c_str();
    obj["name"] = button->get_name().c_str();
    obj["type"] = "button";
  }
#endif
}

#ifdef USE_SWITCH
void WebServerCustom::build_switch_json(JsonObject obj, switch_::Switch *sw) {
  obj["id"] = make_id(sw->get_name()).c_str(); obj["name"] = sw->get_name().c_str();
  obj["type"] = "switch"; obj["state"] = sw->state;
}
#endif

#ifdef USE_LIGHT
void WebServerCustom::build_light_json(JsonObject obj, light::LightState *light) {
  obj["id"] = make_id(light->get_name()).c_str(); obj["name"] = light->get_name().c_str();
  obj["type"] = "light";
  auto values = light->current_values;
  obj["state"] = values.is_on(); obj["brightness"] = (int)(values.get_brightness() * 255);
  auto traits = light->get_traits();
  if (traits.supports_color_mode(light::ColorMode::COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::COLD_WARM_WHITE)) {
    obj["color_temp"] = values.get_color_temperature();
    obj["min_mireds"] = traits.get_min_mireds();
    obj["max_mireds"] = traits.get_max_mireds();
  }
  if (traits.supports_color_mode(light::ColorMode::RGB) ||
      traits.supports_color_mode(light::ColorMode::RGB_WHITE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLOR_TEMPERATURE) ||
      traits.supports_color_mode(light::ColorMode::RGB_COLD_WARM_WHITE)) {
    obj["r"] = (int)(values.get_red() * 255);
    obj["g"] = (int)(values.get_green() * 255);
    obj["b"] = (int)(values.get_blue() * 255);
  }
  JsonArray effects = obj["effects"].to<JsonArray>();
  for (auto &effect : light->get_effects()) effects.add(effect->get_name().c_str());
  obj["effect"] = light->get_effect_name().c_str();
}
#endif

#ifdef USE_SENSOR
void WebServerCustom::build_sensor_json(JsonObject obj, sensor::Sensor *sensor) {
  obj["id"] = make_id(sensor->get_name()).c_str(); obj["name"] = sensor->get_name().c_str();
  obj["type"] = "sensor"; obj["unit"] = sensor->get_unit_of_measurement().c_str();
  obj["device_class"] = safe_device_class(sensor).c_str();
  if (sensor->has_state()) obj["state"] = sensor->get_state(); else obj["state"] = nullptr;
}
#endif

#ifdef USE_BINARY_SENSOR
void WebServerCustom::build_binary_sensor_json(JsonObject obj, binary_sensor::BinarySensor *bs) {
  obj["id"] = make_id(bs->get_name()).c_str(); obj["name"] = bs->get_name().c_str();
  obj["type"] = "binary_sensor"; obj["device_class"] = safe_device_class(bs).c_str();
  obj["state"] = bs->state;
}
#endif

#ifdef USE_TEXT_SENSOR
void WebServerCustom::build_text_sensor_json(JsonObject obj, text_sensor::TextSensor *ts) {
  obj["id"] = make_id(ts->get_name()).c_str(); obj["name"] = ts->get_name().c_str();
  obj["type"] = "text_sensor"; obj["device_class"] = safe_device_class(ts).c_str();
  obj["state"] = ts->get_state().c_str();
}
#endif

#ifdef USE_CLIMATE
void WebServerCustom::build_climate_json(JsonObject obj, climate::Climate *climate) {
  obj["id"] = make_id(climate->get_name()).c_str(); obj["name"] = climate->get_name().c_str();
  obj["type"] = "climate"; obj["mode"] = climate::climate_mode_to_string(climate->mode);
  obj["current_temperature"] = climate->current_temperature;
  obj["target_temperature"]  = climate->target_temperature;
  obj["action"] = climate::climate_action_to_string(climate->action);
}
#endif

#ifdef USE_FAN
void WebServerCustom::build_fan_json(JsonObject obj, fan::Fan *fan) {
  obj["id"] = make_id(fan->get_name()).c_str(); obj["name"] = fan->get_name().c_str();
  obj["type"] = "fan"; obj["state"] = fan->state;
  if (fan->get_traits().supports_speed()) {
    obj["speed"] = fan->speed; obj["speed_count"] = fan->get_traits().supported_speed_count();
  }
  obj["oscillating"] = fan->oscillating;
}
#endif

#ifdef USE_NUMBER
void WebServerCustom::build_number_json(JsonObject obj, number::Number *number) {
  obj["id"] = make_id(number->get_name()).c_str(); obj["name"] = number->get_name().c_str();
  obj["type"] = "number"; obj["min"] = number->traits.get_min_value();
  obj["max"] = number->traits.get_max_value(); obj["step"] = number->traits.get_step();
  obj["unit"] = number->traits.get_unit_of_measurement().c_str();
  if (number->has_state()) obj["state"] = number->state; else obj["state"] = nullptr;
}
#endif

#ifdef USE_SELECT
void WebServerCustom::build_select_json(JsonObject obj, select::Select *select) {
  obj["id"] = make_id(select->get_name()).c_str(); obj["name"] = select->get_name().c_str();
  obj["type"] = "select";
  JsonArray options = obj["options"].to<JsonArray>();
  for (auto &opt : select->traits.get_options()) options.add(opt.c_str());
  obj["state"] = select->state.c_str();
}
#endif

}  // namespace web_server_custom
}  // namespace esphome

#endif
