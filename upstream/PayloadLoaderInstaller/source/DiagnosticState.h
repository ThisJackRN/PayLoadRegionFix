#pragma once
#include "Menu.h"
#include <vector>

// Separate UI: deliberately has no install/restore/boot-write states.
class DiagnosticState {
public:
    DiagnosticState();
    void update(Input *input) { menu.update(input); }
    void render() { menu.render(); }
private:
    void showPage();
    Menu<int> menu;
    std::vector<std::string> rows;
    size_t page = 0;
};
