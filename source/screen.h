#pragma once

#include <string>
#include <vector>

// Minimal OSScreen text console. The same text is drawn to TV and GamePad.
namespace Screen {

bool init();
void shutdown();

// Draws a header, a scrollable body and a fixed footer, then flips.
// `scroll` is clamped to the valid range and written back.
void draw(const std::string &title,
          const std::vector<std::string> &body,
          const std::vector<std::string> &footer,
          int &scroll);

// Number of body rows visible at once.
int bodyRows();

} // namespace Screen
