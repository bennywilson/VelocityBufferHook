// Classic CreateRemoteThread + LoadLibraryW injection.
//
// Important: Does not attempt to thwart anti-cheat.  Recommend use is on
// shipping builds of single player Unreal Demos
//
// Usage:
//   mv_injector.exe <process_name.exe> <path_to_hook_dll>

#include <windows.h>
#include <tlhelp32.h>

#include <iostream>
#include <string>

namespace {

DWORD FindProcessId(const std::wstring& processName) {
    // Take a snapshot of every running process on the system.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    // Walk the snapshot looking for our process
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

// Returns the module name from a DLL path. The module name is what
// TH32CS_SNAPMODULE reports (szModule, name w/o path).
//
// Note: This function doesn't distinguish between two same-named modules loaded
// from different paths.
std::wstring ModuleNameFromPath(const std::wstring& dllPath) {
    const size_t lastSlash = dllPath.find_last_of(L"/\\");
    return (lastSlash == std::wstring::npos) ? dllPath : dllPath.substr(lastSlash + 1);
}

// Finds a module's base address inside a target process by name, via TH32CS_SNAPMODULE
HMODULE FindRemoteModuleBase(DWORD pid, const std::wstring& moduleName, std::wstring* const outFullPath) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    HMODULE base = nullptr;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (moduleName == entry.szModule) {
                base = entry.hModule;
                if (outFullPath != nullptr) {
                    *outFullPath = entry.szExePath;
                }
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return base;
}

std::wstring AbsolutePath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    return (length > 0 && length < MAX_PATH) ? std::wstring(buffer, length) : path;
}

bool PathsEqual(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

// Runs LoadLibraryW export as a remote thread on the target process and waits.
//
// Note: we're calling a void(void) export through a start routine typed
// DWORD WINAPI(LPVOID). That's fine as the export never reads the unused
// argument register.
bool RunRemoteThread(HANDLE process, LPVOID arg, DWORD& exitCode) {
    // kernel32.dll loads at the same base address in every process within
    // a boot session. So we can resolve LoadLibraryW's address in *this* process and
    // using it as the remote thread's start routine in the target process.
    const auto loadLibraryAddr =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibraryAddr, arg, 0, nullptr);
    if (thread == nullptr) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    const BOOL ok = GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    return ok != FALSE;
}

int DoInject(const std::wstring& processName, const std::wstring& dllPath) {
    const DWORD pid = FindProcessId(processName);
    if (pid == 0) {
        std::wcerr << L"process not found: " << processName << L"\n";
        return 1;
    }
    std::wcout << L"found " << processName << L", pid=" << pid << L"\n";


    // Check if the dll was already injected.
    {
        const std::wstring moduleName = ModuleNameFromPath(dllPath);
        std::wstring existingPath;
        const HMODULE existing = FindRemoteModuleBase(pid, moduleName, &existingPath);
        if (existing != nullptr) {
            std::wcerr << L"App already injected. Close the app and restart before injecting again.\n";
            return 1;
        }
    }

    const HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ,
        FALSE,
        pid);
    if (process == nullptr) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 1;
    }

    // Allocate memory inside the target process to store dll path
    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    const LPVOID remoteMem = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMem == nullptr) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        CloseHandle(process);
        return 1;
    }

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), pathBytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    // Perform the injection
    DWORD exitCodeLow = 0;
    if (!RunRemoteThread(process, remoteMem, exitCodeLow)) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return 1;
    }

    // LoadLibraryW returns an HMODULE (64 bits on x64), but the remote thread's
    // exit code is only a DWORD, so we only get the low 32 bits. We treat an
    // all-zero result as failure.
    //
    // Note: Windows pages are 4KB-aligned, so the low 12 bits of a real module
    // base are always 0; ASLR randomizes bits 12-31. If those also land all
    // zero (roughly one-in-a-million), a successful load would misreport as
    // failure here.
    if (exitCodeLow == 0) {
        std::wcerr << L"injection likely failed (LoadLibraryW returned NULL in the target process)\n";
    } else {
        std::wcout << L"injection succeeded (low 32 bits of the returned HMODULE = 0x" << std::hex << exitCodeLow
                   << L"; the full 64-bit handle cannot be recovered from a thread exit code)\n";
    }

    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    CloseHandle(process);
    return exitCodeLow == 0 ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3) {
        return DoInject(argv[1], argv[2]);
    }

    std::wcerr << L"usage:\n"
               << L"  mv_injector.exe <process_name.exe> <path_to_hook_dll>\n";
    return 1;
}
