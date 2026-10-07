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
        uintptr_t onPreLoadMap = 0;

        // H2 early-attachment probe dependencies.
        // gEngineStorage is the address of native UEngine* GEngine storage.
        uintptr_t gEngineStorage = 0;
        uintptr_t getGameWorld = 0;
        uintptr_t hasBegunPlay = 0;
    };

    bool Resolve(
        IPluginSelf* self,
        IPluginHookScanner* scanner,
        ResolvedAddresses& addresses);
}
