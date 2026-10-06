#pragma once

#include <cstdint>

struct IPluginEngineEvents;

namespace SDK
{
    struct FCrMassSaveData;
    class UScriptStruct;
}

namespace PersistentIdFixMassFragmentClassifier
{
    struct ClassificationSnapshot
    {
        std::uint64_t massPayloads = 0;
        std::uint64_t classifiedPayloads = 0;
        std::uint64_t pidBearingPayloads = 0;
        std::uint64_t pidFreePayloads = 0;
        std::uint64_t emptyPayloads = 0;
        std::uint64_t malformedPayloads = 0;
        std::uint64_t unsupportedPayloads = 0;
        std::uint64_t schemaMismatches = 0;
        std::uint64_t massDescriptorMismatches = 0;
        std::uint64_t entitySlotsVisited = 0;
        std::uint64_t entityCountMismatches = 0;
        std::uint64_t tagPayloads = 0;
    };

    // Diagnostic-only Step 3C classifier. It does not read semantic PID
    // fields and does not affect allocator/reuse state.
    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents);
    void ResetDescriptorRegistry();
    bool IsDescriptorRegistryReady();

    void ObserveMassSaveData(
        const SDK::UScriptStruct* structType,
        const SDK::FCrMassSaveData* massSaveData);

    ClassificationSnapshot GetSnapshot();
    void LogSnapshot(const char* phase);
    void Reset();
}