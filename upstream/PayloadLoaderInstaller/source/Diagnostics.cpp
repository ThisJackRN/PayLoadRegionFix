#include "Diagnostics.h"
#include <cstdio>
#include <whb/sdcard.h>

namespace {
bool collecting = false;
std::vector<std::string> report;
std::string firstFailure;
}

void Diagnostics::begin() {
    report.clear();
    firstFailure.clear();
    collecting = true;
    record("PLI RegionFix Diagnostic v2 (runtime mount fix)");
    record("Read-only console checks; no title/config writes.");
}

void Diagnostics::record(const std::string &message) {
    if (collecting) report.push_back(message);
}

void Diagnostics::fail(const std::string &message) {
    if (firstFailure.empty()) firstFailure = message;
    record("FAIL: " + message);
}

const std::string &Diagnostics::failure() { return firstFailure; }
const std::vector<std::string> &Diagnostics::lines() { return report; }

bool Diagnostics::saveToSD() {
    if (!WHBMountSdCard()) return false;
    bool ok = false;
    const char *mount = WHBGetSdCardMountPath();
    if (mount) {
        const auto path = std::string(mount) + "/regionfix-pli-diagnostic.log";
        // Append: preserve earlier runs and never touch regionfix_backup.
        FILE *file = std::fopen(path.c_str(), "ab");
        if (file) {
            ok = std::fprintf(file, "\n===== PLI RegionFix Diagnostic v2 =====\n") >= 0;
            for (const auto &line : report) {
                if (std::fprintf(file, "%s\n", line.c_str()) < 0) ok = false;
            }
            if (std::fflush(file) != 0) ok = false;
            if (std::fclose(file) != 0) ok = false;
        }
    }
    WHBUnmountSdCard();
    return ok;
}
