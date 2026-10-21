#pragma once

#include <string>

// Minimal persisted config for the Agent Remote app.
// Stored at /system/.crosspoint/agent.json.
struct AgentConfig {
  std::string baseUrl;  // normalized, e.g. "http://192.168.1.10:8765"

  bool load();
  bool save() const;

  static std::string normalizeBaseUrl(const std::string& input);
};
