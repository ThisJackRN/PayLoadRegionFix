#pragma once

#include <string>

// Appends to SD:/regionfix_backup/regionfix.log. Never pass console-unique
// secrets (OTP/SEEPROM keys, serial numbers) to these functions.
namespace Log {

void open();
void close();
void write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace Log

// SD card paths (WUT mounts the SD card as "fs:/vol/external01").
#define SD_BACKUP_DIR "fs:/vol/external01/regionfix_backup"
#define SD_LOG_PATH   SD_BACKUP_DIR "/regionfix.log"

// "YYYYMMDD-HHMMSS" from the console clock.
std::string timestampString();
