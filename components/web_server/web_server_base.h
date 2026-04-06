#pragma once
// ---------------------------------------------------------------------------
// web_server_base.h — compatibility shim for the custom web_server component
//
// ESPHome's built-in web_server_base provides AsyncWebServer management and
// shares one server instance across components. This shim documents what the
// custom component expects so you can either:
//   (a) declare a dependency on the upstream web_server_base component, or
//   (b) include AsyncWebServer directly and manage the server yourself.
//
// The custom component manages its own AsyncWebServer instance, so option (b)
// is the default — just ensure AsyncTCP and ESPAsyncWebServer are available
// in your platform libraries.
// ---------------------------------------------------------------------------

// Nothing to declare here for the self-hosted mode.
// If you want to share the server with other components (e.g. captive_portal),
// uncomment the lines below and add "web_server_base" to DEPENDENCIES in
// __init__.py, then inject the base server via a set_base() call.

/*
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace web_server_custom {

inline AsyncWebServer *get_server_from_base(
    web_server_base::WebServerBase *base) {
  return base->get_server();
}

}  // namespace web_server_custom
}  // namespace esphome
*/
