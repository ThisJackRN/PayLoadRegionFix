#include "regionfix.h"
#include "log.h"

#include "TinySHA1.hpp"
#include "pugixml.hpp"

#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <mocha/mocha.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Mount names are private to this tool so they cannot collide with anything
// Aroma or other homebrew registered. /vol/system is sys/ on the raw SLC.
constexpr const char *SLC_MOUNT      = "rf_slc";
constexpr const char *SLC_DEVICE     = "/dev/slc01";
constexpr const char *SLC_MOUNT_PATH = "/vol/storage_slc01";
constexpr const char *MLC_MOUNT      = "rf_mlc";

// PayloadLoaderInstaller: "storage_slc_installer:/config" + "/system.xml"
// The raw SLC device root holds scfm.img and sys/; the system's
// /vol/system is that sys/ folder (verified in regionfix.log on hardware).
constexpr const char *SYSTEM_XML_PATH     = "rf_slc:/sys/config/system.xml";
constexpr const char *SYSTEM_XML_DISPLAY  = "/vol/system/config/system.xml";
constexpr const char *SYSPROD_BIN_NAME    = "sysprod_settings.bin";

bool sMounted = false;

// recovery_menu ios_mcp/source/menu.h region_tbl
const char *kRegionTbl[7] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN"};

// PayloadLoaderInstaller source/common/common.cpp systemXMLHashInformation.
// SHA1 of the pugixml-normalised system.xml after default_title_id is set.
struct PliSystemXmlHash {
    uint64_t tid;
    const char *hash;
    const char *hash2;
};
const PliSystemXmlHash kPliHashes[] = {
        {TID_MENU_JPN, "2645065A42D18D390C78543E3C4FE7E1D1957A63", "5E5C707E6DAF82393E93971BE98BE3B12204932A"},
        {TID_MENU_USA, "124562D41A02C7112DDD5F9A8F0EE5DF97E23471", "DC0F9941E99C629625419F444B5A5B177A67309F"},
        {TID_MENU_EUR, "F06041A4E5B3F899E748F1BAEB524DE058809F1D", "A0273C466DE15F33EC161BCD908B5BFE359FE6E0"},
        {TID_HS_JPN, "066D672824128713F0A7D156142A68B998080148", "2849DE91560F6667FE7415F89FC916BE3A27DE75"},
        {TID_HS_USA, "0EBCA1DFC0AB7A6A7FE8FB5EAF23179621B726A1", "83CF5B1CE0B64C51D15B1EFCAD659063790EB590"},
        {TID_HS_EUR, "DE46EC3E9B823ABA6CB0638D0C4CDEEF9C793BDD", "ED59630448EC6946F3E51618DA3681EC3A84D391"},
};

bool isMenuTid(uint64_t tid) {
    return tid == TID_MENU_JPN || tid == TID_MENU_USA || tid == TID_MENU_EUR;
}

int menuRegionIdx(uint64_t tid) {
    return isMenuTid(tid) ? (int) ((tid >> 8) & 0xF) : -1;
}

int regionIdxFromBits(uint32_t bits) {
    if (bits == 0 || (bits & (bits - 1)) != 0) return -1; // must be exactly one bit
    return __builtin_ctz(bits);
}

std::string regionBitsString(uint32_t bits) {
    std::string s;
    for (int i = 0; i < 7; i++) {
        if (bits & (1u << i)) {
            if (!s.empty()) s += " ";
            s += kRegionTbl[i];
        }
    }
    char hex[16];
    snprintf(hex, sizeof(hex), " (0x%02lX)", (unsigned long) bits);
    return (s.empty() ? "none" : s) + hex;
}

std::string sha1Hex(const void *data, size_t size) {
    sha1::SHA1 s;
    s.processBytes(data, size);
    uint32_t digest[5];
    s.getDigest(digest);
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%08lX%08lX%08lX%08lX%08lX",
             (unsigned long) digest[0], (unsigned long) digest[1], (unsigned long) digest[2],
             (unsigned long) digest[3], (unsigned long) digest[4]);
    return tmp;
}

std::string sha1Hex(const std::vector<uint8_t> &v) {
    return sha1Hex(v.data(), v.size());
}

bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool readFile(const std::string &path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[0x1000];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// Writes a new file on the SD card and reads it back to confirm the content.
bool writeFileVerified(const std::string &path, const void *data, size_t size) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok      = (fflush(f) == 0) && ok;
    fsync(fileno(f));
    ok = (fclose(f) == 0) && ok;
    if (!ok) return false;

    std::vector<uint8_t> check;
    if (!readFile(path, check)) return false;
    return check.size() == size && memcmp(check.data(), data, size) == 0;
}

bool writeTextVerified(const std::string &path, const std::string &text) {
    return writeFileVerified(path, text.data(), text.size());
}

// Flushes SLC and MLC the same way PayloadLoaderInstaller::setBootTitle() does.
void flushSlc() {
    FSAInit();
    FSAClientHandle h = FSAAddClient(nullptr);
    if (h < 0) {
        Log::write("FSAAddClient failed: %d (volumes not explicitly flushed)", (int) h);
        return;
    }
    MochaUtilsStatus st = Mocha_UnlockFSClientEx(h);
    if (st == MOCHA_RESULT_SUCCESS) {
        for (const char *vol : {SLC_MOUNT_PATH, "/vol/system", "/vol/storage_mlc01"}) {
            FSError r = FSAFlushVolume(h, vol);
            Log::write("FSAFlushVolume(%s): %d", vol, (int) r);
        }
    } else {
        Log::write("Mocha_UnlockFSClientEx failed: %s", Mocha_GetStatusStr(st));
    }
    FSADelClient(h);
}

// ---------------------------------------------------------------------------
// Direct FSA access to SLC files, with a Mocha-unlocked client. This is how
// PayloadLoaderInstaller (iosuhax FSA) and recovery_menu reach /vol/system.
// ---------------------------------------------------------------------------

struct UnlockedFsa {
    FSAClientHandle h = -1;
    UnlockedFsa() {
        FSAInit();
        h = FSAAddClient(nullptr);
        if (h >= 0 && Mocha_UnlockFSClientEx(h) != MOCHA_RESULT_SUCCESS) {
            Log::write("FSA: Mocha_UnlockFSClientEx failed");
            FSADelClient(h);
            h = -1;
        }
    }
    ~UnlockedFsa() {
        if (h >= 0) FSADelClient(h);
    }
};

bool fsaReadFile(const char *path, std::vector<uint8_t> &out) {
    out.clear();
    UnlockedFsa c;
    if (c.h < 0) return false;
    FSAFileHandle fh;
    FSError r = FSAOpenFileEx(c.h, path, "r", (FSMode) 0x666, FS_OPEN_FLAG_NONE, 0, &fh);
    if (r != FS_ERROR_OK) {
        Log::write("FSA open(%s, r): %s (%d)", path, FSAGetStatusStr(r), (int) r);
        return false;
    }
    auto *buf = (uint8_t *) memalign(0x40, 0x1000);
    bool ok   = buf != nullptr;
    while (ok) {
        FSError n = FSAReadFile(c.h, buf, 1, 0x1000, fh, 0);
        if (n < 0) {
            Log::write("FSA read(%s): %s (%d)", path, FSAGetStatusStr(n), (int) n);
            ok = false;
        } else if (n == 0) {
            break;
        } else {
            out.insert(out.end(), buf, buf + n);
        }
    }
    free(buf);
    FSACloseFile(c.h, fh);
    return ok;
}

bool fsaOverwrite(const char *path, size_t offset, const void *data, size_t size) {
    UnlockedFsa c;
    if (c.h < 0) return false;
    FSAFileHandle fh;
    FSError r = FSAOpenFileEx(c.h, path, "r+", (FSMode) 0x666, FS_OPEN_FLAG_NONE, 0, &fh);
    if (r != FS_ERROR_OK) {
        Log::write("FSA open(%s, r+): %s (%d)", path, FSAGetStatusStr(r), (int) r);
        return false;
    }
    auto *buf = (uint8_t *) memalign(0x40, (size + 0x3F) & ~0x3F);
    bool ok   = buf != nullptr;
    if (ok) {
        memcpy(buf, data, size);
        FSError n = FSAWriteFileWithPos(c.h, buf, 1, size, offset, fh, 0);
        ok        = n == (FSError) size;
        if (!ok) Log::write("FSA write(%s): %s (%d)", path, FSAGetStatusStr(n), (int) n);
        FSError f = FSAFlushFile(c.h, fh);
        Log::write("FSA flush file: %d", (int) f);
    }
    free(buf);
    FSACloseFile(c.h, fh);
    return ok;
}

// Logs what the devoptab mount can see, to diagnose read failures.
void logDir(const std::string &path) {
    DIR *d = opendir(path.c_str());
    if (!d) {
        Log::write("opendir(%s) failed: errno %d", path.c_str(), errno);
        return;
    }
    std::string names;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < 25) {
        names += std::string(e->d_name) + " ";
        n++;
    }
    closedir(d);
    Log::write("%s contains: %s", path.c_str(), names.c_str());
}

// ---------------------------------------------------------------------------
// MCP helpers
// ---------------------------------------------------------------------------

bool getSysProd(MCPSysProdSettings &out, int32_t &err) {
    int32_t mcp = MCP_Open();
    if (mcp < 0) {
        err = mcp;
        return false;
    }
    auto *buf = (MCPSysProdSettings *) memalign(0x40, 0x80);
    if (!buf) {
        MCP_Close(mcp);
        err = -1;
        return false;
    }
    memset(buf, 0, 0x80);
    err = MCP_GetSysProdSettings(mcp, buf);
    MCP_Close(mcp);
    if (err >= 0) memcpy(&out, buf, sizeof(out));
    free(buf);
    return err >= 0;
}

// WUT has no wrapper for MCP_SetSysProdSettings. This is the same ioctl
// (0x41, one input vector holding the 0x46-byte struct) that recovery_menu
// (ios_mcp/source/mcp_misc.c) and wafel_setup_mlc (source/sysprod.c) send
// from inside IOSU. Whether MCP accepts it from the PPC side depends on the
// running CFW; a rejection changes nothing.
int32_t setSysProd(const MCPSysProdSettings &in) {
    int32_t mcp = MCP_Open();
    if (mcp < 0) return mcp;

    auto *buf = (MCPSysProdSettings *) memalign(0x40, 0x80);
    auto *vec = (IOSVec *) memalign(0x40, 0x40);
    if (!buf || !vec) {
        free(buf);
        free(vec);
        MCP_Close(mcp);
        return -1;
    }
    memset(buf, 0, 0x80);
    memcpy(buf, &in, sizeof(in));
    memset(vec, 0, 0x40);
    vec[0].vaddr = buf;
    vec[0].len   = sizeof(MCPSysProdSettings);
    vec[0].paddr = nullptr;

    int32_t res = IOS_Ioctlv(mcp, 0x41, 1, 0, vec);
    MCP_Close(mcp);
    free(vec);
    free(buf);
    return res;
}

bool listTitlesByAppType(MCPAppType type, std::vector<MCPTitleListType> &out, int32_t &err) {
    out.clear();
    int32_t mcp = MCP_Open();
    if (mcp < 0) {
        err = mcp;
        return false;
    }
    int32_t count = MCP_TitleCount(mcp);
    if (count <= 0) {
        MCP_Close(mcp);
        err = count;
        return false;
    }
    auto *list = (MCPTitleListType *) memalign(0x40, sizeof(MCPTitleListType) * count);
    if (!list) {
        MCP_Close(mcp);
        err = -1;
        return false;
    }
    uint32_t outCount = 0;
    err = MCP_TitleListByAppType(mcp, type, &outCount, list, sizeof(MCPTitleListType) * count);
    MCP_Close(mcp);
    if (err >= 0) {
        for (uint32_t i = 0; i < outCount && i < (uint32_t) count; i++) {
            out.push_back(list[i]);
        }
    }
    free(list);
    return err >= 0;
}

// Byte-for-byte the query in PayloadLoaderInstaller::getSystemMenuTitleId():
// a buffer for exactly one entry. If more than one Wii U Menu is registered,
// PLI silently gets whichever MCP returns first.
bool pliSystemMenuQuery(uint64_t &tid) {
    int32_t mcp = MCP_Open();
    if (mcp < 0) return false;
    uint32_t titleCount = 1;
    auto *list = (MCPTitleListType *) memalign(0x40, sizeof(MCPTitleListType) * titleCount);
    if (!list) {
        MCP_Close(mcp);
        return false;
    }
    MCPError e = MCP_TitleListByAppType(mcp, MCP_APP_TYPE_SYSTEM_MENU, &titleCount, list, sizeof(MCPTitleListType) * titleCount);
    MCP_Close(mcp);
    bool ok = e >= 0 && titleCount == 1;
    if (ok) tid = list->titleId;
    free(list);
    return ok;
}

// ---------------------------------------------------------------------------
// system.xml
// ---------------------------------------------------------------------------

bool isHex16(const std::string &s) {
    if (s.size() != 16) return false;
    for (char c : s) {
        if (!isxdigit((unsigned char) c)) return false;
    }
    return true;
}

// Validates system.xml and finds the exact byte range of the default_title_id
// value so a repair only has to overwrite those 16 bytes in place.
bool parseSystemXml(const std::vector<uint8_t> &raw, uint64_t &tid, std::string &text, size_t &offset, std::string &err) {
    pugi::xml_document doc;
    pugi::xml_parse_result res = doc.load_buffer(raw.data(), raw.size());
    if (!res) {
        err = std::string("XML parse error: ") + res.description();
        return false;
    }
    pugi::xml_node sys = doc.child("system");
    if (!sys) {
        err = "no <system> root element";
        return false;
    }
    pugi::xml_node dt = sys.child("default_title_id");
    if (!dt) {
        err = "no <default_title_id> element";
        return false;
    }
    if (dt.next_sibling("default_title_id")) {
        err = "more than one <default_title_id> element";
        return false;
    }
    // Same accessor PayloadLoaderInstaller::getColdbootTitleId() uses.
    std::string value = dt.first_child().value();
    if (!isHex16(value)) {
        err = "default_title_id is not 16 hex digits: '" + value + "'";
        return false;
    }

    std::string s(raw.begin(), raw.end());
    const std::string openTag  = "<default_title_id";
    const std::string closeTag = "</default_title_id>";
    size_t p = s.find(openTag);
    if (p == std::string::npos || s.find(openTag, p + 1) != std::string::npos) {
        err = "cannot locate a unique <default_title_id> tag in the raw file";
        return false;
    }
    char after = s[p + openTag.size()];
    if (after != '>' && after != ' ' && after != '\t') {
        err = "unexpected <default_title_id> tag syntax";
        return false;
    }
    size_t gt = s.find('>', p);
    if (gt == std::string::npos || s[gt - 1] == '/') {
        err = "unexpected <default_title_id> tag syntax";
        return false;
    }
    size_t start = gt + 1;
    if (start + 16 + closeTag.size() > s.size() ||
        s.compare(start + 16, closeTag.size(), closeTag) != 0) {
        err = "default_title_id value is not exactly 16 characters in the raw file";
        return false;
    }
    std::string rawValue = s.substr(start, 16);
    if (rawValue != value) {
        err = "raw default_title_id does not match the parsed value";
        return false;
    }

    tid    = strtoull(value.c_str(), nullptr, 16);
    text   = value;
    offset = start;
    return true;
}

// Replicates PayloadLoaderInstaller::checkSystemXML(): set default_title_id,
// add missing length attributes, save with pugixml, SHA1 the output.
bool pliSystemXmlHashMatches(const std::vector<uint8_t> &raw, uint64_t tid) {
    const PliSystemXmlHash *entry = nullptr;
    for (auto &h : kPliHashes) {
        if (h.tid == tid) entry = &h;
    }
    if (!entry) return false;

    pugi::xml_document doc;
    if (!doc.load_buffer(raw.data(), raw.size())) return false;

    char tmp[18];
    snprintf(tmp, 17, "%016llX", (unsigned long long) tid);
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
    std::string out = ss.str();
    std::string h   = sha1Hex(out.data(), out.size());
    return h == entry->hash || h == entry->hash2;
}

// Bounded search for sys_prod.xml on the SLC. It is only backed up for
// reference; the region itself is always read and written through MCP.
void findFiles(const std::string &dir, const char *name, int depth, std::vector<std::string> &out) {
    if (depth < 0) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + (dir.back() == '/' ? "" : "/") + n;
        if (n == name) {
            out.push_back(full);
            continue;
        }
        // Skip the installed-title tree; it is large and holds no config.
        if (n == "title") continue;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            findFiles(full, name, depth - 1, out);
        }
    }
    closedir(d);
}

std::string displayPath(const std::string &p) {
    std::string prefix = std::string(SLC_MOUNT) + ":";
    if (p.rfind(prefix + "/sys/", 0) == 0) return "/vol/system" + p.substr(prefix.size() + 4);
    if (p.rfind(prefix, 0) == 0) return "slc01:" + p.substr(prefix.size());
    return p;
}

// Anything that identifies the console must match before a region restore.
bool sameConsole(const MCPSysProdSettings &a, const MCPSysProdSettings &b) {
    return memcmp(a.code_id, b.code_id, sizeof(a.code_id)) == 0 &&
           memcmp(a.serial_id, b.serial_id, sizeof(a.serial_id)) == 0 &&
           memcmp(a.model_number, b.model_number, sizeof(a.model_number)) == 0;
}

void emit(std::vector<std::string> &out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void emit(std::vector<std::string> &out, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out.emplace_back(buf);
    Log::write("%s", buf);
}

// Writes <name>.<stamp> always, and <name>.original (+ .sha1) only the first
// time, so the pristine pre-tool state is never overwritten.
bool backupBlob(const std::string &name, const std::string &source, const std::vector<uint8_t> &data,
                const std::string &stamp, std::vector<std::string> &out) {
    std::string base     = std::string(SD_BACKUP_DIR) + "/" + name;
    std::string stamped  = base + "." + stamp;
    std::string original = base + ".original";
    std::string hash     = sha1Hex(data);

    if (!writeFileVerified(stamped, data.data(), data.size())) {
        emit(out, "FAILED to write %s.%s", name.c_str(), stamp.c_str());
        return false;
    }
    if (!fileExists(original)) {
        if (!writeFileVerified(original, data.data(), data.size()) ||
            !writeTextVerified(original + ".sha1", hash)) {
            emit(out, "FAILED to write %s.original", name.c_str());
            return false;
        }
        emit(out, "Saved %s.original", name.c_str());
    } else {
        Log::write("Kept existing %s.original (not overwritten)", name.c_str());
    }
    emit(out, "Saved %s.%s", name.c_str(), stamp.c_str());

    FILE *m = fopen(SD_BACKUP_DIR "/manifest.txt", "a");
    if (m) {
        fprintf(m, "%s  %s  size=%u  sha1=%s  source=%s\n", stamp.c_str(), name.c_str(),
                (unsigned) data.size(), hash.c_str(), source.c_str());
        fflush(m);
        fsync(fileno(m));
        fclose(m);
    }
    return true;
}

// Loads <name>.original and checks it against its .sha1 sidecar.
bool loadOriginal(const std::string &name, std::vector<uint8_t> &data, std::string &err) {
    std::string path = std::string(SD_BACKUP_DIR) + "/" + name + ".original";
    if (!fileExists(path)) {
        err = "no backup (" + name + ".original)";
        return false;
    }
    std::vector<uint8_t> sha;
    if (!readFile(path, data) || !readFile(path + ".sha1", sha)) {
        err = "cannot read " + name + ".original or its .sha1";
        return false;
    }
    std::string expected(sha.begin(), sha.end());
    while (!expected.empty() && isspace((unsigned char) expected.back())) expected.pop_back();
    if (sha1Hex(data) != expected) {
        err = name + ".original does not match its .sha1 (corrupt?)";
        return false;
    }
    return true;
}

// Overwrites a file in place without truncating it. The caller guarantees
// the new content has the same size as the file on disk.
bool overwriteInPlace(const char *path, size_t offset, const void *data, size_t size) {
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    bool ok = fseek(f, (long) offset, SEEK_SET) == 0;
    ok      = ok && fwrite(data, 1, size, f) == size;
    ok      = (fflush(f) == 0) && ok;
    fsync(fileno(f));
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// system.xml I/O. Direct FSA on /vol/system is tried first (the path
// PayloadLoaderInstaller uses); the devoptab mount is the fallback. Writes
// always go through whichever method last read the file successfully.
constexpr const char *SYSTEM_XML_FSA_PATH = "/vol/system/config/system.xml";
enum class XmlIo { None, Fsa, Devoptab };
XmlIo sXmlIo = XmlIo::None;

bool readSystemXml(std::vector<uint8_t> &out) {
    errno = 0;
    if (readFile(SYSTEM_XML_PATH, out) && !out.empty()) {
        if (sXmlIo != XmlIo::Devoptab) Log::write("system.xml read via devoptab (%s)", SYSTEM_XML_PATH);
        sXmlIo = XmlIo::Devoptab;
        return true;
    }
    Log::write("devoptab read of %s failed: errno %d", SYSTEM_XML_PATH, errno);
    if (fsaReadFile(SYSTEM_XML_FSA_PATH, out) && !out.empty()) {
        if (sXmlIo != XmlIo::Fsa) Log::write("system.xml read via direct FSA (%s)", SYSTEM_XML_FSA_PATH);
        sXmlIo = XmlIo::Fsa;
        return true;
    }
    sXmlIo = XmlIo::None;
    return false;
}

bool writeSystemXml(size_t offset, const void *data, size_t size) {
    switch (sXmlIo) {
        case XmlIo::Fsa: return fsaOverwrite(SYSTEM_XML_FSA_PATH, offset, data, size);
        case XmlIo::Devoptab: return overwriteInPlace(SYSTEM_XML_PATH, offset, data, size);
        default: return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

const char *regionName(int idx) {
    return (idx >= 0 && idx < 7) ? kRegionTbl[idx] : "???";
}

std::string tidString(uint64_t tid) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long) tid);
    return buf;
}

std::string titleName(uint64_t tid) {
    switch (tid) {
        case TID_MENU_JPN: return "Wii U Menu [JPN]";
        case TID_MENU_USA: return "Wii U Menu [USA]";
        case TID_MENU_EUR: return "Wii U Menu [EUR]";
        case TID_HS_JPN: return "Health & Safety [JPN]";
        case TID_HS_USA: return "Health & Safety [USA]";
        case TID_HS_EUR: return "Health & Safety [EUR]";
        default: return "unknown title";
    }
}

bool mountSystem(std::string &err) {
    MochaUtilsStatus st = Mocha_InitLibrary();
    if (st != MOCHA_RESULT_SUCCESS) {
        err = std::string("Mocha_InitLibrary: ") + Mocha_GetStatusStr(st) +
              ". Run this from Aroma (MochaPayload required).";
        return false;
    }
    // Reusing the system's /vol/system mount gives no file access from an
    // app, so mount the SLC device itself, like ftpiiu_plugin's
    // MountWrapper("storage_slc", "/dev/slc01", "/vol/storage_slc01").
    st = Mocha_MountFS(SLC_MOUNT, SLC_DEVICE, SLC_MOUNT_PATH);
    if (st == MOCHA_RESULT_ALREADY_EXISTS) {
        st = Mocha_MountFS(SLC_MOUNT, nullptr, SLC_MOUNT_PATH);
    }
    if (st != MOCHA_RESULT_SUCCESS) {
        err = std::string("Mounting the SLC failed: ") + Mocha_GetStatusStr(st);
        return false;
    }
    st = Mocha_MountFS(MLC_MOUNT, nullptr, "/vol/storage_mlc01");
    if (st != MOCHA_RESULT_SUCCESS) {
        Mocha_UnmountFS(SLC_MOUNT);
        err = std::string("Mounting /vol/storage_mlc01 failed: ") + Mocha_GetStatusStr(st);
        return false;
    }
    sMounted = true;
    Log::write("Mounted %s at %s as %s:/ and /vol/storage_mlc01 as %s:/", SLC_DEVICE, SLC_MOUNT_PATH, SLC_MOUNT, MLC_MOUNT);
    return true;
}

void unmountSystem() {
    if (!sMounted) return;
    Mocha_UnmountFS(MLC_MOUNT);
    Mocha_UnmountFS(SLC_MOUNT);
    Mocha_DeInitLibrary();
    sMounted = false;
}

// ---------------------------------------------------------------------------
// Inspection (read only)
// ---------------------------------------------------------------------------

Inspection inspect() {
    Inspection in;
    Log::write("--- Inspection (read-only) ---");

    // 1. Region via MCP (same call recovery_menu getRegionInfo() uses).
    in.sysProdOk = getSysProd(in.sysProd, in.sysProdErr);
    if (in.sysProdOk) {
        Log::write("sys_prod product_area=0x%02lX game_region=0x%02lX (via MCP_GetSysProdSettings)",
                   (unsigned long) in.sysProd.product_area, (unsigned long) in.sysProd.game_region);
    } else {
        Log::write("MCP_GetSysProdSettings failed: 0x%08lX", (unsigned long) in.sysProdErr);
    }
    findFiles(std::string(SLC_MOUNT) + ":/", "sys_prod.xml", 3, in.sysProdXmlPaths);
    for (auto &p : in.sysProdXmlPaths) Log::write("Found sys_prod.xml at %s", displayPath(p).c_str());
    std::vector<std::string> systemXmls;
    findFiles(std::string(SLC_MOUNT) + ":/", "system.xml", 3, systemXmls);
    for (auto &p : systemXmls) Log::write("Found system.xml at %s (%s)", p.c_str(), displayPath(p).c_str());

    // 2. Installed Wii U Menu titles.
    std::vector<MCPTitleListType> list;
    int32_t err = 0;
    in.menuListOk = listTitlesByAppType(MCP_APP_TYPE_SYSTEM_MENU, list, err);
    if (!in.menuListOk) Log::write("MCP_TitleListByAppType(SYSTEM_MENU) failed: 0x%08lX", (unsigned long) err);
    for (auto &t : list) {
        in.menus.push_back({t.titleId, std::string(t.path, strnlen(t.path, sizeof(t.path)))});
        Log::write("MCP system menu title: %s at %s", tidString(t.titleId).c_str(), in.menus.back().path.c_str());
    }
    in.pliMenuOk = pliSystemMenuQuery(in.pliMenuTid);
    Log::write("PayloadLoaderInstaller-style menu query: %s %s", in.pliMenuOk ? "ok" : "failed",
               in.pliMenuOk ? tidString(in.pliMenuTid).c_str() : "");

    // recovery_menu DebugSystemRegion probe: sys/title/00050010/10040x00/code/app.xml
    for (int i = 0; i < 7; i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s:/sys/title/00050010/10040%d00/code/app.xml", MLC_MOUNT, i);
        in.menuAppXml[i] = fileExists(path);
        if (in.menuAppXml[i]) Log::write("Wii U Menu app.xml present for region %s", regionName(i));
    }

    // Health & Safety titles, checked in PayloadLoaderInstaller's order.
    if (listTitlesByAppType(MCP_APP_TYPE_SYSTEM_APPS, list, err)) {
        for (auto &t : list) {
            if (t.titleId == TID_HS_JPN || t.titleId == TID_HS_USA || t.titleId == TID_HS_EUR) {
                in.hsTitles.push_back(t.titleId);
                Log::write("H&S title installed: %s", tidString(t.titleId).c_str());
            }
        }
    }
    for (uint64_t hs : {TID_HS_JPN, TID_HS_USA, TID_HS_EUR}) {
        bool found = false;
        for (uint64_t t : in.hsTitles) found |= (t == hs);
        if (found) {
            in.pliHsTid = hs;
            break;
        }
    }

    // 3. system.xml
    Log::write("Coldboot config path: %s", SYSTEM_XML_DISPLAY);
    if (!readSystemXml(in.systemXml) || in.systemXml.empty()) {
        in.systemXmlError = "cannot read " + std::string(SYSTEM_XML_DISPLAY);
    } else if (parseSystemXml(in.systemXml, in.defaultTitleId, in.defaultTitleIdText,
                              in.defaultTitleIdOffset, in.systemXmlError)) {
        in.systemXmlOk = true;
        Log::write("system.xml size=%u sha1=%s default_title_id=%s (offset %u)",
                   (unsigned) in.systemXml.size(), sha1Hex(in.systemXml).c_str(),
                   in.defaultTitleIdText.c_str(), (unsigned) in.defaultTitleIdOffset);

        in.pliMenuHashChecked = true;
        in.pliMenuHashMatch   = pliSystemXmlHashMatches(in.systemXml, TID_MENU_USA);
        if (in.pliHsTid) {
            in.pliHsHashChecked = true;
            in.pliHsHashMatch   = pliSystemXmlHashMatches(in.systemXml, in.pliHsTid);
        }
        Log::write("PLI system.xml hash gate: USA menu=%s H&S=%s",
                   in.pliMenuHashMatch ? "match" : "MISMATCH",
                   in.pliHsHashChecked ? (in.pliHsHashMatch ? "match" : "MISMATCH") : "n/a");
    }
    if (!in.systemXmlOk) {
        Log::write("system.xml problem: %s", in.systemXmlError.c_str());
        logDir(std::string(SLC_MOUNT) + ":/");
        logDir(std::string(SLC_MOUNT) + ":/sys");
        logDir(std::string(SLC_MOUNT) + ":/sys/config");
    }

    // 4. Decide. The expected region comes from the installed Wii U Menu,
    //    never from an assumption (same rule as recovery_menu/wafel_setup_mlc).
    int mcpMenuCount = 0, mcpMenuIdx = -1;
    for (auto &m : in.menus) {
        int idx = menuRegionIdx(m.tid);
        if (idx < 0) {
            in.blockers.push_back("Unrecognised system menu title " + tidString(m.tid) + ".");
        } else {
            mcpMenuCount++;
            mcpMenuIdx = idx;
        }
    }
    int appXmlCount = 0, appXmlIdx = -1;
    for (int i = 0; i < 7; i++) {
        if (in.menuAppXml[i]) {
            appXmlCount++;
            appXmlIdx = i;
        }
    }

    if (!in.menuListOk) {
        in.blockers.push_back("Could not list installed Wii U Menu titles.");
    } else if (mcpMenuCount == 0) {
        in.blockers.push_back("MCP reports no Wii U Menu title.");
    } else if (mcpMenuCount > 1) {
        in.blockers.push_back("MORE THAN ONE Wii U Menu is installed.");
        in.blockers.push_back("PayloadLoaderInstaller only reads the first");
        in.blockers.push_back("one MCP returns: " + (in.pliMenuOk ? tidString(in.pliMenuTid) : std::string("?")) +
                              " " + (in.pliMenuOk ? titleName(in.pliMenuTid) : ""));
        in.blockers.push_back("Press + for the patched installer instructions.");
    }
    if (appXmlCount > 1) {
        in.blockers.push_back("Wii U Menu folders found for several regions");
        in.blockers.push_back("(sys/title/00050010/10040x00).");
    } else if (appXmlCount == 0 && in.menuListOk) {
        in.blockers.push_back("No Wii U Menu app.xml found on MLC.");
    }
    if (mcpMenuCount == 1 && appXmlCount == 1) {
        if (mcpMenuIdx != appXmlIdx) {
            in.blockers.push_back("MCP title list and MLC folders disagree on");
            in.blockers.push_back("the Wii U Menu region.");
        } else {
            in.installedMenuRegionIdx = mcpMenuIdx;
        }
    }
    if (in.installedMenuRegionIdx >= 0 && in.installedMenuRegionIdx != REGION_IDX_USA) {
        in.blockers.push_back(std::string("Installed Wii U Menu is ") + regionName(in.installedMenuRegionIdx) +
                              ", not USA. Refusing to change region.");
    }

    if (!in.sysProdOk) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MCP_GetSysProdSettings failed (0x%08lX).", (unsigned long) in.sysProdErr);
        in.blockers.push_back(buf);
    } else if (regionIdxFromBits(in.sysProd.product_area) < 0) {
        in.blockers.push_back("product_area is not a single known region: " +
                              regionBitsString(in.sysProd.product_area));
    }

    if (!in.systemXmlOk) {
        in.blockers.push_back("system.xml: " + in.systemXmlError);
    } else if (in.defaultTitleId != TID_MENU_USA && !isMenuTid(in.defaultTitleId)) {
        in.blockers.push_back("Coldboot title is " + tidString(in.defaultTitleId) + " (" +
                              titleName(in.defaultTitleId) + "),");
        in.blockers.push_back("not a Wii U Menu. Leaving it alone.");
    }

    if (in.installedMenuRegionIdx == REGION_IDX_USA && in.sysProdOk) {
        uint32_t usa     = 1u << REGION_IDX_USA;
        in.needRegionFix = in.sysProd.product_area != usa || in.sysProd.game_region != usa;
    }
    if (in.installedMenuRegionIdx == REGION_IDX_USA && in.systemXmlOk) {
        in.needColdbootFix = isMenuTid(in.defaultTitleId) && in.defaultTitleId != TID_MENU_USA;
    }

    if (in.pliMenuOk && in.pliMenuTid != TID_MENU_USA) {
        in.warnings.push_back("PayloadLoaderInstaller would treat " + titleName(in.pliMenuTid));
        in.warnings.push_back("as the system menu (it asks MCP, not region).");
    }
    if (in.systemXmlOk && !in.pliMenuHashMatch) {
        in.warnings.push_back("system.xml differs from what PLI expects; its");
        in.warnings.push_back("Boot options will refuse to edit it.");
    }
    if (in.pliHsTid && in.pliHsTid != TID_HS_USA) {
        in.warnings.push_back("PLI would install to " + titleName(in.pliHsTid) + ".");
    }

    for (auto &b : in.blockers) Log::write("BLOCKER: %s", b.c_str());
    for (auto &w : in.warnings) Log::write("WARNING: %s", w.c_str());
    Log::write("needRegionFix=%d needColdbootFix=%d", in.needRegionFix, in.needColdbootFix);
    return in;
}

std::vector<std::string> describePlannedChanges(const Inspection &in) {
    std::vector<std::string> l;
    if (in.needRegionFix) {
        int cur = regionIdxFromBits(in.sysProd.product_area);
        l.push_back(std::string("- Region (product_area): ") + regionName(cur) + " -> USA");
        l.push_back("- Game region (game_region): " + regionBitsString(in.sysProd.game_region) + " -> USA (0x02)");
    }
    if (in.needColdbootFix) {
        l.push_back("- Default title (system.xml default_title_id):");
        l.push_back("    " + tidString(in.defaultTitleId) + " -> " + tidString(TID_MENU_USA));
    }
    return l;
}

std::vector<std::string> describe(const Inspection &in) {
    std::vector<std::string> l;
    char buf[128];

    int cur = in.sysProdOk ? regionIdxFromBits(in.sysProd.product_area) : -1;
    snprintf(buf, sizeof(buf), "Detected console region:  %s",
             !in.sysProdOk ? "UNREADABLE" : (cur >= 0 ? regionName(cur) : "INVALID"));
    l.push_back(buf);
    if (in.sysProdOk) {
        l.push_back("  product_area: " + regionBitsString(in.sysProd.product_area));
        l.push_back("  game_region:  " + regionBitsString(in.sysProd.game_region));
    }
    snprintf(buf, sizeof(buf), "Expected region:          %s (from installed menu)",
             in.installedMenuRegionIdx >= 0 ? regionName(in.installedMenuRegionIdx) : "UNKNOWN");
    l.push_back(buf);
    l.push_back("");

    l.push_back("Current default (coldboot) title:");
    if (in.systemXmlOk) {
        l.push_back("  " + in.defaultTitleIdText + "  " + titleName(in.defaultTitleId));
    } else {
        l.push_back("  UNREADABLE");
    }
    l.push_back("Expected USA Wii U Menu:");
    l.push_back("  " + tidString(TID_MENU_USA) + "  " + titleName(TID_MENU_USA));
    l.push_back("");

    l.push_back("Installed Wii U Menu titles (MCP):");
    if (in.menus.empty()) l.push_back("  none");
    for (auto &m : in.menus) {
        l.push_back("  " + tidString(m.tid) + "  " + titleName(m.tid));
        l.push_back("    " + m.path);
    }
    std::string probe = "Menu folders on MLC:";
    for (int i = 0; i < 7; i++) {
        if (in.menuAppXml[i]) probe += std::string(" ") + regionName(i);
    }
    l.push_back(probe);
    l.push_back("");

    l.push_back("PayloadLoaderInstaller view:");
    l.push_back("  System menu: " + (in.pliMenuOk ? tidString(in.pliMenuTid) + " " + titleName(in.pliMenuTid) : std::string("query failed")));
    l.push_back("  Target app:  " + (in.pliHsTid ? titleName(in.pliHsTid) : std::string("no H&S found")));
    if (in.pliMenuHashChecked) {
        l.push_back(std::string("  system.xml check (USA menu): ") + (in.pliMenuHashMatch ? "PASS" : "FAIL"));
    }
    if (in.pliHsHashChecked) {
        l.push_back(std::string("  system.xml check (H&S):      ") + (in.pliHsHashMatch ? "PASS" : "FAIL"));
    }
    l.push_back("");

    l.push_back("Files:");
    l.push_back(std::string("  ") + SYSTEM_XML_DISPLAY);
    for (auto &p : in.sysProdXmlPaths) l.push_back("  " + displayPath(p) + " (backup only)");
    l.push_back("");

    if (!in.blockers.empty()) {
        l.push_back("PROBLEMS - repair is disabled:");
        for (auto &b : in.blockers) l.push_back("  " + b);
        l.push_back("");
    }
    if (!in.warnings.empty()) {
        l.push_back("Notes:");
        for (auto &w : in.warnings) l.push_back("  " + w);
        l.push_back("");
    }

    if (in.blockers.empty()) {
        auto changes = describePlannedChanges(in);
        if (changes.empty()) {
            l.push_back("Changes required: none. Metadata already USA.");
        } else {
            l.push_back("Changes required:");
            for (auto &c : changes) l.push_back(c);
        }
        l.push_back("");
    }
    l.push_back("No changes have been made.");
    return l;
}

// ---------------------------------------------------------------------------
// Backup
// ---------------------------------------------------------------------------

bool createBackups(const Inspection &in, std::vector<std::string> &out) {
    Log::write("--- Backup ---");
    mkdir(SD_BACKUP_DIR, 0777);
    struct stat st;
    if (stat(SD_BACKUP_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        emit(out, "Cannot create SD:/regionfix_backup/");
        return false;
    }
    std::string stamp = timestampString();

    // system.xml: must still be exactly what was inspected.
    std::vector<uint8_t> xml;
    if (!readSystemXml(xml) || xml.empty()) {
        emit(out, "Cannot read %s", SYSTEM_XML_DISPLAY);
        return false;
    }
    if (in.systemXmlOk && xml != in.systemXml) {
        emit(out, "system.xml changed since inspection. Aborting.");
        return false;
    }
    if (!backupBlob("system.xml", SYSTEM_XML_DISPLAY, xml, stamp, out)) return false;

    // Region settings exactly as MCP reports them (used by Restore).
    MCPSysProdSettings sp;
    int32_t err;
    if (!getSysProd(sp, err)) {
        emit(out, "MCP_GetSysProdSettings failed: 0x%08lX", (unsigned long) err);
        return false;
    }
    if (in.sysProdOk && memcmp(&sp, &in.sysProd, sizeof(sp)) != 0) {
        emit(out, "sys_prod settings changed since inspection. Aborting.");
        return false;
    }
    std::vector<uint8_t> spBytes((uint8_t *) &sp, (uint8_t *) &sp + sizeof(sp));
    if (!backupBlob(SYSPROD_BIN_NAME, "MCP_GetSysProdSettings", spBytes, stamp, out)) return false;

    // Raw sys_prod.xml copies, for reference / manual recovery only.
    for (size_t i = 0; i < in.sysProdXmlPaths.size(); i++) {
        std::vector<uint8_t> data;
        if (!readFile(in.sysProdXmlPaths[i], data)) {
            emit(out, "Cannot read %s", displayPath(in.sysProdXmlPaths[i]).c_str());
            return false;
        }
        std::string name = i == 0 ? "sys_prod.xml" : "sys_prod_" + std::to_string(i + 1) + ".xml";
        if (!backupBlob(name, displayPath(in.sysProdXmlPaths[i]), data, stamp, out)) return false;
    }

    emit(out, "Backup OK: SD:/regionfix_backup/");
    return true;
}

// ---------------------------------------------------------------------------
// Repair
// ---------------------------------------------------------------------------

namespace {

bool repairRegion(const Inspection &in, std::vector<std::string> &out) {
    MCPSysProdSettings cur;
    int32_t err;
    if (!getSysProd(cur, err) || memcmp(&cur, &in.sysProd, sizeof(cur)) != 0) {
        emit(out, "sys_prod settings changed or unreadable. Aborting.");
        return false;
    }

    // Same edit as recovery_menu option_DebugSystemRegion() and
    // wafel_setup_mlc fix_region(): both fields = the installed menu's bit.
    MCPSysProdSettings next = cur;
    next.product_area       = (MCPRegion) (1u << REGION_IDX_USA);
    next.game_region        = (MCPRegion) (1u << REGION_IDX_USA);

    int32_t res = setSysProd(next);
    Log::write("MCP ioctl 0x41 (SetSysProdSettings) returned 0x%08lX", (unsigned long) res);
    if (res < 0) {
        emit(out, "MCP refused the region write (0x%08lX).", (unsigned long) res);
        emit(out, "Nothing was changed. Use Recovery Menu >");
        emit(out, "Debug System Region > Fix Region instead.");
        return false;
    }

    MCPSysProdSettings check;
    if (!getSysProd(check, err)) {
        emit(out, "VERIFY FAILED: cannot re-read sys_prod.");
        return false;
    }
    if (memcmp(&check, &next, sizeof(check)) != 0) {
        emit(out, "VERIFY FAILED: sys_prod is not what was written.");
        emit(out, "  product_area=0x%02lX game_region=0x%02lX",
             (unsigned long) check.product_area, (unsigned long) check.game_region);
        return false;
    }
    emit(out, "Region verified: product_area=USA game_region=USA");
    return true;
}

bool repairColdboot(const Inspection &in, std::vector<std::string> &out) {
    std::vector<uint8_t> before;
    if (!readSystemXml(before) || before != in.systemXml) {
        emit(out, "system.xml changed or unreadable. Aborting.");
        return false;
    }

    // Only the 16 hex digits inside <default_title_id> are rewritten.
    std::string newText = tidString(TID_MENU_USA);
    Log::write("Writing '%s' over '%s' at offset %u of %s", newText.c_str(), in.defaultTitleIdText.c_str(),
               (unsigned) in.defaultTitleIdOffset, SYSTEM_XML_DISPLAY);
    bool wrote = writeSystemXml(in.defaultTitleIdOffset, newText.data(), newText.size());
    flushSlc();
    emit(out, "Write system.xml: %s", wrote ? "ok" : "FAILED");

    std::vector<uint8_t> after;
    if (!readSystemXml(after)) {
        emit(out, "VERIFY FAILED: cannot re-read system.xml.");
        return false;
    }
    bool sameSize = after.size() == before.size();
    bool othersSame = sameSize;
    for (size_t i = 0; othersSame && i < after.size(); i++) {
        bool inRange = i >= in.defaultTitleIdOffset && i < in.defaultTitleIdOffset + 16;
        if (!inRange && after[i] != before[i]) othersSame = false;
    }
    uint64_t tid = 0;
    std::string text, perr;
    size_t off = 0;
    bool parsed = parseSystemXml(after, tid, text, off, perr);
    Log::write("Verify: sameSize=%d othersUnchanged=%d parsed=%d value=%s", sameSize, othersSame, parsed, text.c_str());
    if (!sameSize || !othersSame || !parsed || tid != TID_MENU_USA) {
        emit(out, "VERIFY FAILED for system.xml.");
        if (!parsed) emit(out, "  %s", perr.c_str());
        return false;
    }
    emit(out, "Coldboot verified: %s", text.c_str());
    return wrote;
}

} // namespace

bool repair(const Inspection &planned, std::vector<std::string> &out) {
    Log::write("--- Repair requested ---");
    const Inspection in = inspect();
    if (!in.sysProdOk || !planned.sysProdOk || in.systemXml != planned.systemXml ||
        memcmp(&in.sysProd, &planned.sysProd, sizeof(in.sysProd)) != 0 ||
        in.needRegionFix != planned.needRegionFix || in.needColdbootFix != planned.needColdbootFix) {
        emit(out, "Configuration changed or could not be re-read. Inspect again before repairing.");
        return false;
    }
    if (!in.blockers.empty()) {
        emit(out, "Repair refused: problems were detected.");
        return false;
    }
    if (!in.needRegionFix && !in.needColdbootFix) {
        emit(out, "Nothing to repair.");
        return true;
    }
    for (auto &c : describePlannedChanges(in)) Log::write("Planned: %s", c.c_str());

    if (!createBackups(in, out)) {
        emit(out, "Backup failed. NOTHING was modified.");
        return false;
    }

    bool ok = true;
    if (in.needRegionFix) ok = repairRegion(in, out);
    if (ok && in.needColdbootFix) ok = repairColdboot(in, out);
    flushSlc();
    Log::write("Repair result: %s", ok ? "SUCCESS" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------

RestorePlan planRestore() {
    RestorePlan p;
    auto &l = p.lines;
    Log::write("--- Restore planning ---");

    std::string err;
    std::vector<uint8_t> xml;
    if (loadOriginal("system.xml", xml, err)) {
        uint64_t tid;
        std::string text;
        size_t off;
        std::vector<uint8_t> current;
        if (!parseSystemXml(xml, tid, text, off, err)) {
            l.push_back("system.xml backup is not valid: " + err);
        } else if (!readSystemXml(current)) {
            l.push_back("Cannot read current system.xml.");
        } else if (current == xml) {
            l.push_back("system.xml: already identical to backup.");
        } else if (current.size() != xml.size()) {
            l.push_back("system.xml size changed since the backup;");
            l.push_back("something else edited it. Not restoring.");
        } else {
            p.restoreSystemXml = true;
            p.systemXml        = xml;
            l.push_back("system.xml default_title_id -> " + text);
            l.push_back("  (" + titleName(tid) + ")");
        }
    } else {
        l.push_back("system.xml: " + err);
    }

    std::vector<uint8_t> spBytes;
    if (loadOriginal(SYSPROD_BIN_NAME, spBytes, err)) {
        MCPSysProdSettings backup, cur;
        int32_t e;
        if (spBytes.size() != sizeof(backup)) {
            l.push_back("sys_prod backup has the wrong size.");
        } else if (!getSysProd(cur, e)) {
            l.push_back("Cannot read current sys_prod settings.");
        } else {
            memcpy(&backup, spBytes.data(), sizeof(backup));
            if (!sameConsole(backup, cur)) {
                l.push_back("sys_prod backup is from a DIFFERENT console.");
                l.push_back("Not restoring region.");
            } else if (backup.product_area == cur.product_area && backup.game_region == cur.game_region) {
                l.push_back("Region: already identical to backup.");
            } else {
                p.restoreRegion = true;
                p.productArea   = backup.product_area;
                p.gameRegion    = backup.game_region;
                l.push_back("product_area -> " + regionBitsString(backup.product_area));
                l.push_back("game_region  -> " + regionBitsString(backup.game_region));
            }
        }
    } else {
        l.push_back("Region: " + err);
    }

    p.possible = p.restoreSystemXml || p.restoreRegion;
    for (auto &s : l) Log::write("Restore plan: %s", s.c_str());
    return p;
}

bool restore(const RestorePlan &plan, std::vector<std::string> &out) {
    Log::write("--- Restore confirmed ---");
    if (!plan.possible) {
        emit(out, "Nothing to restore.");
        return false;
    }

    // Snapshot the current state first, so the restore can itself be undone.
    std::string stamp = timestampString();
    std::vector<uint8_t> current;
    if (!readSystemXml(current) || !writeFileVerified(std::string(SD_BACKUP_DIR) + "/system.xml.pre-restore." + stamp, current.data(), current.size())) {
        emit(out, "Could not back up current state. Aborting.");
        return false;
    }
    MCPSysProdSettings cur;
    int32_t err;
    if (!getSysProd(cur, err) ||
        !writeFileVerified(std::string(SD_BACKUP_DIR) + "/" + SYSPROD_BIN_NAME + ".pre-restore." + stamp, &cur, sizeof(cur))) {
        emit(out, "Could not back up current region. Aborting.");
        return false;
    }

    bool ok = true;
    if (plan.restoreSystemXml) {
        if (current.size() != plan.systemXml.size()) {
            emit(out, "system.xml size changed. Not restoring.");
            ok = false;
        } else {
            bool wrote = writeSystemXml(0, plan.systemXml.data(), plan.systemXml.size());
            flushSlc();
            std::vector<uint8_t> check;
            bool verified = wrote && readSystemXml(check) && check == plan.systemXml;
            emit(out, "Restore system.xml: %s", verified ? "verified" : "FAILED");
            ok = ok && verified;
        }
    }
    if (ok && plan.restoreRegion) {
        MCPSysProdSettings next = cur;
        next.product_area       = (MCPRegion) plan.productArea;
        next.game_region        = (MCPRegion) plan.gameRegion;
        int32_t res             = setSysProd(next);
        MCPSysProdSettings check;
        bool verified = res >= 0 && getSysProd(check, err) && memcmp(&check, &next, sizeof(check)) == 0;
        emit(out, "Restore region: %s (0x%08lX)", verified ? "verified" : "FAILED", (unsigned long) res);
        ok = verified;
    }
    flushSlc();
    Log::write("Restore result: %s", ok ? "SUCCESS" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------
// Stray EUR Wii U Menu
// ---------------------------------------------------------------------------

namespace {

// Paths as MCP reports them: /vol/storage_mlc01/sys/title/00050010/10040x00
const std::string kUsaMenuDir      = std::string(MLC_MOUNT) + ":/sys/title/00050010/10040100";
const std::string kEurMenuDir      = std::string(MLC_MOUNT) + ":/sys/title/00050010/10040200";
const std::string kDisabledDir     = std::string(MLC_MOUNT) + ":/disabled_titles";
const std::string kEurDisabledDir  = kDisabledDir + "/10040200";

bool isDir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void check(EurMenuPlan &p, bool &allOk, bool ok, const std::string &what) {
    p.lines.push_back(std::string(ok ? "[ OK ] " : "[FAIL] ") + what);
    allOk = allOk && ok;
}

} // namespace

EurMenuPlan planEurMenu(const Inspection &in) {
    EurMenuPlan p;
    bool eurActive   = isDir(kEurMenuDir);
    bool eurDisabled = isDir(kEurDisabledDir);

    if (!eurActive && eurDisabled) {
        p.lines.push_back("The EUR Wii U Menu is currently moved aside to");
        p.lines.push_back("/vol/storage_mlc01/disabled_titles/10040200");
        p.lines.push_back("Automatic restore is unavailable: the MLC rename");
        p.lines.push_back("has no verified undo. Use recovery_menu to inspect.");
        return p;
    }

    bool ok = true;
    p.lines.push_back("Duplicate-menu diagnostics (read only):");
    check(p, ok, in.systemXmlOk, "system.xml readable");
    check(p, ok, in.systemXmlOk && in.defaultTitleId == TID_MENU_USA,
          "Coldboot title is USA menu (" + (in.systemXmlOk ? in.defaultTitleIdText : std::string("?")) + ")");
    check(p, ok, in.sysProdOk && in.sysProd.product_area == MCP_REGION_USA &&
          in.sysProd.game_region == MCP_REGION_USA, "Product area AND game region are USA");
    bool usaListed = false;
    for (auto &m : in.menus) usaListed |= (m.tid == TID_MENU_USA &&
        m.path == "/vol/storage_mlc01/sys/title/00050010/10040100");
    check(p, ok, usaListed, "USA menu registered with MCP");
    for (const char *f : {"/code/app.xml", "/code/cos.xml", "/code/title.tmd"}) {
        check(p, ok, fileExists(kUsaMenuDir + f), std::string("USA menu") + f + " present");
    }
    check(p, ok, eurActive, "EUR menu folder present in sys/title");
    check(p, ok, !eurDisabled, "disabled_titles/10040200 not already used");

    p.lines.push_back("");
    p.lines.push_back("Folder moves are disabled. The console rejected");
    p.lines.push_back("the previous rename (errno 91 / invalid path).");
    p.lines.push_back("");
    p.lines.push_back("Use PayloadLoaderInstaller-RegionFix.wuhb.");
    p.lines.push_back("It selects the menu and H&S by console region.");
    p.lines.push_back("Both Wii U Menu installations can stay in place.");
    if (!ok) p.lines.push_back("Resolve the failed checks before changing coldboot.");
    for (auto &l : p.lines) Log::write("EUR plan: %s", l.c_str());
    return p;
}
