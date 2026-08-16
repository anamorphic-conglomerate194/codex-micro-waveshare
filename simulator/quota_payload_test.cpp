#include <ArduinoJson.h>

#include <cassert>

#include "QuotaPayload.h"

namespace {

bool accepts(const char* json) {
  StaticJsonDocument<256> document;
  if (deserializeJson(document, json)) return false;
  quota_payload::Snapshot snapshot;
  return document.is<JsonObject>() &&
         quota_payload::parse(document.as<JsonObjectConst>(), snapshot);
}

}  // namespace

int main() {
  assert(accepts(R"({"remaining_percent":83.5,"reset_in_seconds":1200})"));
  assert(accepts(R"({"remaining_percent":0,"reset_in_seconds":0})"));
  assert(!accepts(R"({"remaining_percent":83.5})"));
  assert(!accepts(R"({"remaining_percent":"83.5","reset_in_seconds":1200})"));
  assert(!accepts(R"({"remaining_percent":83.5,"reset_in_seconds":"1200"})"));
  assert(!accepts(R"({"remaining_percent":83.5,"reset_in_seconds":-1})"));
  assert(!accepts(R"({"remaining_percent":-0.1,"reset_in_seconds":1200})"));
  assert(!accepts(R"({"remaining_percent":100.1,"reset_in_seconds":1200})"));
  assert(!accepts("not-json"));
}
