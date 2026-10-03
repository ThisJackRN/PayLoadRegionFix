#include "log.h"

#include <coreinit/time.h>
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace {
FILE *sLog = nullptr;
}

std::string timestampString() {
    OSCalendarTime ct;
    OSTicksToCalendarTime(OSGetTime(), &ct);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
             ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec);
    return buf;
}

namespace Log {

void open() {
    if (sLog) return;
    mkdir(SD_BACKUP_DIR, 0777);
    sLog = fopen(SD_LOG_PATH, "a");
    if (sLog) {
        fprintf(sLog, "\n===== regionfix session %s =====\n", timestampString().c_str());
        fflush(sLog);
    }
}

void close() {
    if (!sLog) return;
    fflush(sLog);
    fsync(fileno(sLog));
    fclose(sLog);
    sLog = nullptr;
}

void write(const char *fmt, ...) {
    if (!sLog) return;
    OSCalendarTime ct;
    OSTicksToCalendarTime(OSGetTime(), &ct);
    fprintf(sLog, "[%02d:%02d:%02d] ", ct.tm_hour, ct.tm_min, ct.tm_sec);
    va_list args;
    va_start(args, fmt);
    vfprintf(sLog, fmt, args);
    va_end(args);
    fputc('\n', sLog);
    // Keep the log useful even if the console hangs or loses power.
    fflush(sLog);
    fsync(fileno(sLog));
}

} // namespace Log
