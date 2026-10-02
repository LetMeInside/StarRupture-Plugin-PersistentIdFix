#pragma once

#include "plugin.h"

#include <cstdint>

namespace PersistentIdFixConfig
{
    constexpr int DefaultLogIntervalMinutes = 10;
    constexpr const char* DefaultUIToggleKey = "F3";

    bool Initialize(IPluginSelf* self);

    int GetLogIntervalMinutes();
    const char* GetUIToggleKey();
}