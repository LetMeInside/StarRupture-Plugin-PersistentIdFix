#pragma once

#include "plugin.h"

namespace PersistentIdFixSystem
{
    void Configure(
        IPluginSelf* self,
        SetIDHandlePairFn setIDHandlePair,
        GetOrAddIDForHandleFn originalGetOrAddIDForHandle);

    void Reset();

    void SetSessionNetMode(EPluginNetMode netMode);

    void OnSaveLoaded();

    void Tick();

    void EndSession();

    bool IsSessionActive();

    SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* returnValue,
        SDK::FMassEntityHandle handle);
}
