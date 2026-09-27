#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
    }

    return TRUE;
}
