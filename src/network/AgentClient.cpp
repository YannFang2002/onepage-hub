#include "AgentClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstring>
#include <algorithm>

namespace {

constexpr int kTimeoutMs = 8000;
constexpr int kRxBuffer = 4096;
constexpr int kTxBuffer = 2048;

std::string truncateUtf8(const std::string& input, size_t maxChars) {
  std::string collapsed;
  collapsed.reserve(std::min<size_t>(input.size(), maxChars * 4));
  bool lastSpace = false;
  for (char c : input) {
    const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
    if (space) {
      if (!lastSpace && !collapsed.empty()) {
        collapsed.push_back(' ');
      }
    } else {
      collapsed.push_back(c);
    }
    lastSpace = space;
  }
  while (!collapsed.empty() && collapsed.back() == ' ') {
    collapsed.pop_back();
  }

  size_t chars = 0;
  size_t offset = 0;
  while (offset < collapsed.size() && chars < maxChars) {
    const unsigned char c = static_cast<unsigned char>(collapsed[offset]);
    size_t length = 1;
    if ((c & 0x80) == 0) {
      length = 1;
    } else if ((c & 0xE0) == 0xC0) {
      length = 2;
    } else if ((c & 0xF0) == 0xE0) {
      length = 3;
    } else if ((c & 0xF8) == 0xF0) {
      length = 4;
    }
    if (offset + length > collapsed.size()) {
      break;
    }
    offset += length;
    ++chars;
  }
  if (offset >= collapsed.size()) {
    return collapsed;
  }
  return collapsed.substr(0, offset) + "…";
}

bool getBody(const std::string& url, std::string& response, std::string& error, int timeoutMs) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = timeoutMs;
  config.buffer_size = kRxBuffer;
  config.buffer_size_tx = kTxBuffer;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    error = "client_init";
    return false;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("AGENT", "GET open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    error = "network";
    return false;
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    LOG_ERR("AGENT", "GET HTTP %d", status);
    esp_http_client_cleanup(client);
    error = "http_" + std::to_string(status);
    return false;
  }

  char buffer[512];
  int len = 0;
  while ((len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, static_cast<size_t>(len));
  }
  esp_http_client_cleanup(client);
  return true;
}

std::string endpoint(const std::string& baseUrl, const char* path) {
  if (baseUrl.empty()) {
    return {};
  }
  std::string url = baseUrl;
  if (url.back() == '/') {
    url.pop_back();
  }
  url += path;
  return url;
}

bool postBody(const std::string& url, const char* contentType, const std::string& body, std::string& response,
              std::string& error, int timeoutMs) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = timeoutMs;
  config.buffer_size = kRxBuffer;
  config.buffer_size_tx = kTxBuffer;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    error = "client_init";
    return false;
  }

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", contentType);
  esp_http_client_set_header(client, "User-Agent", "OnePage-Agent-Remote");

  esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
  if (err != ESP_OK) {
    LOG_ERR("AGENT", "POST open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    error = "network";
    return false;
  }

  size_t offset = 0;
  while (offset < body.size()) {
    const size_t chunk = std::min<size_t>(4096, body.size() - offset);
    const int written = esp_http_client_write(client, body.data() + offset, static_cast<int>(chunk));
    if (written <= 0) {
      LOG_ERR("AGENT", "POST write failed: %d", written);
      esp_http_client_cleanup(client);
      error = "network";
      return false;
    }
    offset += static_cast<size_t>(written);
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    LOG_ERR("AGENT", "POST HTTP %d", status);
    esp_http_client_cleanup(client);
    error = "http_" + std::to_string(status);
    return false;
  }

  char buffer[512];
  int len = 0;
  while ((len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
    response.append(buffer, static_cast<size_t>(len));
  }
  esp_http_client_cleanup(client);
  return true;
}

}  // namespace

bool AgentClient::heartbeat(const std::string& baseUrl) {
  const std::string url = endpoint(baseUrl, "/api/v1/heartbeat");
  if (url.empty()) {
    return false;
  }
  std::string response;
  std::string error;
  // 2 s cap: this runs from the UI loop, so it must never hold the frame for
  // long when the host has gone away.
  return getBody(url, response, error, 2000) && response.find("\"ok\"") != std::string::npos;
}

bool AgentClient::fetchStatus(const std::string& baseUrl, AgentStatus& out, std::string& error,
                              int timeoutMs) {
  out = {};
  error.clear();

  const std::string url = endpoint(baseUrl, "/api/v1/status");
  if (url.empty()) {
    error = "not_configured";
    return false;
  }

  std::string content;
  if (!getBody(url, content, error, timeoutMs)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, content);
  if (parseError) {
    LOG_ERR("AGENT", "status JSON parse failed: %s", parseError.c_str());
    error = "bad_json";
    return false;
  }

  out.ok = doc["ok"] | false;
  out.agent = doc["agent"] | "";
  out.repo = truncateUtf8(doc["repo"] | "", 40);
  out.state = doc["state"] | "unknown";
  out.lastReply = truncateUtf8(doc["lastReply"] | "", 80);

  if (!out.ok) {
    const char* serverError = doc["error"] | "server_error";
    error = serverError;
    return false;
  }
  return true;
}

bool AgentClient::sendText(const std::string& baseUrl, const std::string& text, std::string& reply, std::string& error) {
  reply.clear();
  error.clear();

  const std::string url = endpoint(baseUrl, "/api/v1/send");
  if (url.empty()) {
    error = "not_configured";
    return false;
  }

  JsonDocument request;
  request["text"] = text;
  String body;
  serializeJson(request, body);

  std::string response;
  if (!postBody(url, "application/json", body.c_str(), response, error, kTimeoutMs)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, response);
  if (parseError) {
    LOG_ERR("AGENT", "send JSON parse failed: %s", parseError.c_str());
    error = "bad_json";
    return false;
  }

  if (!(doc["ok"] | false)) {
    const char* serverError = doc["error"] | "server_error";
    error = serverError;
    return false;
  }

  reply = truncateUtf8(doc["reply"] | "", 80);
  return true;
}

bool AgentClient::transcribe(const std::string& baseUrl, const std::string& wav, std::string& text,
                             std::string& error) {
  text.clear();
  error.clear();

  const std::string url = endpoint(baseUrl, "/api/v1/voice");
  if (url.empty()) {
    error = "not_configured";
    return false;
  }
  if (wav.empty()) {
    error = "empty_audio";
    return false;
  }

  std::string response;
  if (!postBody(url, "audio/wav", wav, response, error, 15000)) {
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, response);
  if (parseError) {
    LOG_ERR("AGENT", "voice JSON parse failed: %s", parseError.c_str());
    error = "bad_json";
    return false;
  }

  if (!(doc["ok"] | false)) {
    const char* serverError = doc["error"] | "server_error";
    error = serverError;
    return false;
  }

  text = truncateUtf8(doc["text"] | "", 80);
  if (text.empty()) {
    error = "empty_transcript";
    return false;
  }
  return true;
}
