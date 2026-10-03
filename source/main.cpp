#include "log.h"
#include "regionfix.h"
#include "screen.h"

#include <coreinit/launch.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/proc.h>

namespace {

constexpr const char *kTitle = "Wii U Region Repair Utility";

enum class State {
    Fatal,
    Main,
    ConfirmRepair,
    ConfirmRestore1,
    ConfirmRestore2,
    EurMenu,
    Result,
    Exiting,
};

struct App {
    State state = State::Main;
    Inspection in;
    RestorePlan restorePlan;
    EurMenuPlan eurPlan;
    std::vector<std::string> resultLines;
    bool lastOk          = false;
    bool offerRestore    = false; // after a failed repair verification
    bool offerShutdown   = false; // after a successful write
    int scroll           = 0;
    std::string fatalError;
};

void drawBusy(const std::string &what) {
    int s = 0;
    Screen::draw(kTitle, {"", what, "", "Do NOT power off the console."}, {}, s);
}

void goTo(App &app, State s) {
    app.state  = s;
    app.scroll = 0;
}

void reinspect(App &app) {
    drawBusy("Inspecting...");
    app.in = inspect();
}

void handleMain(App &app, uint32_t trig) {
    if (trig & VPAD_BUTTON_A) {
        if (!app.in.blockers.empty() || (!app.in.needRegionFix && !app.in.needColdbootFix)) {
            app.resultLines = {app.in.blockers.empty() ? "Nothing needs repairing." : "Repair is disabled because problems were found.",
                               "See the main screen and SD:/regionfix_backup/regionfix.log."};
            app.lastOk = app.offerRestore = app.offerShutdown = false;
            goTo(app, State::Result);
        } else {
            goTo(app, State::ConfirmRepair);
        }
    } else if (trig & VPAD_BUTTON_X) {
        drawBusy("Creating backup...");
        app.resultLines.clear();
        app.lastOk = createBackups(app.in, app.resultLines);
        app.resultLines.push_back(app.lastOk ? "Backup complete. Nothing was modified." : "BACKUP FAILED. Nothing was modified.");
        app.offerRestore = app.offerShutdown = false;
        goTo(app, State::Result);
    } else if (trig & VPAD_BUTTON_Y) {
        drawBusy("Reading backups...");
        app.restorePlan = planRestore();
        goTo(app, State::ConfirmRestore1);
    } else if (trig & VPAD_BUTTON_PLUS) {
        drawBusy("Checking...");
        app.in = inspect();
        app.eurPlan = planEurMenu(app.in);
        goTo(app, State::EurMenu);
    } else if (trig & VPAD_BUTTON_B) {
        goTo(app, State::Exiting);
        SYSLaunchMenu();
    }
}

void handleEurMenu(App &app, uint32_t trig) {
    if (trig & VPAD_BUTTON_B) {
        goTo(app, State::Main);
    }
}

void handleConfirmRepair(App &app, uint32_t trig) {
    if (trig & VPAD_BUTTON_A) {
        drawBusy("Backing up and repairing...");
        OSEnableHomeButtonMenu(FALSE);
        app.resultLines.clear();
        app.lastOk = repair(app.in, app.resultLines);
        app.resultLines.push_back("");
        if (app.lastOk) {
            app.resultLines.push_back("Repair successful.");
            app.resultLines.push_back("");
            app.resultLines.push_back("Region: USA");
            app.resultLines.push_back("Wii U Menu: " + tidString(TID_MENU_USA));
            app.resultLines.push_back("");
            app.resultLines.push_back("Reboot the console and verify the normal Wii U Menu");
            app.resultLines.push_back("boots before using PayloadLoaderInstaller.");
        } else {
            app.resultLines.push_back("REPAIR DID NOT COMPLETE.");
            app.resultLines.push_back("Backups (if made) are in SD:/regionfix_backup/.");
            app.resultLines.push_back("Press Y to review a restore from backup.");
        }
        app.offerRestore  = !app.lastOk;
        app.offerShutdown = app.lastOk;
        goTo(app, State::Result);
    } else if (trig & VPAD_BUTTON_B) {
        goTo(app, State::Main);
    }
}

void handleConfirmRestore1(App &app, uint32_t trig) {
    if ((trig & VPAD_BUTTON_A) && app.restorePlan.possible) {
        goTo(app, State::ConfirmRestore2);
    } else if (trig & VPAD_BUTTON_B) {
        goTo(app, State::Main);
    }
}

void handleConfirmRestore2(App &app, uint32_t trig) {
    if (trig & VPAD_BUTTON_X) {
        drawBusy("Restoring backup...");
        OSEnableHomeButtonMenu(FALSE);
        app.resultLines.clear();
        app.lastOk = restore(app.restorePlan, app.resultLines);
        app.resultLines.push_back("");
        app.resultLines.push_back(app.lastOk ? "Restore complete." : "RESTORE DID NOT COMPLETE. See regionfix.log.");
        app.offerRestore  = false;
        app.offerShutdown = app.lastOk;
        goTo(app, State::Result);
    } else if (trig & VPAD_BUTTON_B) {
        goTo(app, State::Main);
    }
}

void handleResult(App &app, uint32_t trig) {
    if ((trig & VPAD_BUTTON_Y) && app.offerRestore) {
        drawBusy("Reading backups...");
        app.restorePlan = planRestore();
        goTo(app, State::ConfirmRestore1);
    } else if ((trig & VPAD_BUTTON_X) && app.offerShutdown) {
        drawBusy("Shutting down...");
        unmountSystem();
        Log::close();
        OSShutdown();
        goTo(app, State::Exiting);
    } else if (trig & (VPAD_BUTTON_A | VPAD_BUTTON_B)) {
        reinspect(app);
        goTo(app, State::Main);
    }
}

void render(App &app) {
    std::vector<std::string> body, footer;
    switch (app.state) {
        case State::Fatal:
            body = {"Cannot inspect this console:", "", app.fatalError, "", "Nothing was modified."};
            footer = {"[B] Exit"};
            break;
        case State::Main:
            body   = describe(app.in);
            footer = {"[A] Backup+Repair  [X] Backup only  [Y] Restore",
                      "[+] Duplicate-menu help  [B] Exit"};
            break;
        case State::ConfirmRepair:
            body = {"The following will be changed:", ""};
            for (auto &c : describePlannedChanges(app.in)) body.push_back(c);
            body.push_back("");
            body.push_back("A backup is written to SD:/regionfix_backup/");
            body.push_back("first. If the backup fails, nothing is changed.");
            footer = {"[A] Confirm: backup + repair", "[B] Cancel"};
            break;
        case State::ConfirmRestore1:
            body = {"Restore previous configuration from", "SD:/regionfix_backup/*.original:", ""};
            for (auto &s : app.restorePlan.lines) body.push_back(s);
            body.push_back("");
            body.push_back(app.restorePlan.possible ? "Continue to final confirmation?" : "Nothing can be restored.");
            footer = {app.restorePlan.possible ? "[A] Continue   [B] Cancel" : "[B] Back"};
            break;
        case State::ConfirmRestore2:
            body = {"FINAL CONFIRMATION", "", "This overwrites the current configuration", "with the backed-up values listed before.", "",
                    "The current state is saved as *.pre-restore.* first."};
            footer = {"[X] Yes, restore now", "[B] Cancel"};
            break;
        case State::EurMenu:
            body = app.eurPlan.lines;
            footer = {"[B] Back"};
            break;
        case State::Result:
            body = app.resultLines;
            if (app.offerShutdown) {
                footer = {"[X] Shut down now (recommended)", "[A] Back to main screen"};
            } else if (app.offerRestore) {
                footer = {"[Y] Restore backup", "[A] Back to main screen"};
            } else {
                footer = {"[A] Back to main screen"};
            }
            break;
        case State::Exiting:
            body = {"Exiting..."};
            break;
    }
    Screen::draw(kTitle, body, footer, app.scroll);
}

} // namespace

int main(int argc, char **argv) {
    WHBProcInit();
    // Keep the HOME menu from interrupting a write; B exits normally.
    OSEnableHomeButtonMenu(FALSE);

    if (!Screen::init()) {
        WHBProcShutdown();
        return -1;
    }
    Log::open();

    App app;
    drawBusy("Mounting system storage (read-only inspection)...");
    std::string err;
    if (!mountSystem(err)) {
        app.fatalError = err;
        Log::write("FATAL: %s", err.c_str());
        goTo(app, State::Fatal);
    } else {
        reinspect(app);
        goTo(app, State::Main);
    }

    while (WHBProcIsRunning()) {
        VPADStatus vpad{};
        VPADReadError verr;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        uint32_t trig = (verr == VPAD_READ_SUCCESS) ? vpad.trigger : 0;

        if (trig & VPAD_BUTTON_UP) app.scroll--;
        if (trig & VPAD_BUTTON_DOWN) app.scroll++;

        switch (app.state) {
            case State::Fatal:
                if (trig & VPAD_BUTTON_B) {
                    goTo(app, State::Exiting);
                    SYSLaunchMenu();
                }
                break;
            case State::Main: handleMain(app, trig); break;
            case State::ConfirmRepair: handleConfirmRepair(app, trig); break;
            case State::ConfirmRestore1: handleConfirmRestore1(app, trig); break;
            case State::ConfirmRestore2: handleConfirmRestore2(app, trig); break;
            case State::EurMenu: handleEurMenu(app, trig); break;
            case State::Result: handleResult(app, trig); break;
            case State::Exiting: break;
        }

        render(app);
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    unmountSystem();
    Log::close();
    Screen::shutdown();
    WHBProcShutdown();
    return 0;
}
