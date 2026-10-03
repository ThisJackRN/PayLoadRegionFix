#include "DiagnosticState.h"
#include "Diagnostics.h"
#include "InstallerService.h"
#include "common/TitleSelection.h"
#include "utils/StringTools.h"
#include <iosuhax.h>
#include <sysapp/launch.h>

extern int sFSAFd;
extern bool sIosuhaxMount;

DiagnosticState::DiagnosticState() {
    if (sFSAFd >= 0 && sIosuhaxMount) {
        // Exercise the SAME selector and guards as the installer, without
        // entering ApplicationState or calling any mutation function.
        const auto app = InstallerService::getInstalledAppInformation();
        if (app) Diagnostics::record(StringTools::strfmt("INSTALLER SELECTION PASS: %016llX", app->titleId));
        else if (Diagnostics::failure().empty()) Diagnostics::fail("Selector returned no app without a detailed reason");
    } else Diagnostics::fail("IOSUHAX/FSA or virtual mounts unavailable; selector not run");

    // Independent raw-stat probes help distinguish an IOSU path/permission
    // failure from a devoptab translation problem. These never open for write.
    if (sFSAFd >= 0) {
        Diagnostics::record("Supplementary raw IOSUHAX stat probes (USA paths):");
        for (const uint64_t tid : {0x0005001010040100ULL, 0x000500101004E100ULL}) {
            for (const char *file : {"/code/app.xml", "/code/cos.xml", "/code/title.tmd"}) {
                const auto path = TitleSelection::mlcPath(tid) + file;
                FSStat st{};
                const int rc = IOSUHAX_FSA_GetStat(sFSAFd, path.c_str(), &st);
                Diagnostics::record("raw stat: " + path);
                Diagnostics::record(StringTools::strfmt("rc=%d flags=0x%08X mode=0x%08X size=%u",
                    rc, static_cast<unsigned>(st.flags), static_cast<unsigned>(st.mode), static_cast<unsigned>(st.size)));
            }
        }
        for (const char *path : {"/vol/system/config/system.xml", "/vol/storage_slc01/sys/config/system.xml"}) {
            FSStat st{};
            const int rc = IOSUHAX_FSA_GetStat(sFSAFd, path, &st);
            Diagnostics::record("raw stat: " + std::string(path));
            Diagnostics::record(StringTools::strfmt("rc=%d flags=0x%08X size=%u", rc,
                static_cast<unsigned>(st.flags), static_cast<unsigned>(st.size)));
        }
        pugi::xml_document doc;
        const auto loaded = doc.load_file("storage_slc_installer:/config/system.xml");
        if (loaded) {
            const auto tid = TitleSelection::parseTitleId(doc.child("system").child("default_title_id").child_value());
            Diagnostics::record(tid ? StringTools::strfmt("Read-only coldboot=%016llX", *tid) : "Read-only coldboot: malformed/missing title ID");
        } else Diagnostics::record(std::string("Read-only system.xml: ") + loaded.description());
    }
    Diagnostics::record("END: no install, restore, title move or coldboot write was run.");
    const bool saved = Diagnostics::saveToSD();

    // Wrap at 56 columns and paginate for the smaller GamePad screen.
    auto append = [&](const std::string &text) {
        if (text.empty()) rows.emplace_back();
        for (size_t start = 0; start < text.size(); start += 56) rows.push_back(text.substr(start, 56));
    };
    append(Diagnostics::failure().empty() ? "Initial title-selection checks PASSED." : "FIRST FAILURE: " + Diagnostics::failure());
    append("No console-storage writes were performed.");
    append(saved ? "Log saved at SD:/regionfix-pli-diagnostic.log" : "SD log save FAILED. Photograph the report pages.");
    for (const auto &line : Diagnostics::lines()) append(line);
    menu.setHeader("PLI RegionFix Diagnostic v2 - READ ONLY");
    menu.setFooter("Up/Down: select   A: activate");
    menu.setOptionsCallback([this](int action) {
        const auto pages = (rows.size() + 7) / 8;
        if (action == 0) { SYSLaunchMenu(); return; }
        if (action == 1 && page + 1 < pages) ++page;
        if (action == -1 && page > 0) --page;
        showPage();
    });
    showPage();
}

void DiagnosticState::showPage() {
    menu.clear();
    menu.addText(StringTools::strfmt("Report page %u / %u", static_cast<unsigned>(page + 1), static_cast<unsigned>((rows.size() + 7) / 8)));
    for (size_t i = page * 8; i < rows.size() && i < (page + 1) * 8; ++i) menu.addText(rows[i]);
    menu.addText();
    menu.addOption("Next page", 1);
    menu.addOption("Previous page", -1);
    menu.addOption("Return to Wii U Menu", 0);
}
