#pragma once

#include <coreinit/mcp.h>
#include <cstdint>
#include <string>
#include <vector>

// Title IDs, taken from PayloadLoaderInstaller (source/common/common.cpp) and
// recovery_menu (ios_mcp/source/options/SetColdbootTitle.c).
constexpr uint64_t TID_MENU_JPN = 0x0005001010040000ULL;
constexpr uint64_t TID_MENU_USA = 0x0005001010040100ULL;
constexpr uint64_t TID_MENU_EUR = 0x0005001010040200ULL;
constexpr uint64_t TID_HS_JPN   = 0x000500101004E000ULL;
constexpr uint64_t TID_HS_USA   = 0x000500101004E100ULL;
constexpr uint64_t TID_HS_EUR   = 0x000500101004E200ULL;

// Region bit index used by recovery_menu's region_tbl and by the Wii U Menu
// title ID nibble ((tid >> 8) & 0xF), see wafel_setup_mlc fix_region().
constexpr int REGION_IDX_JPN = 0;
constexpr int REGION_IDX_USA = 1;
constexpr int REGION_IDX_EUR = 2;

struct TitleEntry {
    uint64_t tid;
    std::string path;
};

struct Inspection {
    // Set when the tool cannot even inspect (no Mocha, mount failure, ...).
    bool fatal = false;
    std::string fatalError;

    // Region, as returned by MCP_GetSysProdSettings (backed by sys_prod.xml).
    bool sysProdOk      = false;
    int32_t sysProdErr  = 0;
    MCPSysProdSettings sysProd{};
    std::vector<std::string> sysProdXmlPaths; // located copies of sys_prod.xml (backup only)

    // Installed Wii U Menu titles.
    bool menuListOk = false;
    std::vector<TitleEntry> menus;       // every MCP_APP_TYPE_SYSTEM_MENU title
    bool menuAppXml[7] = {};             // recovery_menu-style app.xml probe per region idx
    bool pliMenuOk     = false;          // PayloadLoaderInstaller's exact query succeeded
    uint64_t pliMenuTid = 0;             // ...and the title ID it returned
    std::vector<uint64_t> hsTitles;      // installed Health & Safety titles
    uint64_t pliHsTid = 0;               // H&S that PayloadLoaderInstaller would pick

    // Raw SLC:/sys/config/system.xml (/vol/system/config/system.xml)
    bool systemXmlOk = false;
    std::string systemXmlError;
    std::vector<uint8_t> systemXml;      // raw bytes as read
    uint64_t defaultTitleId = 0;
    std::string defaultTitleIdText;
    size_t defaultTitleIdOffset = 0;     // byte offset of the 16 hex digits

    // PayloadLoaderInstaller's hash gate on system.xml (checkSystemXML()).
    bool pliMenuHashChecked = false, pliMenuHashMatch = false;
    bool pliHsHashChecked   = false, pliHsHashMatch   = false;

    // Derived.
    int installedMenuRegionIdx = -1;     // -1 = none or ambiguous
    bool needRegionFix         = false;
    bool needColdbootFix       = false;
    std::vector<std::string> blockers;   // any entry disables every write
    std::vector<std::string> warnings;
};

bool mountSystem(std::string &err);
void unmountSystem();

Inspection inspect();
std::vector<std::string> describe(const Inspection &in);
std::vector<std::string> describePlannedChanges(const Inspection &in);

// All of these append human-readable result lines to `out` and log them.
bool createBackups(const Inspection &in, std::vector<std::string> &out);
bool repair(const Inspection &in, std::vector<std::string> &out);

struct RestorePlan {
    bool possible = false;
    bool restoreSystemXml = false;
    std::vector<uint8_t> systemXml;
    bool restoreRegion = false;
    uint32_t productArea = 0, gameRegion = 0;
    std::vector<std::string> lines;
};
RestorePlan planRestore();
bool restore(const RestorePlan &plan, std::vector<std::string> &out);

// Read-only duplicate-menu diagnostics. Title relocation is not supported.
struct EurMenuPlan {
    std::vector<std::string> lines;
};
EurMenuPlan planEurMenu(const Inspection &in);

const char *regionName(int idx);
std::string titleName(uint64_t tid);
std::string tidString(uint64_t tid);
