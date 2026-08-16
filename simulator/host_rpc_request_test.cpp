#include <ArduinoJson.h>

#include <cassert>

#include "HostRpcRequest.h"

namespace {

host_rpc::Method classify(const char* json) {
  StaticJsonDocument<256> request;
  if (deserializeJson(request, json) || !request.is<JsonObjectConst>()) {
    return host_rpc::Method::Unsupported;
  }
  return host_rpc::classify(request.as<JsonObjectConst>());
}

}  // namespace

int main() {
  using host_rpc::Method;

  assert(classify(R"({"method":"sys.version","id":1})") ==
         Method::SystemVersion);
  assert(classify(R"({"method":"device.status","params":{},"id":2})") ==
         Method::DeviceStatus);
  assert(classify(R"({"method":"v.oai.thstatus","params":[],"id":3})") ==
         Method::ThreadStatus);
  assert(classify(R"({"method":"v.oai.rgbcfg","params":{},"id":4})") ==
         Method::RgbConfig);
  assert(classify(R"({"method":"lights.preview","id":5})") ==
         Method::LightsPreview);
  assert(classify(R"({"method":"host.focused_app","id":6})") ==
         Method::HostFocusedApp);

  // A parseable method name is not enough to establish trusted host activity.
  assert(classify(R"({"method":"attacker.probe","params":{},"id":7})") ==
         Method::Unsupported);
  assert(classify(R"({"method":"v.oai.thstatus","params":{},"id":8})") ==
         Method::Unsupported);
  assert(classify(R"({"method":"v.oai.rgbcfg","params":[],"id":9})") ==
         Method::Unsupported);
  assert(classify(R"({"method":42,"params":{},"id":10})") ==
         Method::Unsupported);
  assert(classify(R"(["device.status"])") == Method::Unsupported);
}
