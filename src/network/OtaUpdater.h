#pragma once

#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  std::string lastError;
  // Diagnostic heap readout for the last checkForUpdate() failure, drawn on its
  // own line so the error reason (line 1) stays within the narrow display.
  std::string lastMemoryInfo;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  const std::string& getLastError() const { return lastError; }
  const std::string& getLastMemoryInfo() const { return lastMemoryInfo; }
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
};
