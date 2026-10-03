#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

// Pure policy shared by the installer and the host regression tests.
namespace TitleSelection {
inline std::optional<uint64_t> parseTitleId(const char *value) {
    if (!value || std::strlen(value) != 16) return {};
    uint64_t result = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        unsigned digit;
        if (value[i] >= '0' && value[i] <= '9') digit = value[i] - '0';
        else if (value[i] >= 'a' && value[i] <= 'f') digit = value[i] - 'a' + 10;
        else if (value[i] >= 'A' && value[i] <= 'F') digit = value[i] - 'A' + 10;
        else return {};
        result = (result << 4) | digit;
    }
    return result;
}

inline std::optional<uint64_t> menuForRegion(uint32_t product, uint32_t game) {
    if (product != game) return {};
    switch (product) {
        case 1: return 0x0005001010040000ULL;
        case 2: return 0x0005001010040100ULL;
        case 4: return 0x0005001010040200ULL;
        default: return {};
    }
}

inline uint64_t healthAndSafety(uint64_t menu) {
    return menu + 0xE000;
}

inline std::string mlcPath(uint64_t tid) {
    char path[56];
    std::snprintf(path, sizeof(path), "/vol/storage_mlc01/sys/title/%08x/%08x",
                  static_cast<unsigned>(tid >> 32), static_cast<unsigned>(tid));
    return path;
}

inline std::string mountedPath(uint64_t tid) {
    return "storage_mlc_installer:" + mlcPath(tid).substr(std::strlen("/vol/storage_mlc01"));
}

template<typename Entry>
std::optional<std::size_t> findUnique(const Entry *entries, std::size_t count,
                                    uint64_t expected, uint32_t appType) {
    std::optional<std::size_t> selected;
    const auto path = mlcPath(expected);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        if (entry.titleId != expected) continue;
        if (selected || static_cast<uint32_t>(entry.appType) != appType ||
            path.size() >= sizeof(entry.path) ||
            std::memcmp(entry.path, path.c_str(), path.size() + 1) != 0) return {};
        selected = i;
    }
    return selected;
}

inline bool knownColdboot(uint64_t current, uint64_t menu) {
    return current == menu || current == healthAndSafety(menu);
}
} // namespace TitleSelection
