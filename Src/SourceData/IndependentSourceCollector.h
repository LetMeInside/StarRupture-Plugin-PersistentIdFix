#pragma once

#include <cstdint>
#include <vector>

struct IPluginEngineEvents;

namespace SDK
{
    class UScriptStruct;
    struct FBuildingCustomNameSaveData;
    struct FCrAntennaSaveData;
    struct FCrZiplineReplicatorSaveData;
    struct FCrZiplineSaveData;
    struct FBaseCoreReplicationSaveData;
    struct FGameStateSaveData;
#if defined(MODLOADER_CLIENT_BUILD)
    struct FCrSenderConnectionsSaveData;
    struct FCrStorageShareableSubsystemSaveData;
    struct FBaseSiteAttackSubsystemSaveData;
#endif
}

namespace PersistentIdFixIndependentSourceCollector
{
    enum class Section : std::uint8_t
    {
        BuildingCustomNames = 0,
        AntennasData,
        ZiplineReplicator,
        ZiplineSubsystem,
        BaseCoreReplication,
        GameStateData,
        CrPackageTransportReplicator,
        StorageShareableSubsystemData,
        BaseSiteAttackData,
        Count
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
        std::uint64_t incompleteCoverage = 0;
    };

    struct Snapshot
    {
        SectionSnapshot buildingCustomNames;
        SectionSnapshot antennasData;
        SectionSnapshot ziplineReplicator;
        SectionSnapshot ziplineSubsystem;
        SectionSnapshot baseCoreReplication;
        SectionSnapshot gameStateData;
#if defined(MODLOADER_CLIENT_BUILD)
        SectionSnapshot crPackageTransportReplicator;
        SectionSnapshot storageShareableSubsystemData;
        SectionSnapshot baseSiteAttackData;
#endif

        std::uint64_t gameStatePlayers = 0;
        std::uint64_t gameStateFloorValues = 0;
        std::uint64_t gameStateAntennaFogValues = 0;
        std::uint64_t gameStateDevicePayloads = 0;
        std::uint64_t gameStateDeviceEmpty = 0;
        std::uint64_t gameStateDeviceMalformed = 0;
        std::uint64_t gameStateDeviceUnknownPresent = 0;
        std::uint64_t gameStateDeviceRecognizedPidFree = 0;
        std::uint64_t gameStateDeviceShadowSupported = 0;
        std::uint64_t gameStateDeviceShadowUnsupported = 0;
        std::uint64_t gameStateDeviceShadowProperties = 0;
        std::uint64_t gameStateDeviceShadowArrays = 0;
        std::uint64_t gameStateDeviceShadowStructs = 0;
        std::uint64_t gameStateDeviceShadowIntegralLeaves = 0;
        std::uint64_t gameStateDeviceShadowEnumLeaves = 0;
        std::uint64_t gameStateDeviceShadowMaxDepth = 0;
        std::uint64_t gameStateOpaqueStoreEntries = 0;
        std::uint64_t gameStateDiscoveredBuildingValues = 0;
    };

    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents);
    void ResetDescriptorRegistry();
    bool IsDescriptorRegistryReady();

    void RecordReadFailure(Section section);

    // Keeps a successful native read that contradicts inventory absence sticky-failed.
    void RecordConsistencyFailure(Section section);

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

    bool ObserveGameState(
        const SDK::UScriptStruct* structType,
        const SDK::FGameStateSaveData* data);

#if defined(MODLOADER_CLIENT_BUILD)
    // Collects sender and receiver persistent IDs from the validated transport map.
    bool ObserveCrPackageTransportReplicator(
        const SDK::UScriptStruct* structType,
        const SDK::FCrSenderConnectionsSaveData* data);

    // Collects each storage handle regardless of its unlock state.
    bool ObserveStorageShareableSubsystemData(
        const SDK::UScriptStruct* structType,
        const SDK::FCrStorageShareableSubsystemSaveData* data);

    // Collects the three persistent handles saved by the base attack subsystem.
    bool ObserveBaseSiteAttackData(
        const SDK::UScriptStruct* structType,
        const SDK::FBaseSiteAttackSubsystemSaveData* data);
#endif

    Snapshot GetSnapshot();

    // Appends plugin-owned PID copies collected from independent protected sections.
    bool AppendCollectedValues(std::vector<std::uint32_t>& destination);

    void LogSnapshot(const char* phase);
    void Reset();
}
