#pragma once

#include <string>
#include <vector>

// Minimal persisted config for the Agent Remote app.
// Stored at /system/.crosspoint/agent.json.
struct AgentConfig {
  std::string baseUrl;  // normalized, e.g. "http://192.168.1.10:8765"
  // Hosts seen before (discovered or typed), newest first, so the menu can
  // switch between several machines without retyping addresses.
  std::vector<std::string> hosts;

  bool load();
  bool save() const;

  // Move-to-front, deduplicate, cap the list, and persist.
  void rememberHost(const std::string& url);

  static std::string normalizeBaseUrl(const std::string& input);
};
