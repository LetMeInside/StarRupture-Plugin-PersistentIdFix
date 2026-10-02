#pragma once

#include "plugin.h"

#include <cstdint>

namespace PersistentIdFixConfig
{
    constexpr int DefaultLogIntervalMinutes = 10;

#ifdef MODLOADER_CLIENT_BUILD
    constexpr const char* DefaultUIToggleKey = "F3";
#endif

    bool Initialize(IPluginSelf* self);

    int GetLogIntervalMinutes();

#ifdef MODLOADER_CLIENT_BUILD
    const char* GetUIToggleKey();
#endif
}
