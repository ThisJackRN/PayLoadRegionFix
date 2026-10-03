// Read-only reproduction of PLI's XML normalization, using its actual vendors.
#include "../upstream/PayloadLoaderInstaller/source/utils/pugixml.hpp"
#include "../upstream/PayloadLoaderInstaller/source/utils/TinySHA1.hpp"
#include "../upstream/PayloadLoaderInstaller/source/common/TitleSelection.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

std::string hash(const std::string &bytes) {
    sha1::SHA1 s;
    s.processBytes(bytes.data(), bytes.size());
    uint32_t digest[5];
    s.getDigest(digest);
    char text[41];
    std::snprintf(text, sizeof(text), "%08X%08X%08X%08X%08X",
                  digest[0], digest[1], digest[2], digest[3], digest[4]);
    return text;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    assert(input);
    const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    assert(original.size() == 1570);
    assert(hash(original) == "2C7406690B0BEDFD6E633B1C1EFB451244EDBDFC");
    struct Case { const char *tid; const char *hash1; const char *hash2; };
    // Unchanged allowlist from upstream source/common/common.cpp.
    for (const auto &test : {
            Case{"0005001010040100", "124562D41A02C7112DDD5F9A8F0EE5DF97E23471", "DC0F9941E99C629625419F444B5A5B177A67309F"},
            Case{"000500101004E100", "0EBCA1DFC0AB7A6A7FE8FB5EAF23179621B726A1", "83CF5B1CE0B64C51D15B1EFCAD659063790EB590"}}) {
        pugi::xml_document doc;
        assert(doc.load_buffer(original.data(), original.size()));
        auto system = doc.child("system");
        auto title = system.child("default_title_id");
        assert(system && !system.next_sibling("system"));
        assert(title && !title.next_sibling("default_title_id"));
        assert(TitleSelection::parseTitleId(title.child_value()) == 0x0005001010040100ULL);
        assert(title.first_child().set_value(test.tid));
        for (const char *name : {"log", "standby", "ramdisk"}) {
            if (!system.child(name).attribute("length")) system.child(name).append_attribute("length") = "0";
        }
        std::stringstream serialized;
        doc.save(serialized, "  ", pugi::format_default, pugi::encoding_utf8);
        const auto digest = hash(serialized.str());
        assert(digest == test.hash1 || digest == test.hash2);
        std::cout << test.tid << ": original PLI normalized hash gate passes (" << digest << ").\n";
    }
}
