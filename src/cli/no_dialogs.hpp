#pragma once

// Windows: report CRT assertions, abort() and crashes on stderr instead of modal dialogs, so unattended runs
// (tests, Git drivers, CI) fail fast without blocking the desktop. No effect elsewhere.

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <initializer_list>
#include <windows.h>
#endif

namespace splinter::cli {

inline void no_dialogs() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG // the CRT report functions exist only in the debug CRT
    for (int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

} // namespace splinter::cli
