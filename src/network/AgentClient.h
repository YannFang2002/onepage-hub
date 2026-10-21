#pragma once

#include <string>

struct AgentStatus {
  bool ok = false;
  std::string agent;
  std::string repo;
  std::string state;  // idle / running / error / unknown
  std::string lastReply;
};

// Tiny HTTP client for the Agent Remote MVP.
// GET  {baseUrl}/api/v1/status
// POST {baseUrl}/api/v1/send  {"text":"..."}
class AgentClient {
 public:
  static bool fetchStatus(const std::string& baseUrl, AgentStatus& out, std::string& error);
  static bool sendText(const std::string& baseUrl, const std::string& text, std::string& reply, std::string& error);
  static bool transcribe(const std::string& baseUrl, const std::string& wav, std::string& text, std::string& error);
};
