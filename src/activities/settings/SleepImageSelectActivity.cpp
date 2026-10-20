#include "SleepImageSelectActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SleepImageSelectActivity::loadImages() {
  imagePaths.clear();

  for (const char* directory : {"/sleep", "/.sleep"}) {
    auto dir = Storage.open(directory);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }

    char name[500];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }

      file.getName(name, sizeof(name));
      const std::string filename(name);
      if (filename.empty() || filename[0] == '.') {
        file.close();
        continue;
      }

      if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
        imagePaths.emplace_back(std::string(directory) + "/" + filename);
      }
      file.close();
    }
    dir.close();

    if (!imagePaths.empty()) {
      break;
    }
  }

  FsHelpers::sortFileList(imagePaths);
}

void SleepImageSelectActivity::onEnter() {
  Activity::onEnter();

  loadImages();
  selectedIndex = 0;
  for (int i = 0; i < static_cast<int>(imagePaths.size()); i++) {
    if (imagePaths[i] == SETTINGS.sleepScreenImage) {
      selectedIndex = i;
      break;
    }
  }
  requestUpdate();
}

void SleepImageSelectActivity::onExit() { Activity::onExit(); }

void SleepImageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int listSize = static_cast<int>(imagePaths.size());
  if (listSize == 0) {
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator.onNextRelease([this, listSize] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });
}

void SleepImageSelectActivity::handleSelection() {
  if (imagePaths.empty()) {
    return;
  }

  strncpy(SETTINGS.sleepScreenImage, imagePaths[selectedIndex].c_str(), sizeof(SETTINGS.sleepScreenImage) - 1);
  SETTINGS.sleepScreenImage[sizeof(SETTINGS.sleepScreenImage) - 1] = '\0';
  SETTINGS.saveToFile();
  finish();
}

std::string SleepImageSelectActivity::fileName(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void SleepImageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SLEEP_IMAGE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (imagePaths.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_NO_SLEEP_IMAGES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(imagePaths.size()), selectedIndex,
        [this](int index) { return fileName(imagePaths[index]); }, nullptr, nullptr,
        [this](int index) {
          return imagePaths[index] == SETTINGS.sleepScreenImage ? tr(STR_SELECTED) : std::string();
        },
        true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
