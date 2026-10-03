#include "../upstream/PayloadLoaderInstaller/source/common/TitleSelection.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>

struct Entry { uint64_t titleId; char path[56]; uint32_t appType; };
constexpr uint32_t menuType = 0x90000001, appType = 0x90000020;
Entry entry(uint64_t id, uint32_t type = menuType) {
    Entry e{id, {}, type};
    const auto path = TitleSelection::mlcPath(id);
    std::memcpy(e.path, path.c_str(), path.size() + 1);
    return e;
}

int main() {
    using namespace TitleSelection;
    const uint64_t usa = 0x0005001010040100ULL, eur = 0x0005001010040200ULL, jpn = 0x0005001010040000ULL;
    assert(parseTitleId("0005001010040100") == usa);
    assert(parseTitleId("000500101004e100") == healthAndSafety(usa));
    assert(parseTitleId("000500101004E100") == healthAndSafety(usa));
    assert(!parseTitleId(nullptr) && !parseTitleId(""));
    assert(!parseTitleId("0005001010040100junk") && !parseTitleId("000500101004010"));
    assert(!parseTitleId("000500101004010Z") && !parseTitleId(" 005001010040100"));
    assert(mountedPath(usa) == "storage_mlc_installer:/sys/title/00050010/10040100");
    assert(menuForRegion(2, 2) == usa);
    assert(menuForRegion(1, 1) == jpn);
    assert(menuForRegion(4, 4) == eur);
    assert(!menuForRegion(2, 4) && !menuForRegion(2, 6));
    assert(!menuForRegion(0, 0) && !menuForRegion(3, 3) && !menuForRegion(8, 8));
    std::array<Entry, 3> entries{entry(jpn), entry(usa), entry(eur)};
    do {
        const auto selected = findUnique(entries.data(), entries.size(), usa, menuType);
        assert(selected && entries[*selected].titleId == usa);
    } while (std::next_permutation(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {return a.titleId < b.titleId;}));
    std::array<Entry, 2> exactHardwareOrder{entry(eur), entry(usa)};
    assert(findUnique(exactHardwareOrder.data(), 2, usa, menuType) == 1);
    assert(!findUnique(exactHardwareOrder.data(), 1, usa, menuType));
    std::array<Entry, 2> duplicate{entry(usa), entry(usa)};
    assert(!findUnique(duplicate.data(), 2, usa, menuType));
    auto invalid = entry(usa);
    invalid.path[5] = 'X';
    assert(!findUnique(&invalid, 1, usa, menuType));
    std::memset(invalid.path, 'x', sizeof(invalid.path));
    assert(!findUnique(&invalid, 1, usa, menuType));
    invalid = entry(usa, appType);
    assert(!findUnique(&invalid, 1, usa, menuType));
    assert(!findUnique<Entry>(nullptr, 0, usa, menuType));
    assert(healthAndSafety(usa) == 0x000500101004E100ULL);
    std::array<Entry, 2> hs{entry(healthAndSafety(eur), appType), entry(healthAndSafety(usa), appType)};
    assert(findUnique(hs.data(), 2, healthAndSafety(usa), appType) == 1);
    assert(knownColdboot(usa, usa) && knownColdboot(healthAndSafety(usa), usa));
    assert(!knownColdboot(eur, usa) && !knownColdboot(healthAndSafety(eur), usa) && !knownColdboot(0, usa));
    std::cout << "Title selection regression tests passed (including EUR-first hardware order).\n";
}
