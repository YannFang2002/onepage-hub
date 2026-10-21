#pragma once

#include <cstdint>
#include <memory>

#include <I18n.h>

#include "components/icons/UIIcon.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// Identifies a top-level app launched from the desktop. The value is persisted
// in state.json (see CrossPointState::lastApp), so never renumber an existing
// entry — only append before None.
enum class AppId : uint8_t {
  Reader = 0,
  Settings = 1,
  FileTransfer = 2,
  Recorder = 3,
  FileManager = 4,
  None = 0xFF
};

// Where "go back to the top level" lands while this app is active.
//   Library: the reader app's own home (recent books / file browser), so the
//            reader keeps its existing back-to-library behaviour.
//   Desktop: every other app returns to the launcher.
enum class AppRoot : uint8_t { Desktop, Library };

// Static description of one desktop app. Adding an app means adding one entry
// to the kApps table in AppRegistry.cpp — nothing else in the launcher or in
// ActivityManager needs to change.
struct AppDescriptor {
  AppId id;
  StrId title;  // resolved with tr() at draw time
  UIIcon icon;
  AppRoot root;
  std::unique_ptr<Activity> (*create)(GfxRenderer& renderer, MappedInputManager& mappedInput);
};
