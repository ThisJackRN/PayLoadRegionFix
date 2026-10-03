#include "InstallerService.h"
#include "safe_payload.h"
#include "Diagnostics.h"
#include "common/common.h"
#include "common/TitleSelection.h"
#include "common/fst_structs.h"
#include "fs/FSUtils.h"
#include "utils/StringTools.h"
#include "utils/WiiUScreen.h"
#include "utils/logger.h"
#include "utils/pugixml.hpp"
#include "utils/utils.h"
#include <coreinit/mcp.h>
#include <cstdlib>
#include <iosuhax.h>
#include <malloc.h>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <cerrno>

namespace {
std::optional<uint64_t> readRegionMenu() {
    const int32_t handle = MCP_Open();
    Diagnostics::record("Region: MCP_Open=" + std::to_string(handle));
    if (handle < 0) { Diagnostics::fail("Region MCP_Open failed"); return {}; }
    alignas(0x40) MCPSysProdSettings settings{};
    const auto result = MCP_GetSysProdSettings(handle, &settings);
    MCP_Close(handle);
    Diagnostics::record("MCP_GetSysProdSettings rc=" + std::to_string(result));
    if (result != 0) { Diagnostics::fail("MCP_GetSysProdSettings rejected (rc != 0)"); return {}; }
    // Never log serial, model, code_id or raw sysprod data.
    Diagnostics::record(StringTools::strfmt("Region product=0x%08X game=0x%08X",
        static_cast<unsigned>(settings.product_area), static_cast<unsigned>(settings.game_region)));
    const auto menu = TitleSelection::menuForRegion(settings.product_area, settings.game_region);
    if (!menu) Diagnostics::fail("Region fields are not an exact supported match");
    else Diagnostics::record(StringTools::strfmt("Expected menu=%016llX", *menu));
    return menu;
}

bool readTitleList(MCPAppType type, std::vector<MCPTitleListType> &out) {
    const int32_t handle = MCP_Open();
    Diagnostics::record(StringTools::strfmt("Title query type=%08X MCP_Open=%d", static_cast<unsigned>(type), handle));
    if (handle < 0) { Diagnostics::fail("Title-list MCP_Open failed"); return false; }
    const int32_t capacity = MCP_TitleCount(handle);
    Diagnostics::record("MCP_TitleCount=" + std::to_string(capacity));
    if (capacity <= 0 || capacity > 8192) {
        Diagnostics::fail("Title count outside 1..8192");
        MCP_Close(handle);
        return false;
    }
    const uint32_t bytes = sizeof(MCPTitleListType) * capacity;
    auto *entries = static_cast<MCPTitleListType *>(memalign(0x40, bytes));
    if (!entries) {
        Diagnostics::fail("Title-list allocation failed");
        MCP_Close(handle);
        return false;
    }
    std::memset(entries, 0, bytes);
    uint32_t count = capacity;
    const auto result = MCP_TitleListByAppType(handle, type, &count, entries, bytes);
    MCP_Close(handle);
    const bool ok = result == 0 && count <= static_cast<uint32_t>(capacity);
    Diagnostics::record(StringTools::strfmt("MCP_TitleListByAppType rc=%d count=%u capacity=%d", static_cast<int>(result), count, capacity));
    if (!ok) Diagnostics::fail("Title query rejected (rc != 0 or count > capacity)");
    if (ok) out.assign(entries, entries + count);
    free(entries);
    return ok;
}

bool selectTitle(const std::vector<MCPTitleListType> &entries, uint64_t expected, MCPAppType type) {
    const auto path = TitleSelection::mlcPath(expected);
    Diagnostics::record(StringTools::strfmt("Select expected=%016llX type=%08X", expected, static_cast<unsigned>(type)));
    Diagnostics::record("Expected path: " + path);
    unsigned matches = 0;
    for (const auto &entry : entries) {
        Diagnostics::record(StringTools::strfmt("MCP title=%016llX type=%08X version=%u", entry.titleId,
            static_cast<unsigned>(entry.appType), static_cast<unsigned>(entry.titleVersion)));
        const auto len = strnlen(entry.path, sizeof(entry.path));
        Diagnostics::record("MCP path: " + std::string(entry.path, len));
        if (entry.titleId != expected) continue;
        ++matches;
        if (len == sizeof(entry.path)) Diagnostics::fail("Expected title MCP path is not terminated");
        else if (std::string(entry.path, len) != path) Diagnostics::fail("Expected title MCP path differs");
        if (entry.appType != type) Diagnostics::fail("Expected title app type differs");
    }
    if (!matches) Diagnostics::fail("Expected title ID absent from MCP result");
    else if (matches > 1) Diagnostics::fail("Expected title ID appears more than once");
    const bool ok = TitleSelection::findUnique(entries.data(), entries.size(), expected, type).has_value();
    Diagnostics::record(ok ? "Unique title/path/type check PASS" : "Unique title/path/type check FAIL");
    return ok;
}

bool validateTitleFiles(uint64_t tid) {
    const auto path = TitleSelection::mountedPath(tid);
    for (const char *file : {"/code/app.xml", "/code/cos.xml", "/code/title.tmd"}) {
        struct stat st{};
        errno = 0;
        const auto name = path + file;
        const int rc = stat(name.c_str(), &st);
        const int statError = errno;
        Diagnostics::record("stat: " + name);
        Diagnostics::record(StringTools::strfmt("rc=%d errno=%d mode=0x%08X size=%lld regular=%d",
            rc, statError, static_cast<unsigned>(st.st_mode), static_cast<long long>(st.st_size), S_ISREG(st.st_mode) ? 1 : 0));
        if (rc != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            Diagnostics::fail(StringTools::strfmt("%016llX%s stat validation failed", tid, file));
            // Read probe for diagnosis only. It does not override a failed stat check.
            errno = 0;
            FILE *probe = std::fopen(name.c_str(), "rb");
            const int openError = errno;
            if (probe) {
                unsigned char byte;
                const auto n = std::fread(&byte, 1, 1, probe);
                Diagnostics::record(StringTools::strfmt("Read-only open PASS; first-byte read=%u ferror=%d", static_cast<unsigned>(n), std::ferror(probe)));
                std::fclose(probe);
            } else Diagnostics::record("Read-only open FAIL errno=" + std::to_string(openError));
            return false;
        }
    }
    pugi::xml_document doc;
    const auto parsed = doc.load_file((path + "/code/app.xml").c_str());
    if (!parsed) { Diagnostics::fail(std::string("app.xml parse: ") + parsed.description()); return false; }
    const auto titleNode = doc.child("app").child("title_id");
    if (!titleNode || titleNode.next_sibling("title_id")) { Diagnostics::fail("app.xml title_id missing or duplicated"); return false; }
    const auto actual = TitleSelection::parseTitleId(titleNode.child_value());
    if (!actual) { Diagnostics::fail("app.xml title_id is not exactly 16 hex digits"); return false; }
    Diagnostics::record(StringTools::strfmt("app.xml title_id=%016llX expected=%016llX", *actual, tid));
    if (*actual != tid) { Diagnostics::fail("app.xml title_id differs from expected title"); return false; }
    Diagnostics::record("Title files and app.xml identity PASS");
    return true;
}
} // namespace

InstallerService::eResults InstallerService::checkCOS(const std::string &path, char *hash) {
    std::string cosFilePath = path + "/code/cos.xml";
    DEBUG_FUNCTION_LINE("Loading %s", cosFilePath.c_str());

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(cosFilePath.c_str());
    if (!result) {
        DEBUG_FUNCTION_LINE("failed to open %s : %s", cosFilePath.c_str(), result.description());
        return COS_XML_PARSING_FAILED;
    }

    patchCOSXMLData(&doc);

    std::stringstream ss;
    doc.save(ss, "  ", pugi::format_default, pugi::encoding_utf8);

    std::string newHash = Utils::calculateSHA1(ss.str().c_str(), ss.str().size());

    if (std::string(hash) == newHash) {
        DEBUG_FUNCTION_LINE("Success! cos.xml is compatible");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("Hash mismatch! cos.xml is NOT compatible. Expected hash: %s actual hash: %s", hash, newHash.c_str());
    }

    return COS_XML_HASH_MISMATCH;
}

InstallerService::eResults InstallerService::checkSystemXML(const std::string &path, uint64_t titleId) {
    std::string inputFile = std::string(path + "/system.xml");

    systemXMLInformation *data = nullptr;
    for (int i = 0; systemXMLHashInformation[i].titleId != 0; i++) {
        if (systemXMLHashInformation[i].titleId == titleId) {
            data = &systemXMLHashInformation[i];
            break;
        }
    }

    if (data == nullptr) {
        DEBUG_FUNCTION_LINE("system xml information were not found.");
        return SYSTEM_XML_INFORMATION_NOT_FOUND;
    }

    DEBUG_FUNCTION_LINE("Setting coldboot to %016llX", data->titleId);

    pugi::xml_document doc;
    pugi::xml_parse_result resultSystem = doc.load_file(inputFile.c_str());

    if (!resultSystem) {
        DEBUG_FUNCTION_LINE("Error while parsing %s: %s", inputFile.c_str(), resultSystem.description());
        return SYSTEM_XML_PARSING_FAILED;
    }

    char tmp[18];
    snprintf(tmp, 17, "%016llX", data->titleId);

    doc.child("system").child("default_title_id").first_child().set_value(tmp);
    if (!doc.child("system").child("log").attribute("length")) {
        doc.child("system").child("log").append_attribute("length") = "0";
    }
    if (!doc.child("system").child("standby").attribute("length")) {
        doc.child("system").child("standby").append_attribute("length") = "0";
    }
    if (!doc.child("system").child("ramdisk").attribute("length")) {
        doc.child("system").child("ramdisk").append_attribute("length") = "0";
    }

    std::stringstream ss;
    doc.save(ss, "  ", pugi::format_default, pugi::encoding_utf8);

    std::string newHash = Utils::calculateSHA1(ss.str().c_str(), ss.str().size());

    if (std::string(data->hash) == newHash || std::string(data->hash2) == newHash) {
        DEBUG_FUNCTION_LINE("Success! system.xml is compatible");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("Hash mismatch! system.xml is NOT compatible. Expected hash: %s actual hash: %s", data->hash, newHash.c_str());
    }
    return SYSTEM_XML_HASH_MISMATCH;
}

InstallerService::eResults InstallerService::checkFST(const std::string &path, const char *fstHash) {
    std::string fstFilePath = path + "/code/title.fst";

    uint8_t *fstData     = nullptr;
    uint32_t fstDataSize = 0;

    DEBUG_FUNCTION_LINE("Trying to load FST from %s", fstFilePath.c_str());
    if (FSUtils::LoadFileToMem(fstFilePath.c_str(), &fstData, &fstDataSize) < 0) {
        DEBUG_FUNCTION_LINE("Failed to load title.fst");
        return FAILED_TO_LOAD_FILE;
    }
    InstallerService::eResults res = patchFSTData(fstData, fstDataSize);
    if (res != SUCCESS) {
        free(fstData);
        fstData = nullptr;
        return res;
    }

    std::string newHash = Utils::calculateSHA1((const char *) fstData, fstDataSize);

    free(fstData);
    fstData = nullptr;

    if (std::string(fstHash) == newHash) {
        DEBUG_FUNCTION_LINE("title.fst is compatible");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("title.fst is NOT compatible. expected hash: %s actual hash: %s", fstHash, newHash.c_str());
        return FST_HASH_MISMATCH;
    }
}

bool InstallerService::patchCOSXMLData(pugi::xml_document *doc) {
    pugi::xml_node appEntry = doc->child("app");
    appEntry.child("argstr").first_child().set_value("safe.rpx");
    appEntry.child("avail_size").first_child().set_value("00000000");
    appEntry.child("codegen_size").first_child().set_value("02000000");
    appEntry.child("codegen_core").first_child().set_value("80000001");
    appEntry.child("max_size").first_child().set_value("40000000");
    appEntry.child("max_codesize").first_child().set_value("00800000");
    for (pugi::xml_node permission : appEntry.child("permissions").children()) {
        auto mask = permission.child("mask");
        mask.first_child().set_value("FFFFFFFFFFFFFFFF");
    }
    return true;
}

std::optional<appInformation> InstallerService::getInstalledAppInformation() {
    const auto menu = getSystemMenuTitleId();
    if (!menu) { Diagnostics::record("H&S selection stopped because menu validation failed"); return {}; }
    const auto expected = TitleSelection::healthAndSafety(*menu);
    std::vector<MCPTitleListType> entries;
    if (!readTitleList(MCP_APP_TYPE_SYSTEM_APPS, entries)) return {};
    if (!selectTitle(entries, expected, MCP_APP_TYPE_SYSTEM_APPS) ||
        !validateTitleFiles(expected)) return {};
    for (int i = 0; supportedApps[i].titleId != 0; ++i) {
        if (supportedApps[i].titleId != expected) continue;
        auto app = supportedApps[i];
        const auto path = TitleSelection::mountedPath(expected);
        std::snprintf(app.path, sizeof(app.path), "%s", path.c_str());
        app.onTheWiiU = true;
        DEBUG_FUNCTION_LINE("Region-selected H&S: %016llX at %s", app.titleId, app.path);
        return app;
    }
    return {};
}

std::optional<uint64_t> InstallerService::getSystemMenuTitleId() {
    const auto expected = readRegionMenu();
    if (!expected) return {};
    std::vector<MCPTitleListType> entries;
    if (!readTitleList(MCP_APP_TYPE_SYSTEM_MENU, entries)) return {};
    if (!selectTitle(entries, *expected, MCP_APP_TYPE_SYSTEM_MENU) ||
        !validateTitleFiles(*expected)) return {};
    DEBUG_FUNCTION_LINE("Region-selected menu: %016llX (%u total menus)", *expected, static_cast<unsigned>(entries.size()));
    return expected;
}
InstallerService::eResults InstallerService::patchFSTData(uint8_t *fstData, uint32_t size) {
    auto *fstHeader = (FSTHeader *) fstData;
    if (strncmp(FSTHEADER_MAGIC, fstHeader->magic, 3) != 0) {
        DEBUG_FUNCTION_LINE("FST magic is wrong %s", fstHeader->magic);
        return InstallerService::FST_HEADER_MISMATCH;
    }

    auto numberOfSections = fstHeader->numberOfSections;
    DEBUG_FUNCTION_LINE("Found %d sections", numberOfSections);
    auto *sections = (FSTSectionEntry *) (fstData + sizeof(FSTHeader));

    auto usableSectionIndex = -1;

    for (uint32_t i = 0; i < numberOfSections; i++) {
        if (sections[i].hashMode == 2) {
            usableSectionIndex = i;
        }
    }

    if (usableSectionIndex < 0) {
        DEBUG_FUNCTION_LINE("Failed to find suitable section");
        return InstallerService::FST_NO_USABLE_SECTION_FOUND;
    } else {
        DEBUG_FUNCTION_LINE("Section %d can be used as a base", usableSectionIndex);
    }

    auto *rootEntry          = (FSTNodeEntry *) (fstData + sizeof(FSTHeader) + numberOfSections * sizeof(FSTSectionEntry));
    auto numberOfNodeEntries = rootEntry->directory.lastEntryNumber;

    char *stringTableOffset = (char *) ((uint32_t) rootEntry + (sizeof(FSTNodeEntry) * numberOfNodeEntries));

    FSTNodeEntry *nodeEntries = rootEntry;
    for (uint32_t i = 0; i < numberOfNodeEntries; i++) {
        if (sections[nodeEntries[i].sectionNumber].hashMode != 2) {
            auto nameOffset = (*((uint32_t *) nodeEntries[i].nameOffset) & 0xFFFFFF00) >> 8;
            DEBUG_FUNCTION_LINE("Updating sectionNumber for %s (entry %d)", stringTableOffset + nameOffset, i);
            nodeEntries[i].sectionNumber = usableSectionIndex;
        }
    }
    return InstallerService::SUCCESS;
}

std::string InstallerService::ErrorDescription(InstallerService::eResults error) {
    if (error == BOOT_SAFETY_CHECK_FAILED) {
        return "Region, installed titles, coldboot or Payload-Loader verification failed. No boot change was made.";
    } else if (error == VOLUME_FLUSH_FAILED) {
        return "system.xml was written but flushing failed. Keep the console running and inspect the storage error.";
    } else if (error == SUCCESS) {
        return "Success";
    } else if (error == NO_COMPATIBLE_APP_INSTALLED) {
        return "No compatible application was found on the console.";
    } else if (error == FAILED_TO_COPY_FILES) {
        return "Unable to copy files.";
    } else if (error == FAILED_TO_CHECK_HASH_COPIED_FILES) {
        return "The copy of a file has a different hash.";
    } else if (error == SYSTEM_XML_INFORMATION_NOT_FOUND) {
        return "Expected hashes for the target system.xml were not found.";
    } else if (error == SYSTEM_XML_PARSING_FAILED) {
        return "Failed to parse the system.xml";
    } else if (error == SYSTEM_XML_HASH_MISMATCH_RESTORE_FAILED) {
        return "DO NOT REBOOT BEFORE FIXING THIS: Failed to restore the system.xml after an error.";
    } else if (error == SYSTEM_XML_HASH_MISMATCH) {
        return "The patched system.xml had an unexpected hash but was successfully restored";
    } else if (error == RPX_HASH_MISMATCH) {
        return "The installed safe.rpx had an unexpected hash but was successfully restored";
    } else if (error == RPX_HASH_MISMATCH_RESTORE_FAILED) {
        return "DO NOT REBOOT BEFORE FIXING THIS: Failed to restore the safe.rpx after an error.";
    } else if (error == COS_XML_PARSING_FAILED) {
        return "Failed to parse the cos.xml";
    } else if (error == COS_XML_HASH_MISMATCH) {
        return "The patched cos.xml had an unexpected hash but was successfully restored";
    } else if (error == COS_XML_HASH_MISMATCH_RESTORE_FAILED) {
        return "DO NOT REBOOT BEFORE FIXING THIS: Failed to restore the cos.xml after an error";
    } else if (error == MALLOC_FAILED) {
        return "Failed to allocate memory";
    } else if (error == FST_HASH_MISMATCH) {
        return "The patched title.fst had an unexpected hash but was successfully restored";
    } else if (error == FST_HASH_MISMATCH_RESTORE_FAILED) {
        return "DO NOT REBOOT BEFORE FIXING THIS: Failed to restore the title.fst after an error";
    } else if (error == FST_HEADER_MISMATCH) {
        return "Unexpected header in title.fst found. The file is probably broken.";
    } else if (error == FST_NO_USABLE_SECTION_FOUND) {
        return "Unable to patch title.fst to allow FailST";
    } else if (error == FAILED_TO_LOAD_FILE) {
        return "Failed to load file.";
    } else {
        return "UNKNOWN ERROR";
    }
}

std::string InstallerService::ErrorMessage(InstallerService::eResults error) {
    if (error == BOOT_SAFETY_CHECK_FAILED) {
        return "BOOT_SAFETY_CHECK_FAILED";
    } else if (error == VOLUME_FLUSH_FAILED) {
        return "VOLUME_FLUSH_FAILED";
    } else if (error == SUCCESS) {
        return "Success";
    } else if (error == NO_COMPATIBLE_APP_INSTALLED) {
        return "NO_COMPATIBLE_APP_INSTALLED";
    } else if (error == FAILED_TO_COPY_FILES) {
        return "FAILED_TO_COPY_FILES";
    } else if (error == FAILED_TO_CHECK_HASH_COPIED_FILES) {
        return "FAILED_TO_CHECK_HASH_COPIED_FILES";
    } else if (error == SYSTEM_XML_INFORMATION_NOT_FOUND) {
        return "SYSTEM_XML_INFORMATION_NOT_FOUND";
    } else if (error == SYSTEM_XML_PARSING_FAILED) {
        return "SYSTEM_XML_PARSING_FAILED";
    } else if (error == SYSTEM_XML_HASH_MISMATCH_RESTORE_FAILED) {
        return "SYSTEM_XML_HASH_MISMATCH_RESTORE_FAILED";
    } else if (error == SYSTEM_XML_HASH_MISMATCH) {
        return "SYSTEM_XML_HASH_MISMATCH";
    } else if (error == RPX_HASH_MISMATCH) {
        return "RPX_HASH_MISMATCH";
    } else if (error == RPX_HASH_MISMATCH_RESTORE_FAILED) {
        return "RPX_HASH_MISMATCH_RESTORE_FAILED";
    } else if (error == COS_XML_PARSING_FAILED) {
        return "COS_XML_PARSING_FAILED";
    } else if (error == COS_XML_HASH_MISMATCH) {
        return "COS_XML_HASH_MISMATCH";
    } else if (error == COS_XML_HASH_MISMATCH_RESTORE_FAILED) {
        return "COS_XML_HASH_MISMATCH_RESTORE_FAILED";
    } else if (error == MALLOC_FAILED) {
        return "MALLOC_FAILED";
    } else if (error == FST_HASH_MISMATCH) {
        return "FST_HASH_MISMATCH";
    } else if (error == FST_HASH_MISMATCH_RESTORE_FAILED) {
        return "FST_HASH_MISMATCH_RESTORE_FAILED";
    } else if (error == FST_HEADER_MISMATCH) {
        return "FST_HEADER_MISMATCH";
    } else if (error == FST_NO_USABLE_SECTION_FOUND) {
        return "FST_NO_USABLE_SECTION_FOUND";
    } else if (error == FAILED_TO_LOAD_FILE) {
        return "FAILED_TO_LOAD_FILE";
    } else {
        return "UNKNOWN ERROR";
    }
}

bool InstallerService::isBackupAvailable(const std::string &path) {
    std::string backupList[] = {
            {"/content/title.fst.bak"},
            {"/content/cos.xml.bak"},
            {"/content/safe.rpx.bak"},
    };

    for (auto &backupEntry : backupList) {
        std::string backupFile = path + backupEntry;
        std::string backupSha1 = backupFile + ".sha1";

        if (!FSUtils::CheckFile(backupFile.c_str())) {
            return false;
        }

        if (!FSUtils::CheckFile(backupSha1.c_str())) {
            continue;
        }

        uint8_t *sha1FileCont;
        uint32_t sha1FileSize;
        FSUtils::LoadFileToMem(backupSha1.c_str(), &sha1FileCont, &sha1FileSize);
        if (!sha1FileCont) {
            return false;
        }

        std::string savedHash = std::string(sha1FileCont, sha1FileCont + sha1FileSize);
        std::string fileHash  = Utils::hashFile(backupFile);
        if (fileHash != savedHash) {
            return false;
        }
    }

    return true;
}

InstallerService::eResults InstallerService::restoreAppFiles(const std::string &path) {
    std::string backupList[][2] = {
            {"/code/title.fst", "/content/title.fst.bak"},
            {"/code/cos.xml", "/content/cos.xml.bak"},
            {"/code/safe.rpx", "/content/safe.rpx.bak"},
    };

    for (auto &backupOp : backupList) {
        std::string destPath   = path + backupOp[0];
        std::string backupPath = path + backupOp[1];

        if (!FSUtils::copyFile(backupPath, destPath)) {
            DEBUG_FUNCTION_LINE("Failed to copy files");
            return FAILED_TO_COPY_FILES;
        }

        std::string srcHash = Utils::hashFile(backupPath);
        std::string dstHash = Utils::hashFile(destPath);
        if (srcHash != dstHash) {
            DEBUG_FUNCTION_LINE("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
            return FAILED_TO_CHECK_HASH_COPIED_FILES;
        }
    }

    for (auto &backupOp : backupList) {
        std::string backupPath     = path + backupOp[1];
        std::string backupSha1Path = backupPath + ".sha1";
        ::remove(backupPath.c_str());
        ::remove(backupSha1Path.c_str());
    }

    DEBUG_FUNCTION_LINE("Successfully restored app files");
    return SUCCESS;
}

InstallerService::eResults InstallerService::backupAppFiles(const std::string &path) {
    std::string backupList[][2] = {
            {"/code/title.fst", "/content/title.fst.bak"},
            {"/code/cos.xml", "/content/cos.xml.bak"},
            {"/code/safe.rpx", "/content/safe.rpx.bak"},
    };

    for (auto &backupOp : backupList) {
        std::string backupSrc  = path + backupOp[0];
        std::string backupDst  = path + backupOp[1];
        std::string backupSha1 = backupDst + ".sha1";

        if (FSUtils::CheckFile(backupDst.c_str())) {
            DEBUG_FUNCTION_LINE("Already backed up: %s", backupSrc.c_str());
            continue;
        }

        if (!FSUtils::copyFile(backupSrc, backupDst)) {
            DEBUG_FUNCTION_LINE("Failed to copy files");
            return FAILED_TO_COPY_FILES;
        }

        std::string srcHash = Utils::hashFile(backupSrc);
        std::string dstHash = Utils::hashFile(backupDst);

        if (srcHash != dstHash) {
            ::remove(backupDst.c_str());
            DEBUG_FUNCTION_LINE("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
            return FAILED_TO_CHECK_HASH_COPIED_FILES;
        }

        FSUtils::saveBufferToFile(backupSha1.c_str(), srcHash.c_str(), srcHash.size());
    }

    DEBUG_FUNCTION_LINE("Successfully backed up app files");
    return SUCCESS;
}

InstallerService::eResults InstallerService::patchFST(const std::string &path, const char *fstHash) {
    std::string fstFilePath       = path + "/code/title.fst";
    std::string fstBackupFilePath = path + "/code/backup.fst";
    std::string fstTargetFilePath = path + "/code/title.fst";

    if (!FSUtils::copyFile(fstFilePath, fstBackupFilePath)) {
        DEBUG_FUNCTION_LINE("Failed to copy files");
        return FAILED_TO_COPY_FILES;
    }

    std::string srcHash = Utils::hashFile(fstFilePath);
    std::string dstHash = Utils::hashFile(fstBackupFilePath);

    if (srcHash != dstHash) {
        ::remove(fstBackupFilePath.c_str());
        DEBUG_FUNCTION_LINE("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
        return FAILED_TO_CHECK_HASH_COPIED_FILES;
    }

    uint8_t *fstData     = nullptr;
    uint32_t fstDataSize = 0;

    DEBUG_FUNCTION_LINE("Trying to load FST from %s", fstFilePath.c_str());
    if (FSUtils::LoadFileToMem(fstFilePath.c_str(), &fstData, &fstDataSize) < 0) {
        DEBUG_FUNCTION_LINE("Failed to load title.fst");
        return FAILED_TO_LOAD_FILE;
    }
    InstallerService::eResults res = patchFSTData(fstData, fstDataSize);
    if (res != SUCCESS) {
        free(fstData);
        return res;
    }
    FSUtils::saveBufferToFile(fstTargetFilePath.c_str(), fstData, fstDataSize);
    free(fstData);
    fstData = nullptr;

    std::string newHash = Utils::hashFile(fstTargetFilePath);

    if (std::string(fstHash) == newHash) {
        ::remove(fstBackupFilePath.c_str());
        DEBUG_FUNCTION_LINE("Successfully patched the title.fst");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("Hash mismatch! Expected %s but got %s while patching FST", fstHash, newHash.c_str());
    }

    FSUtils::copyFile(fstBackupFilePath, fstTargetFilePath);

    std::string srcHash2 = Utils::hashFile(fstTargetFilePath);

    if (srcHash != srcHash2) {
        DEBUG_FUNCTION_LINE("Something went wrong. Failed to restore the title.fst. DO NOT RESTART THE SYSTEM until you manually restored the title.fst");
        return FST_HASH_MISMATCH_RESTORE_FAILED;
    }

    ::remove(fstBackupFilePath.c_str());

    return FST_HASH_MISMATCH;
}

InstallerService::eResults InstallerService::patchCOS(const std::string &path, char *hash) {
    std::string cosFilePath       = path + "/code/cos.xml";
    std::string cosBackupFilePath = path + "/code/cback.xml";
    std::string cosTargetFilePath = path + "/code/cos.xml";

    if (!FSUtils::copyFile(cosFilePath, cosBackupFilePath)) {
        DEBUG_FUNCTION_LINE("Failed to copy files");
        return FAILED_TO_COPY_FILES;
    }

    std::string srcHash = Utils::hashFile(cosFilePath);
    std::string dstHash = Utils::hashFile(cosBackupFilePath);

    if (srcHash != dstHash) {
        ::remove(cosBackupFilePath.c_str());
        DEBUG_FUNCTION_LINE("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
        return FAILED_TO_CHECK_HASH_COPIED_FILES;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(cosFilePath.c_str());

    if (!result) {
        ::remove(cosBackupFilePath.c_str());
        DEBUG_FUNCTION_LINE("failed to open %s : %s", cosFilePath.c_str(), result.description());
        return COS_XML_PARSING_FAILED;
    }

    patchCOSXMLData(&doc);

    doc.save_file(cosTargetFilePath.c_str(), "  ", pugi::format_default, pugi::encoding_utf8);

    std::string newHash = Utils::hashFile(cosTargetFilePath);

    if (std::string(hash) == newHash) {
        ::remove(cosBackupFilePath.c_str());
        DEBUG_FUNCTION_LINE("Successfully patched the cos.xml");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("Hash mismatch! Expected %s but got %s while patching cos.xml", hash, newHash.c_str());
    }

    FSUtils::copyFile(cosBackupFilePath, cosTargetFilePath);

    std::string srcHash2 = Utils::hashFile(cosTargetFilePath);

    if (srcHash != srcHash2) {
        DEBUG_FUNCTION_LINE("Something went wrong. Failed to restore the title.fst. DO NOT RESTART THE SYSTEM until you manually restored the cos.xml");
        return COS_XML_HASH_MISMATCH_RESTORE_FAILED;
    }
    ::remove(cosBackupFilePath.c_str());

    return COS_XML_HASH_MISMATCH;
}

InstallerService::eResults InstallerService::copyRPX(const std::string &path, const uint8_t *rpx_data, size_t size, const std::string &rpx_hash) {
    std::string rpxSourceFilePath = path + "/code/safe.rpx";
    std::string rpxBackupFilePath = path + "/code/sbac.rpx";
    std::string rpxTargetFilePath = path + "/code/safe.rpx";

    if (!FSUtils::copyFile(rpxSourceFilePath, rpxBackupFilePath)) {
        DEBUG_CONSOLE_LOG("Failed to copy files");
        return FAILED_TO_COPY_FILES;
    }

    std::string srcHash = Utils::hashFile(rpxSourceFilePath);
    std::string dstHash = Utils::hashFile(rpxBackupFilePath);

    if (srcHash != dstHash) {
        ::remove(rpxBackupFilePath.c_str());
        DEBUG_CONSOLE_LOG("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
        return FAILED_TO_CHECK_HASH_COPIED_FILES;
    }

    const bool wrote = FSUtils::saveBufferToFile(rpxTargetFilePath.c_str(), (void *) rpx_data, size) == static_cast<int32_t>(size);
    if (!wrote) {
        DEBUG_CONSOLE_LOG("Cannot save rpx to file %s", rpxTargetFilePath.c_str());
    }

    std::string newHash = Utils::hashFile(rpxTargetFilePath);

    if (wrote && StringTools::strCompareIC(rpx_hash, newHash)) {
        ::remove(rpxBackupFilePath.c_str());
        DEBUG_CONSOLE_LOG("Successfully patched the safe.rpx");
        return SUCCESS;
    } else {
        DEBUG_CONSOLE_LOG("Hash mismatch! Expected %s but got %s while patching safe.rpx", rpx_hash.c_str(), newHash.c_str());
    }

    FSUtils::copyFile(rpxBackupFilePath, rpxTargetFilePath);

    std::string srcHash2 = Utils::hashFile(rpxTargetFilePath);

    if (srcHash != srcHash2) {
        DEBUG_CONSOLE_LOG("Something went wrong. Failed to restore the safe.rpx. DO NOT RESTART THE SYSTEM until you manually restored the safe.rpx");
        return RPX_HASH_MISMATCH_RESTORE_FAILED;
    }
    ::remove(rpxBackupFilePath.c_str());

    return RPX_HASH_MISMATCH;
}

InstallerService::eResults InstallerService::patchSystemXML(const std::string &path, uint64_t titleId) {
    std::string inputFile  = std::string(path + "/system.xml");
    std::string backupFile = std::string(path + "/sbackup.xml");
    std::string finalFile  = std::string(path + "/system.xml");

    if (!FSUtils::copyFile(inputFile, backupFile)) {
        DEBUG_CONSOLE_LOG("Failed to copy files");
        return FAILED_TO_COPY_FILES;
    }

    std::string srcHash = Utils::hashFile(inputFile);
    std::string dstHash = Utils::hashFile(backupFile);

    if (srcHash != dstHash) {
        ::remove(backupFile.c_str());
        DEBUG_CONSOLE_LOG("Hashes do not match. %s %s", srcHash.c_str(), dstHash.c_str());
        return FAILED_TO_CHECK_HASH_COPIED_FILES;
    }

    systemXMLInformation *data = nullptr;
    for (int i = 0; systemXMLHashInformation[i].titleId != 0; i++) {
        if (systemXMLHashInformation[i].titleId == titleId) {
            data = &systemXMLHashInformation[i];
            break;
        }
    }

    if (data == nullptr) {
        ::remove(backupFile.c_str());
        DEBUG_FUNCTION_LINE("Data was NULL");
        return SYSTEM_XML_INFORMATION_NOT_FOUND;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result resultSystem = doc.load_file(inputFile.c_str());
    if (!resultSystem) {
        ::remove(backupFile.c_str());
        DEBUG_FUNCTION_LINE("Error while parsing %s: %s", inputFile.c_str(), resultSystem.description());
        return SYSTEM_XML_PARSING_FAILED;
    }

    char tmp[18];
    snprintf(tmp, 17, "%016llX", data->titleId);

    doc.child("system").child("default_title_id").first_child().set_value(tmp);
    if (!doc.child("system").child("log").attribute("length")) {
        doc.child("system").child("log").append_attribute("length") = "0";
    }
    if (!doc.child("system").child("standby").attribute("length")) {
        doc.child("system").child("standby").append_attribute("length") = "0";
    }
    if (!doc.child("system").child("ramdisk").attribute("length")) {
        doc.child("system").child("ramdisk").append_attribute("length") = "0";
    }
    doc.save_file(finalFile.c_str(), "  ", pugi::format_default, pugi::encoding_utf8);

    std::string newHash = Utils::hashFile(finalFile);

    if (std::string(data->hash) == newHash || std::string(data->hash2) == newHash) {
        ::remove(backupFile.c_str());
        DEBUG_FUNCTION_LINE("Successfully patched the system.xml");
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("Hash mismatch! Expected %s but got %s while setting default titleID to %016llX", data->hash, newHash.c_str(), data->titleId);
    }

    FSUtils::copyFile(backupFile, finalFile);

    std::string srcHash2 = Utils::hashFile(finalFile);

    if (srcHash != srcHash2) {
        DEBUG_FUNCTION_LINE("Something went wrong. Failed to restore the system.xml. DO NOT RESTART THE SYSTEM until you manually restored the system.xml");
        return SYSTEM_XML_HASH_MISMATCH_RESTORE_FAILED;
    }

    ::remove(backupFile.c_str());

    return SYSTEM_XML_HASH_MISMATCH;
}

uint64_t InstallerService::getColdbootTitleId(const std::string &path) {
    std::string inputFile = std::string(path + "/system.xml");

    pugi::xml_document doc;
    pugi::xml_parse_result resultSystem = doc.load_file(inputFile.c_str());

    if (!resultSystem) {
        DEBUG_FUNCTION_LINE("Error while parsing %s: %s", inputFile.c_str(), resultSystem.description());
        return SYSTEM_XML_PARSING_FAILED;
    }

    const auto system = doc.child("system");
    const auto title = system.child("default_title_id");
    if (!system || system.next_sibling("system") || !title || title.next_sibling("default_title_id")) return 0;
    return TitleSelection::parseTitleId(title.child_value()).value_or(0);
}

InstallerService::eResults InstallerService::checkFSTAlreadyValid(const std::string &path, const std::string &hash) {
    std::string filePath = path + "/code/title.fst";
    return checkFileHash(filePath, hash);
}

InstallerService::eResults InstallerService::checkTMDValid(const std::string &path, const std::string &hash, const std::string &tmdWithCertHash) {
    std::string filePath = path + "/code/title.tmd";

    InstallerService::eResults result = checkFileHash(filePath, hash);

    if (result != SUCCESS) {
        // In some cases the tmd seems to have cert appended
        return checkFileHash(filePath, tmdWithCertHash);
    }
    return result;
}

InstallerService::eResults InstallerService::checkCOSAlreadyValid(const std::string &path, const std::string &hash) {
    std::string filePath = path + "/code/cos.xml";
    return checkFileHash(filePath, hash);
}

InstallerService::eResults InstallerService::checkRPXAlreadyValid(const std::string &path, const std::string &hash) {
    std::string filePath = path + "/code/safe.rpx";
    return checkFileHash(filePath, hash);
}

InstallerService::eResults InstallerService::checkFileHash(const std::string &filePath, const std::string &hash) {
    uint8_t *fileData     = nullptr;
    uint32_t fileDataSize = 0;

    if (FSUtils::LoadFileToMem(filePath.c_str(), &fileData, &fileDataSize) < 0) {
        return FAILED_TO_LOAD_FILE;
    }

    std::string newHash = Utils::calculateSHA1((const char *) fileData, fileDataSize);

    free(fileData);
    fileData = nullptr;

    if (StringTools::strCompareIC(hash, newHash)) {
        return SUCCESS;
    } else {
        DEBUG_FUNCTION_LINE("expected %s actual %s", hash.c_str(), newHash.c_str());
        return FST_HASH_MISMATCH;
    }
}

InstallerService::eResults InstallerService::setBootTitle(uint64_t titleId) {
    InstallerService::eResults result;

    // Revalidate at the write boundary; menu/UI state may be stale.
    const auto menu = getSystemMenuTitleId();
    const auto app = getInstalledAppInformation();
    const auto current = getColdbootTitleId("storage_slc_installer:/config");
    if (!menu || !app || app->titleId != TitleSelection::healthAndSafety(*menu) ||
        !TitleSelection::knownColdboot(current, *menu) ||
        (titleId != *menu && titleId != app->titleId)) return BOOT_SAFETY_CHECK_FAILED;
    if (titleId == app->titleId &&
        (!isColdBootAllowed(app->titleId) ||
         checkFSTAlreadyValid(app->path, app->fstHash) != SUCCESS ||
         checkCOSAlreadyValid(app->path, app->cosHash) != SUCCESS ||
         checkTMDValid(app->path, app->tmdHash, app->tmdWithCertHash) != SUCCESS ||
         checkRPXAlreadyValid(app->path, RPX_HASH) != SUCCESS)) return BOOT_SAFETY_CHECK_FAILED;

    if ((result = checkSystemXML("storage_slc_installer:/config", titleId)) != SUCCESS) {
        return result;
    }

    if ((result = patchSystemXML("storage_slc_installer:/config", titleId)) != SUCCESS) {
        return result;
    }

    auto fsaFd = IOSUHAX_FSA_Open();
    if (fsaFd >= 0) {
        if (IOSUHAX_FSA_FlushVolume(fsaFd, "/vol/storage_mlc01") == 0) {
            DEBUG_FUNCTION_LINE("Flushed mlc");
        }
        if (IOSUHAX_FSA_FlushVolume(fsaFd, "/vol/system") == 0) {
            DEBUG_FUNCTION_LINE("Flushed slc");
        } else {
            IOSUHAX_FSA_Close(fsaFd);
            return VOLUME_FLUSH_FAILED;
        }
        IOSUHAX_FSA_Close(fsaFd);
    } else {
        DEBUG_FUNCTION_LINE("Failed to open fsa");
        return VOLUME_FLUSH_FAILED;
    }

    return SUCCESS;
}
