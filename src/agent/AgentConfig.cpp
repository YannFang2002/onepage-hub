#include "AgentConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

namespace {
constexpr char kConfigDir[] = "/system/.crosspoint";
constexpr char kConfigPath[] = "/system/.crosspoint/agent.json";
}  // namespace

std::string AgentConfig::normalizeBaseUrl(const std::string& input) {
  std::string url = input;
  while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n')) {
    url.pop_back();
  }
  while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) {
    url.erase(url.begin());
  }
  if (url.empty()) {
    return {};
  }
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    url = "http://" + url;
  }
  while (url.size() > 1 && url.back() == '/') {
    url.pop_back();
  }

  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    return {};
  }
  const size_t hostStart = scheme + 3;
  if (hostStart >= url.size()) {
    return {};
  }
  const size_t hostEnd = url.find_first_of("/:?#", hostStart);
  if (hostEnd == hostStart) {
    return {};
  }
  return url;
}

bool AgentConfig::load() {
  const String content = Storage.readFile(kConfigPath);
  if (content.isEmpty()) {
    baseUrl.clear();
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, content)) {
    LOG_ERR("AGENT", "agent.json parse failed");
    baseUrl.clear();
    return false;
  }
  baseUrl = normalizeBaseUrl(doc["baseUrl"] | "");
  return !baseUrl.empty();
}

bool AgentConfig::save() const {
  if (!Storage.ensureDirectoryExists(kConfigDir)) {
    LOG_ERR("AGENT", "cannot create %s", kConfigDir);
    return false;
  }

  JsonDocument doc;
  doc["baseUrl"] = baseUrl;
  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(kConfigPath, out)) {
    LOG_ERR("AGENT", "cannot write %s", kConfigPath);
    return false;
  }
  return true;
}
