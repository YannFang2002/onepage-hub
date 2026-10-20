#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SleepImageSelectActivity final : public Activity {
 public:
  explicit SleepImageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SleepImageSelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void loadImages();
  void handleSelection();
  static std::string fileName(const std::string& path);

  ButtonNavigator buttonNavigator;
  std::vector<std::string> imagePaths;
  int selectedIndex = 0;
};
