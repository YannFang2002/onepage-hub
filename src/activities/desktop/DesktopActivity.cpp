#include "DesktopActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "apps/AppRegistry.h"
#include "components/UITheme.h"

void DesktopActivity::onEnter() {
  Activity::onEnter();
  refreshAppCount();
  requestUpdate();
}

void DesktopActivity::refreshAppCount() {
  size_t count = 0;
  appList(count);
  appCount = static_cast<int>(count);
  if (selectorIndex >= appCount) {
    selectorIndex = appCount > 0 ? appCount - 1 : 0;
  }
}

void DesktopActivity::moveSelection(int delta) {
  if (appCount <= 0) {
    return;
  }
  int next = (selectorIndex + delta) % appCount;
  if (next < 0) {
    next += appCount;
  }
  if (next == selectorIndex) {
    return;
  }
  selectorIndex = next;
  requestUpdate();
}

void DesktopActivity::openSelected() {
  size_t count = 0;
  const AppDescriptor* apps = appList(count);
  if (selectorIndex < 0 || selectorIndex >= static_cast<int>(count)) {
    return;
  }
  activityManager.launchApp(apps[selectorIndex].id);
}

void DesktopActivity::loop() {
  if (appCount <= 0) {
    return;
  }

  // Front buttons move by column, side buttons move by row. Use the raw side
  // keys, NOT Button::NavNext/NavPrevious: those also resolve to the front
  // Right/Left keys (MappedInputManager::mapButton), so a single Left/Right
  // press would move one column AND one row at once.
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveSelection(1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveSelection(-1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveSelection(kColumns);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveSelection(-kColumns);
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
  }

  // Back on the desktop is intentionally a no-op: there is nowhere above it.
}

void DesktopActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DESKTOP));

  const int gridTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect grid{0, gridTop, pageWidth, gridBottom - gridTop};

  size_t count = 0;
  const AppDescriptor* apps = appList(count);

  GUI.drawAppGrid(
      renderer, grid, static_cast<int>(count), selectorIndex, kColumns,
      [apps](int index) { return std::string(I18N.get(apps[index].title)); },
      [apps](int index) { return apps[index].icon; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
