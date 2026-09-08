// Launcher.cpp - Cultures 1 GameExtend DLL Injector
//
// Creates Cultures.exe in suspended state, injects CulturesGameExtend.asi
// via CreateRemoteThread + LoadLibraryA, then resumes the process.
//
// The .asi is a standard DLL renamed; LoadLibraryA works on it regardless
// of extension as long as we pass the full path.
//
// Build: Win32 Release EXE, links kernel32+user32.

#include <windows.h>
#include <string>
#include <cstdio>

static void MsgBox(const char* msg) {
    MessageBoxA(nullptr, msg, "Cultures Launcher", MB_ICONERROR | MB_OK);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // ---- Resolve paths relative to launcher exe directory ----
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';
    else { dir[0] = '.'; dir[1] = '\0'; }

    std::string exePath  = std::string(dir) + "\\Cultures.exe";
    std::string dllPath  = std::string(dir) + "\\CulturesGameExtend.asi";
    std::string workDir  = std::string(dir);

    // ---- Verify files exist ----
    DWORD attr = GetFileAttributesA(exePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        MsgBox("Cultures.exe not found in launcher directory.");
        return 1;
    }
    attr = GetFileAttributesA(dllPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        MsgBox("CulturesGameExtend.asi not found in launcher directory.");
        return 1;
    }

    // ---- CreateProcess in suspended state ----
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = "\"Cultures.exe\"";

    BOOL ok = CreateProcessA(
        exePath.c_str(),
        const_cast<LPSTR>(cmdLine.c_str()),
        nullptr, nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workDir.c_str(),
        &si, &pi
    );
    if (!ok) {
        char buf[256];
        sprintf_s(buf, "CreateProcess failed (error=%lu).", GetLastError());
        MsgBox(buf);
        return 1;
    }

    // ---- Inject DLL via CreateRemoteThread + LoadLibraryA ----
    size_t dllPathLen = dllPath.size() + 1;
    LPVOID remoteMem = VirtualAllocEx(
        pi.hProcess, nullptr, dllPathLen,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
    );
    if (!remoteMem) {
        char buf[256];
        sprintf_s(buf, "VirtualAllocEx failed (error=%lu).", GetLastError());
        MsgBox(buf);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPath.c_str(), dllPathLen, &written)) {
        char buf[256];
        sprintf_s(buf, "WriteProcessMemory failed (error=%lu).", GetLastError());
        MsgBox(buf);
        VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLibAddr = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!loadLibAddr) {
        MsgBox("Cannot find LoadLibraryA.");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    HANDLE hRemoteThread = CreateRemoteThread(
        pi.hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibAddr),
        remoteMem, 0, nullptr
    );
    if (!hRemoteThread) {
        char buf[256];
        sprintf_s(buf, "CreateRemoteThread failed (error=%lu).", GetLastError());
        MsgBox(buf);
        VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    // Wait for LoadLibraryA to complete (DllMain called)
    WaitForSingleObject(hRemoteThread, 5000);
    VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hRemoteThread);

    // ---- Resume the main thread ----
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        char buf[256];
        sprintf_s(buf, "ResumeThread failed (error=%lu).", GetLastError());
        MsgBox(buf);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
