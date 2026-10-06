#pragma once

#include <cstdint>

struct IPluginEngineEvents;

namespace SDK
{
    class UScriptStruct;
    struct FBuildingCustomNameSaveData;
    struct FCrAntennaSaveData;
    struct FCrZiplineReplicatorSaveData;
    struct FCrZiplineSaveData;
    struct FBaseCoreReplicationSaveData;
}

namespace PersistentIdFixIndependentSourceCollector
{
    enum class Section : std::uint8_t
    {
        BuildingCustomNames = 0,
        AntennasData,
        ZiplineReplicator,
        ZiplineSubsystem,
        BaseCoreReplication
    };

    struct SectionSnapshot
    {
        std::uint64_t readFailures = 0;
        std::uint64_t attempts = 0;
        std::uint64_t successes = 0;
        std::uint64_t failures = 0;
        std::uint64_t schemaMismatches = 0;
        std::uint64_t values = 0;
        std::uint64_t zeroValues = 0;
        std::uint64_t invalidSentinels = 0;
        std::uint64_t containerFailures = 0;
    };

    struct Snapshot
    {
        SectionSnapshot buildingCustomNames;
        SectionSnapshot antennasData;
        SectionSnapshot ziplineReplicator;
        SectionSnapshot ziplineSubsystem;
        SectionSnapshot baseCoreReplication;
    };

    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents);
    void ResetDescriptorRegistry();
    bool IsDescriptorRegistryReady();

    void RecordReadFailure(Section section);

    bool ObserveBuildingCustomNames(
        const SDK::UScriptStruct* structType,
        const SDK::FBuildingCustomNameSaveData* data);

    bool ObserveAntennas(
        const SDK::UScriptStruct* structType,
        const SDK::FCrAntennaSaveData* data);

    bool ObserveZiplineReplicator(
        const SDK::UScriptStruct* structType,
        const SDK::FCrZiplineReplicatorSaveData* data);

    bool ObserveZiplineSubsystem(
        const SDK::UScriptStruct* structType,
        const SDK::FCrZiplineSaveData* data);

    bool ObserveBaseCoreReplication(
        const SDK::UScriptStruct* structType,
        const SDK::FBaseCoreReplicationSaveData* data);

    Snapshot GetSnapshot();
    void LogSnapshot(const char* phase);
    void Reset();
}
