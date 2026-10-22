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
// GET  {baseUrl}/api/v1/heartbeat
class AgentClient {
 public:
  static bool fetchStatus(const std::string& baseUrl, AgentStatus& out, std::string& error);
  static bool sendText(const std::string& baseUrl, const std::string& text, std::string& reply, std::string& error);
  static bool transcribe(const std::string& baseUrl, const std::string& wav, std::string& text, std::string& error);
  // Liveness ping. The host treats a device as online only while it keeps
  // talking, so the app sends this every ~20 s while it is open. Deliberately
  // short-timeout: a sleeping device must not stall the UI.
  static bool heartbeat(const std::string& baseUrl);
};
