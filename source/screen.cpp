#include "screen.h"

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>

namespace {

void *sTvBuffer      = nullptr;
void *sDrcBuffer     = nullptr;
uint32_t sTvSize     = 0;
uint32_t sDrcSize    = 0;

// The GamePad is the smaller screen, so layout is sized for it.
constexpr int kTotalRows  = 17;
constexpr int kHeaderRows = 2;
constexpr int kFooterRows = 3;
constexpr int kMaxCols    = 64;

void putLine(int row, const std::string &text) {
    std::string clipped = text.substr(0, kMaxCols);
    OSScreenPutFontEx(SCREEN_TV, 0, row, clipped.c_str());
    OSScreenPutFontEx(SCREEN_DRC, 0, row, clipped.c_str());
}

} // namespace

namespace Screen {

bool init() {
    OSScreenInit();
    sTvSize  = OSScreenGetBufferSizeEx(SCREEN_TV);
    sDrcSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

    sTvBuffer  = MEMAllocFromDefaultHeapEx(sTvSize, 0x100);
    sDrcBuffer = MEMAllocFromDefaultHeapEx(sDrcSize, 0x100);
    if (!sTvBuffer || !sDrcBuffer) {
        shutdown();
        return false;
    }

    OSScreenSetBufferEx(SCREEN_TV, sTvBuffer);
    OSScreenSetBufferEx(SCREEN_DRC, sDrcBuffer);
    OSScreenEnableEx(SCREEN_TV, TRUE);
    OSScreenEnableEx(SCREEN_DRC, TRUE);
    return true;
}

void shutdown() {
    if (sTvBuffer) {
        MEMFreeToDefaultHeap(sTvBuffer);
        sTvBuffer = nullptr;
    }
    if (sDrcBuffer) {
        MEMFreeToDefaultHeap(sDrcBuffer);
        sDrcBuffer = nullptr;
    }
    OSScreenShutdown();
}

int bodyRows() {
    return kTotalRows - kHeaderRows - kFooterRows;
}

void draw(const std::string &title,
          const std::vector<std::string> &body,
          const std::vector<std::string> &footer,
          int &scroll) {
    OSScreenClearBufferEx(SCREEN_TV, 0x00000000);
    OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);

    putLine(0, " " + title);
    putLine(1, std::string(kMaxCols - 4, '='));

    int rows      = bodyRows();
    int maxScroll = (int) body.size() > rows ? (int) body.size() - rows : 0;
    if (scroll < 0) scroll = 0;
    if (scroll > maxScroll) scroll = maxScroll;

    for (int i = 0; i < rows && scroll + i < (int) body.size(); i++) {
        putLine(kHeaderRows + i, body[scroll + i]);
    }

    int footerStart = kTotalRows - kFooterRows;
    std::string sep = std::string(kMaxCols - 4, '-');
    if (maxScroll > 0) {
        sep = "-- [Up/Down] scroll (" + std::to_string(scroll + 1) + "/" + std::to_string(maxScroll + 1) + ") ";
        sep += std::string(sep.size() < kMaxCols - 4 ? kMaxCols - 4 - sep.size() : 0, '-');
    }
    putLine(footerStart, sep);
    for (int i = 0; i < kFooterRows - 1 && i < (int) footer.size(); i++) {
        putLine(footerStart + 1 + i, footer[i]);
    }

    DCFlushRange(sTvBuffer, sTvSize);
    DCFlushRange(sDrcBuffer, sDrcSize);
    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
}

} // namespace Screen
