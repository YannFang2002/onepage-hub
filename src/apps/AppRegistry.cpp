#include "AppRegistry.h"

#include "activities/agent_remote/AgentRemoteActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/recorder/RecorderActivity.h"
#include "activities/settings/SettingsActivity.h"

namespace {

// App factories. Each one only needs the two services every activity already
// takes, so adding an app never touches ActivityManager or the desktop.
std::unique_ptr<Activity> createReader(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<HomeActivity>(renderer, mappedInput);
}

std::unique_ptr<Activity> createSettings(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<SettingsActivity>(renderer, mappedInput);
}

std::unique_ptr<Activity> createFileTransfer(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput);
}

std::unique_ptr<Activity> createRecorder(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<RecorderActivity>(renderer, mappedInput);
}

// On-device file manager: the same browser the reader uses, but rooted at the SD
// card and unfiltered, so recordings, fonts, /system and loose files are all
// visible and deletable without a phone or the web UI.
std::unique_ptr<Activity> createFileManager(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::Manager);
}

std::unique_ptr<Activity> createAgentRemote(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return std::make_unique<AgentRemoteActivity>(renderer, mappedInput);
}


// Display order == tile order. To add an app:
//   1. append a value to AppId (never renumber existing ones)
//   2. add the factory above (if the app does not exist yet)
//   3. add one row here
// Nothing else in the launcher needs to change.
constexpr AppDescriptor kApps[] = {
    {AppId::Reader, StrId::STR_APP_READER, UIIcon::Book, AppRoot::Library, &createReader},
    {AppId::Settings, StrId::STR_SETTINGS_TITLE, UIIcon::Settings, AppRoot::Desktop, &createSettings},
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, AppRoot::Desktop, &createFileTransfer},
    {AppId::Recorder, StrId::STR_VOICE_RECORDER, UIIcon::Mic, AppRoot::Desktop, &createRecorder},
    {AppId::FileManager, StrId::STR_FILE_MANAGER, UIIcon::Folder, AppRoot::Desktop, &createFileManager},
    {AppId::AgentRemote, StrId::STR_AGENT_REMOTE, UIIcon::Agent, AppRoot::Desktop, &createAgentRemote},
};

}  // namespace

const AppDescriptor* appList(size_t& count) {
  count = sizeof(kApps) / sizeof(kApps[0]);
  return kApps;
}

const AppDescriptor* findApp(AppId id) {
  for (const AppDescriptor& app : kApps) {
    if (app.id == id) {
      return &app;
    }
  }
  return nullptr;
}
