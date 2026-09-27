#pragma once

#include <windows.h>
#include "plugin_interface.h"
#include "ChimeraMassCommon_classes.hpp"

extern IPluginSelf* g_self;

#define LOG_INFO(format, ...) \
    if (g_self != nullptr && g_self->logger != nullptr) \
        g_self->logger->Info(g_self, format, ##__VA_ARGS__)

#define LOG_WARN(format, ...) \
    if (g_self != nullptr && g_self->logger != nullptr) \
        g_self->logger->Warn(g_self, format, ##__VA_ARGS__)

#define LOG_ERROR(format, ...) \
    if (g_self != nullptr && g_self->logger != nullptr) \
        g_self->logger->Error(g_self, format, ##__VA_ARGS__)

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

extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginSelf* self);
    __declspec(dllexport) void PluginShutdown();

    __declspec(dllexport) void OnPluginLoadHooks(
        IPluginSelf* self,
        IPluginHookScanner* scanner);
}
