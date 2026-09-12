#include "version_proxy.h"

#include <windows.h>

#include <string>

namespace
{
HMODULE g_realVersion = nullptr;

FARPROC GetRealVersionProc(const char* name)
{
    if (!g_realVersion)
    {
        wchar_t sysDir[MAX_PATH]{};
        GetSystemDirectoryW(sysDir, MAX_PATH);

        std::wstring path = sysDir;
        path += L"\\version.dll";

        g_realVersion = LoadLibraryW(path.c_str());
        if (!g_realVersion)
        {
            MessageBoxW(nullptr, L"Failed to load real system version.dll", L"Moca Love Relive Motion Sync", MB_ICONERROR);
            return nullptr;
        }
    }

    return GetProcAddress(g_realVersion, name);
}
}

#define RESOLVE_VERSION_API(name) reinterpret_cast<decltype(&Proxy_##name)>(GetRealVersionProc(#name))

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoA);
    return fn ? fn(a, b, c, d) : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoW);
    return fn ? fn(a, b, c, d) : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExA(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoExA);
    return fn ? fn(a, b, c, d, e) : FALSE;
}

extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExW(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoExW);
    return fn ? fn(a, b, c, d, e) : FALSE;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoSizeA);
    return fn ? fn(a, b) : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoSizeW);
    return fn ? fn(a, b) : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(DWORD a, LPCSTR b, LPDWORD c)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoSizeExA);
    return fn ? fn(a, b, c) : 0;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(DWORD a, LPCWSTR b, LPDWORD c)
{
    auto fn = RESOLVE_VERSION_API(GetFileVersionInfoSizeExW);
    return fn ? fn(a, b, c) : 0;
}

extern "C" DWORD WINAPI Proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h)
{
    auto fn = RESOLVE_VERSION_API(VerFindFileA);
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

extern "C" DWORD WINAPI Proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h)
{
    auto fn = RESOLVE_VERSION_API(VerFindFileW);
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

extern "C" DWORD WINAPI Proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h)
{
    auto fn = RESOLVE_VERSION_API(VerInstallFileA);
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

extern "C" DWORD WINAPI Proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h)
{
    auto fn = RESOLVE_VERSION_API(VerInstallFileW);
    return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

extern "C" DWORD WINAPI Proxy_VerLanguageNameA(DWORD a, LPSTR b, DWORD c)
{
    auto fn = RESOLVE_VERSION_API(VerLanguageNameA);
    return fn ? fn(a, b, c) : 0;
}

extern "C" DWORD WINAPI Proxy_VerLanguageNameW(DWORD a, LPWSTR b, DWORD c)
{
    auto fn = RESOLVE_VERSION_API(VerLanguageNameW);
    return fn ? fn(a, b, c) : 0;
}

extern "C" BOOL WINAPI Proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d)
{
    auto fn = RESOLVE_VERSION_API(VerQueryValueA);
    return fn ? fn(a, b, c, d) : FALSE;
}

extern "C" BOOL WINAPI Proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d)
{
    auto fn = RESOLVE_VERSION_API(VerQueryValueW);
    return fn ? fn(a, b, c, d) : FALSE;
}

void ShutdownVersionProxy()
{
    if (g_realVersion)
    {
        FreeLibrary(g_realVersion);
        g_realVersion = nullptr;
    }
}

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=Proxy_GetFileVersionInfoA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=Proxy_GetFileVersionInfoW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExA=Proxy_GetFileVersionInfoExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoExW=Proxy_GetFileVersionInfoExW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=Proxy_GetFileVersionInfoSizeA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=Proxy_GetFileVersionInfoSizeW")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExA=Proxy_GetFileVersionInfoSizeExA")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeExW=Proxy_GetFileVersionInfoSizeExW")
#pragma comment(linker, "/EXPORT:VerFindFileA=Proxy_VerFindFileA")
#pragma comment(linker, "/EXPORT:VerFindFileW=Proxy_VerFindFileW")
#pragma comment(linker, "/EXPORT:VerInstallFileA=Proxy_VerInstallFileA")
#pragma comment(linker, "/EXPORT:VerInstallFileW=Proxy_VerInstallFileW")
#pragma comment(linker, "/EXPORT:VerLanguageNameA=Proxy_VerLanguageNameA")
#pragma comment(linker, "/EXPORT:VerLanguageNameW=Proxy_VerLanguageNameW")
#pragma comment(linker, "/EXPORT:VerQueryValueA=Proxy_VerQueryValueA")
#pragma comment(linker, "/EXPORT:VerQueryValueW=Proxy_VerQueryValueW")
