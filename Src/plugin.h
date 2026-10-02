#pragma once

#include <windows.h>
#include "plugin_interface.h"
#include "ChimeraMassCommon_classes.hpp"

extern IPluginSelf* g_self;

using GetOrAddIDForHandleFn =
SDK::FCrMassPersistentEntityID* (*)(
    void* subsystem,
    SDK::FCrMassPersistentEntityID* returnValue,
    SDK::FMassEntityHandle handle);

using SetIDHandlePairFn =
bool (*)(
    void* subsystem,
    SDK::FCrMassPersistentEntityID* persistentId,
    SDK::FMassEntityHandle handle);

bool IsGameSessionActive();

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();

    __declspec(dllexport) void OnPluginLoadHooks(
        IPluginSelf* self,
        IPluginHookScanner* scanner);
}
