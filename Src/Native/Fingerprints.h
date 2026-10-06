#pragma once

#include "plugin.h"
#include "plugin_interface.h"

#include <cstdint>

namespace PersistentIdFixFingerprints
{
    struct ResolvedAddresses
    {
        uintptr_t getOrAddIDForHandle = 0;
        uintptr_t setIDHandlePair = 0;
        uintptr_t getSaveData = 0;
    };

    bool Resolve(
        IPluginSelf* self,
        IPluginHookScanner* scanner,
        ResolvedAddresses& addresses);
}
