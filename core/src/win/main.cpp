// OpenSweepCore.exe entry point.
//   (no args)      run as the Windows service (started by the SCM)
//   --console      run the pipe server in the foreground until Ctrl+C  [--verbose]
//   --install      create the OpenSweepCore service (elevated prompt required)
//   --uninstall    stop and delete the service
//   --version      print version and exit
#include <windows.h>

#include <cstdio>
#include <string_view>

#include "app.hpp"
#include "commands.hpp"
#include "log.hpp"
#include "protocol.hpp"
#include "service.hpp"

namespace {

using namespace opensweep;

HANDLE g_console_stop = nullptr;
HANDLE g_console_done = nullptr;

// Process-wide hardening using documented APIs. Failures are ignored (best effort).
void harden_process() {
    // Terminate on heap corruption instead of continuing in an undefined state.
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    // DLL planting: only search System32 for dynamically loaded DLLs and drop the CWD.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
    // Refuse images from remote (UNC) locations or with a low mandatory label.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY image{};
    image.NoRemoteImages = 1;
    image.NoLowMandatoryLabelImages = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &image, sizeof(image));
    // Block legacy extension points (AppInit DLLs, winsock LSPs, global hooks, IMEs).
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));
}

BOOL WINAPI console_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            if (g_console_stop) SetEvent(g_console_stop);
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            // The process is terminated when this handler returns: give the core time to stop.
            if (g_console_stop) SetEvent(g_console_stop);
            if (g_console_done) WaitForSingleObject(g_console_done, 4000);
            return TRUE;
        default:
            return FALSE;
    }
}

int run_console() {
    g_console_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_console_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_console_stop || !g_console_done) {
        std::fprintf(stderr, "error: CreateEvent failed\n");
        return 1;
    }
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::fprintf(stderr, "error: SetConsoleCtrlHandler failed\n");
        return 1;
    }
    const int code = win::run_core(g_console_stop, [] {
        std::fprintf(stderr, "Listening on \\\\.\\pipe\\OpenSweepCore.v1 (mode: monitor). Press Ctrl+C to stop.\n");
    });
    SetEvent(g_console_done);
    return code;
}

void print_usage() {
    std::printf(
        "OpenSweepCore %s - OpenSweep Core monitoring service (IPC protocol %lld)\n"
        "\n"
        "Usage:\n"
        "  OpenSweepCore.exe                 run as a Windows service (started by the SCM)\n"
        "  OpenSweepCore.exe --console [--verbose]\n"
        "                                    run the pipe server in the foreground until Ctrl+C\n"
        "  OpenSweepCore.exe --install       install the OpenSweepCore service (elevated)\n"
        "  OpenSweepCore.exe --uninstall     stop and remove the service (elevated)\n"
        "  OpenSweepCore.exe --version       print the version\n",
        commands::kVersion, static_cast<long long>(protocol::kProtocolVersion));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    harden_process();

    if (argc <= 1) {
        opensweep::log::init(false, false);
        const int code = opensweep::win::run_service();
        opensweep::log::shutdown();
        return code;
    }

    const std::wstring_view cmd = argv[1];
    if (cmd == L"--version" && argc == 2) {
        std::printf("OpenSweepCore %s (protocol %lld)\n", opensweep::commands::kVersion,
                    static_cast<long long>(opensweep::protocol::kProtocolVersion));
        return 0;
    }
    if ((cmd == L"--help" || cmd == L"-h" || cmd == L"/?") && argc == 2) {
        print_usage();
        return 0;
    }
    if (cmd == L"--console" && (argc == 2 || (argc == 3 && std::wstring_view(argv[2]) == L"--verbose"))) {
        opensweep::log::init(true, argc == 3);
        const int code = run_console();
        opensweep::log::shutdown();
        return code;
    }
    if (cmd == L"--install" && argc == 2) {
        opensweep::log::init(true, false);
        const int code = opensweep::win::install_service();
        opensweep::log::shutdown();
        return code;
    }
    if (cmd == L"--uninstall" && argc == 2) {
        opensweep::log::init(true, false);
        const int code = opensweep::win::uninstall_service();
        opensweep::log::shutdown();
        return code;
    }
    print_usage();
    return 2;
}
