#pragma once

#include "activities/Activity.h"

// Launcher screen: one tile per registered app (see AppRegistry.h).
// The desktop is a plain Activity — it owns no heap buffers and is destroyed
// like any other screen when an app is launched, which keeps the peak heap
// identical to the pre-desktop firmware.
class DesktopActivity final : public Activity {
 public:
  explicit DesktopActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Desktop", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int kColumns = 3;

  int selectorIndex = 0;
  int appCount = 0;

  void refreshAppCount();
  void moveSelection(int delta);
  void moveHorizontal(int delta);
  void moveVertical(int delta);
  void openSelected();
};
