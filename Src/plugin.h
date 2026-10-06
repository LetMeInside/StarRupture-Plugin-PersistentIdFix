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

// Native UCrSaveSubsystem::GetSaveData ABI.
//
// The by-value FString parameter is passed indirectly in RDX on Windows x64.
// Step 2 treats the FString and UStruct arguments as opaque pointers. Later
// collector work will copy/interpret them before/after the native call as
// required by the audited serialization contract.
using GetSaveDataFn =
bool (*)(
    void* saveSubsystem,
    void* sectionName,
    const void* structType,
    void* destination);

// Native UCrSaveSubsystem::OnPreLoadMap ABI.
//
// The audited Hotfix 0.3.5 body receives the save subsystem in RCX.
// RDX is forwarded opaquely so the detour does not depend on the
// concrete map-name parameter representation.
using OnPreLoadMapFn =
void (*)(
    void* saveSubsystem,
    const void* mapName);

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();

    __declspec(dllexport) void OnPluginLoadHooks(
        IPluginSelf* self,
        IPluginHookScanner* scanner);
}
