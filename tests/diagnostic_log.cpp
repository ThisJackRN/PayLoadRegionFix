#include "../upstream/PayloadLoaderInstaller/source/Diagnostics.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
bool mountOK = true;
bool nullPath = false;
unsigned unmounts = 0;
std::string directory;
std::string readLog() {
    std::ifstream file(directory + "/regionfix-pli-diagnostic.log", std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
bool WHBMountSdCard() { return mountOK; }
char *WHBGetSdCardMountPath() { return nullPath ? nullptr : directory.data(); }
bool WHBUnmountSdCard() { ++unmounts; return true; }

int main() {
    char pattern[] = "/tmp/regionfix-diagnostic-test-XXXXXX";
    const auto temp = mkdtemp(pattern);
    assert(temp);
    directory = temp;
    Diagnostics::record("not collecting");
    assert(Diagnostics::lines().empty());
    Diagnostics::begin();
    Diagnostics::record("test stat rc=-1 errno=2");
    Diagnostics::fail("first failure");
    Diagnostics::fail("second failure");
    assert(Diagnostics::failure() == "first failure");
    assert(Diagnostics::saveToSD());
    assert(unmounts == 1);
    const auto first = readLog();
    assert(first.find("FAIL: first failure") != std::string::npos);
    assert(first.find("FAIL: second failure") != std::string::npos);
    Diagnostics::begin();
    assert(Diagnostics::failure().empty());
    Diagnostics::record("second session");
    assert(Diagnostics::saveToSD());
    const auto second = readLog();
    assert(second.substr(0, first.size()) == first); // append, not overwrite
    assert(second.find("second session") != std::string::npos);
    mountOK = false;
    assert(!Diagnostics::saveToSD());
    assert(unmounts == 2);
    mountOK = true;
    nullPath = true;
    assert(!Diagnostics::saveToSD());
    assert(unmounts == 3);
    nullPath = false;
    const auto realDirectory = directory;
    directory += "/missing-parent";
    assert(!Diagnostics::saveToSD());
    assert(unmounts == 4);
    directory = realDirectory;
    assert(readLog() == second); // failed saves did not change the earlier log
    std::cout << "Diagnostic log tests passed: first failure, reset, append, mount/path/open failures.\n";
}
