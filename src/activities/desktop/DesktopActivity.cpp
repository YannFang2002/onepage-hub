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

// Left/Right wrap inside the current row only; Up/Down wrap inside the current
// column only. The old linear +/-1 and +/-kColumns walked the whole list, so a
// press at a row/column edge jumped into a different row/column.
void DesktopActivity::moveHorizontal(int delta) {
  if (appCount <= 0) {
    return;
  }
  // Left/Right walk the whole grid in row-major order and wrap at the end:
  // 1 2 3 4 5 6 7 8 9 -> 1.
  int next = ((selectorIndex + delta) % appCount + appCount) % appCount;
  if (next == selectorIndex) {
    return;
  }
  selectorIndex = next;
  requestUpdate();
}

void DesktopActivity::moveVertical(int delta) {
  if (appCount <= 0) {
    return;
  }
  // Up/Down walk the grid in column-major order and wrap at the end:
  // 1 4 7 2 5 8 3 6 9 -> 1. Cells missing from a partial last row are skipped.
  const int rows = (appCount + kColumns - 1) / kColumns;
  int order[kColumns * 16];
  int total = 0;
  for (int column = 0; column < kColumns; ++column) {
    for (int row = 0; row < rows; ++row) {
      const int cell = row * kColumns + column;
      if (cell < appCount && total < static_cast<int>(sizeof(order) / sizeof(order[0]))) {
        order[total++] = cell;
      }
    }
  }
  if (total <= 1) {
    return;
  }
  int position = 0;
  for (int i = 0; i < total; ++i) {
    if (order[i] == selectorIndex) {
      position = i;
      break;
    }
  }
  position = ((position + delta) % total + total) % total;
  if (order[position] != selectorIndex) {
    selectorIndex = order[position];
    requestUpdate();
  }
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
    moveHorizontal(1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveHorizontal(-1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveVertical(1);
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveVertical(-1);
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

  // Front keys move across columns: label them so Left/Right are visible in the
  // hint row (previously only "Select" was rendered, leaving both slots blank).
  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
