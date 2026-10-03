#include <coreinit/debug.h>

#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/proc.h>

#include "InstallerService.h"
#include "utils/WiiUScreen.h"
#include <input/CombinedInput.h>
#include <input/VPADInput.h>
#include <input/WPADInput.h>
#include <iosuhax.h>
#include <iosuhax_devoptab.h>
#include <string_view>
#include <cerrno>

#include "safe_payload.h"
#include "Diagnostics.h"
#include "utils/StringTools.h"
#ifdef REGIONFIX_DIAGNOSTIC_ONLY
#include "DiagnosticState.h"
#else
#include "ApplicationState.h"
#endif

constexpr bool strings_equal(char const *a, char const *b) {
    return std::string_view(a) == b;
}

static_assert(strings_equal(RPX_HASH, "1736574cf6c949557aed0c817eb1927e35a9b820"), "Built with an untested root.rpx! Remove this check if you really know what you're doing.");

void initIOSUHax();

void deInitIOSUHax();

int sFSAFd         = -1;
bool sIosuhaxMount = false;
static bool sIosuhaxOpen = false;
static bool sSlcMounted = false;
static bool sMlcMounted = false;

int main_loop() {
    DEBUG_FUNCTION_LINE("Creating state");
#ifdef REGIONFIX_DIAGNOSTIC_ONLY
    WiiUScreen::clearScreen();
    WiiUScreen::drawLine("PLI RegionFix Diagnostic v2 - reading only...");
    WiiUScreen::flipBuffers();
    DiagnosticState state;
#else
    ApplicationState state;
#endif
    CombinedInput baseInput;
    VPadInput vpadInput;
    WPADInput wpadInputs[4] = {
            WPAD_CHAN_0,
            WPAD_CHAN_1,
            WPAD_CHAN_2,
            WPAD_CHAN_3};

#ifndef REGIONFIX_DIAGNOSTIC_ONLY
    if (sFSAFd < 0 || !sIosuhaxMount) {
        state.setError(ApplicationState::eErrorState::ERROR_IOSUHAX_FAILED);
    }
#endif

    DEBUG_FUNCTION_LINE("Entering main loop");
    while (WHBProcIsRunning()) {
        baseInput.reset();
        if (vpadInput.update(1280, 720)) {
            baseInput.combine(vpadInput);
        }
        for (auto &wpadInput : wpadInputs) {
            if (wpadInput.update(1280, 720)) {
                baseInput.combine(wpadInput);
            }
        }
        baseInput.process();
        state.update(&baseInput);
        state.render();
    }

    return 0;
}

int main(int argc, char **argv) {
    WHBLogUdpInit();
    DEBUG_FUNCTION_LINE("Hello from Payload-Loader Installer!");
    WHBProcInit();
    WiiUScreen::Init();

#ifdef REGIONFIX_DIAGNOSTIC_ONLY
    Diagnostics::begin();
#endif
    initIOSUHax();

    WPADInput::init();

    main_loop();

    WPADInput::close();

    deInitIOSUHax();

    WiiUScreen::DeInit();
    WHBProcShutdown();

    return 0;
}

void initIOSUHax() {
    sIosuhaxMount = false;
    int res       = IOSUHAX_Open(nullptr);
    Diagnostics::record("IOSUHAX_Open=" + std::to_string(res));
    if (res < 0) {
        DEBUG_FUNCTION_LINE("IOSUHAX_open failed");
#ifdef REGIONFIX_DIAGNOSTIC_ONLY
        Diagnostics::fail("IOSUHAX_Open failed; CFW access unavailable");
#else
        OSFatal("IOSUHAX_open failed, please start this installer with an CFW");
#endif
    } else {
        sIosuhaxOpen = true;
        sFSAFd        = IOSUHAX_FSA_Open();
        Diagnostics::record("IOSUHAX_FSA_Open=" + std::to_string(sFSAFd));
        if (sFSAFd < 0) {
            Diagnostics::fail("IOSUHAX_FSA_Open failed");
            DEBUG_FUNCTION_LINE("IOSUHAX_FSA_Open failed");
        } else {
            errno = 0;
            const int slc = mount_fs("storage_slc_installer", sFSAFd, nullptr, "/vol/system");
            const int slcError = errno;
            sSlcMounted = (slc == 0);
            errno = 0;
            const int mlc = mount_fs("storage_mlc_installer", sFSAFd, nullptr, "/vol/storage_mlc01");
            const int mlcError = errno;
            sMlcMounted = (mlc == 0);
            sIosuhaxMount = sSlcMounted && sMlcMounted;
            Diagnostics::record(StringTools::strfmt("mount_fs SLC=%d errno=%d MLC=%d errno=%d", slc, slcError, mlc, mlcError));
            if (!sIosuhaxMount) Diagnostics::fail("Virtual mount registration failed; selector blocked");
        }
        DEBUG_FUNCTION_LINE("IOSUHAX done");
    }
}

void deInitIOSUHax() {
    if (sIosuhaxOpen) {
        // Never unregister a device we failed to register (e.g. a duplicate).
        if (sSlcMounted) unmount_fs("storage_slc_installer");
        if (sMlcMounted) unmount_fs("storage_mlc_installer");
        if (sFSAFd >= 0) {
            IOSUHAX_FSA_Close(sFSAFd);
        }
        IOSUHAX_Close();
    }
    sIosuhaxMount = sIosuhaxOpen = sSlcMounted = sMlcMounted = false;
    sFSAFd = -1;
}
