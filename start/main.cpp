// AgentUnderFire.exe in the release's top folder: the one file a player starts. It runs the game from the Game Files
// folder beside it (the three executables, their code, settings, saves, textures and logs) and exits; the game finds
// the disc image in the ROM folder beside this one. No C runtime, so nothing has to sit beside it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" void* memset(void* d, int c, size_t n) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(d);
    while (n--) *p++ = static_cast<unsigned char>(c);
    return d;
}
extern "C" void* memcpy(void* d, const void* s, size_t n) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(d);
    const volatile unsigned char* q = static_cast<const volatile unsigned char*>(s);
    while (n--) *p++ = *q++;
    return d;
}
extern "C" int _fltused = 0;

namespace {

constexpr int kMax = 1024;

int Length(const wchar_t* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

bool Append(wchar_t* d, int& at, const wchar_t* s) {
    for (; *s; s++) {
        if (at >= kMax - 1) return false;
        d[at++] = *s;
    }
    d[at] = 0;
    return true;
}

// the command line past this program's own name, passed on to the game
const wchar_t* Arguments() {
    const wchar_t* c = GetCommandLineW();
    if (*c == L'"') {
        c++;
        while (*c && *c != L'"') c++;
        if (*c) c++;
    } else {
        while (*c && *c != L' ' && *c != L'\t') c++;
    }
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}

}  // namespace

extern "C" void Start() {
    static wchar_t dir[kMax], exe[kMax], cmd[kMax * 2];
    DWORD n = GetModuleFileNameW(nullptr, dir, kMax);
    if (!n || n >= kMax) ExitProcess(1);
    int at = int(n);
    while (at > 0 && dir[at - 1] != L'\\' && dir[at - 1] != L'/') at--;
    dir[at] = 0;
    bool ok = Append(dir, at, L"Game Files");
    int e = 0;
    ok = ok && Append(exe, e, dir) && Append(exe, e, L"\\AgentUnderFire.exe");
    // "<exe>" <arguments>
    int c = 0;
    cmd[c++] = L'"';
    for (int i = 0; exe[i]; i++) cmd[c++] = exe[i];
    cmd[c++] = L'"';
    const wchar_t* args = Arguments();
    if (*args && c + 1 + Length(args) < kMax * 2 - 1) {
        cmd[c++] = L' ';
        for (; *args; args++) cmd[c++] = *args;
    }
    cmd[c] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!ok || !CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
        MessageBoxW(nullptr,
                    L"The Game Files folder beside AgentUnderFire.exe is missing or incomplete.\n\n"
                    L"Extract the whole release again, keeping its folders, then start AgentUnderFire.exe.",
                    L"Agent Under Fire", MB_ICONERROR);
        ExitProcess(1);
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}
