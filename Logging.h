#pragma once
// Logging.h — no local log file (removed; was unconditional temp-folder
// spam with no rotation/toggle, not needed now that SteamSwitcher's debug
// log pipe is the real sink). Forwards every line to SteamSwitcher.exe's
// optional debug-log pipe via RemoteLog.h - see that file for why this is
// safe to call unconditionally (near-zero cost when nobody's listening);
// the checkbox on the SteamSwitcher side controls display, not delivery.

#include <Windows.h>
#include <cstdio>
#include "RemoteLog.h"

namespace Logging {

    inline void LogFmt(const char* fmt, ...) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        char msg[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char line[1088];
        snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] pid=%lu %s",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), msg);

        RemoteLog::Send(line);
    }

} // namespace Logging