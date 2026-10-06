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

        std::uint64_t semanticAttempts = 0;
        std::uint64_t semanticSuccesses = 0;
        std::uint64_t semanticFailures = 0;
        std::uint64_t semanticNumericValues = 0;
        std::uint64_t semanticZeroValues = 0;
        std::uint64_t semanticInvalidSentinels = 0;
        std::uint64_t semanticContainerFailures = 0;
    };

    // Step 3C classification plus Step 3D read-only semantic PID collection.
    // Collected values remain diagnostic/source data only and do not affect
    // allocator, reuse, activation, or persistent-ID assignment.
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