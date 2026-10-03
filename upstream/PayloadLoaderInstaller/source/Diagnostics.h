#pragma once
#include <string>
#include <vector>

namespace Diagnostics {
void begin();
void record(const std::string &message);
void fail(const std::string &message);
const std::string &failure();
const std::vector<std::string> &lines();
// Only this function writes anything: appends a report to the SD card.
bool saveToSD();
}
