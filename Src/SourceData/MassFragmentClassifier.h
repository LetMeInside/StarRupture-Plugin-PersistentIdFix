#pragma once

#include <cstdint>
#include <vector>

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

        std::uint64_t tagClassified = 0;
        std::uint64_t tagEmpty = 0;
        std::uint64_t tagMalformed = 0;
        std::uint64_t tagUnsupported = 0;
        std::uint64_t tagSchemaMismatches = 0;
        std::uint64_t tagContainerFailures = 0;

        std::uint64_t identityAttempts = 0;
        std::uint64_t identitySuccesses = 0;
        std::uint64_t identityFailures = 0;
        std::uint64_t identityValues = 0;
        std::uint64_t identityZeroValues = 0;
        std::uint64_t identityInvalidSentinels = 0;
        std::uint64_t identityContainerFailures = 0;

        std::uint64_t massRemainderAttempts = 0;
        std::uint64_t massRemainderSuccesses = 0;
        std::uint64_t massRemainderFailures = 0;
        std::uint64_t massRemainderValues = 0;
        std::uint64_t massRemainderZeroValues = 0;
        std::uint64_t massRemainderInvalidSentinels = 0;
        std::uint64_t massRemainderContainerFailures = 0;

        std::uint64_t stabilityValues = 0;
        std::uint64_t electricityValues = 0;
        std::uint64_t logisticsValues = 0;
        std::uint64_t waveValues = 0;
        std::uint64_t spawnOwnershipValues = 0;
        std::uint64_t foundableValues = 0;
    };

    // Step 3C/3D fragment classification and semantic PID collection plus
    // Step 3E-A read-only Mass identity/remainder collection and tag gating.
    // Collected values remain diagnostic/source data only and do not affect
    // allocator, reuse, activation, or persistent-ID assignment.
    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents);
    void ResetDescriptorRegistry();
    bool IsDescriptorRegistryReady();
    // Validates the top-level Mass destination before any field is read.
    // Side-effect free so the source observer can gate high-water capture on
    // the same descriptor/storage contract used by Mass classification.
    bool ValidateMassSaveDataEnvelope(
        const SDK::UScriptStruct* structType,
        const SDK::FCrMassSaveData* massSaveData);

    void ObserveMassSaveData(
        const SDK::UScriptStruct* structType,
        const SDK::FCrMassSaveData* massSaveData);

    ClassificationSnapshot GetSnapshot();

    // Appends plugin-owned PID copies collected from the Mass source.
    // No native save pointers are retained or revisited.
    bool AppendCollectedValues(std::vector<std::uint32_t>& destination);

    void LogSnapshot(const char* phase);
    void Reset();
}