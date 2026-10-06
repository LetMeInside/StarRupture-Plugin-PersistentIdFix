#pragma once

#include "plugin.h"

#include <cstdint>

namespace PersistentIdFixSourceReadObserver
{
    enum class SourceSection : std::uint8_t
    {
        Unknown = 0,
        Mass,
        BuildingCustomNames,
        GameStateData,
        AntennasData,
        ZiplineReplicator,
        ZiplineSubsystem,
        BaseCoreReplicationHelper
    };

    enum class CertificationState : std::uint8_t
    {
        InventoryUnavailable = 0,
        AbsentUnsupported,
        Pending,
        Certified,
        Failed
    };

    struct CoverageSnapshot
    {
        std::uint64_t loadGeneration = 0;

        bool inventoryCaptured = false;
        bool inventoryValid = false;
        std::uint64_t inventoryEntries = 0;
        std::uint64_t inventoryRecognized = 0;
        bool inventoryMass = false;
        bool inventoryBuildingCustomNames = false;
        bool inventoryGameStateData = false;
        bool inventoryAntennasData = false;
        bool inventoryZiplineReplicator = false;
        bool inventoryZiplineSubsystem = false;
        bool inventoryBaseCoreReplicationHelper = false;

        CertificationState massCertification =
            CertificationState::InventoryUnavailable;
        CertificationState buildingCustomNamesCertification =
            CertificationState::InventoryUnavailable;
        CertificationState gameStateDataCertification =
            CertificationState::InventoryUnavailable;
        CertificationState antennasDataCertification =
            CertificationState::InventoryUnavailable;
        CertificationState ziplineReplicatorCertification =
            CertificationState::InventoryUnavailable;
        CertificationState ziplineSubsystemCertification =
            CertificationState::InventoryUnavailable;
        CertificationState baseCoreReplicationCertification =
            CertificationState::InventoryUnavailable;

        std::uint64_t successfulReads = 0;
        std::uint64_t recognizedSuccessfulReads = 0;
        std::uint64_t failedReads = 0;

        std::uint64_t massReads = 0;
        std::uint64_t buildingCustomNameReads = 0;
        std::uint64_t gameStateDataReads = 0;
        std::uint64_t antennasDataReads = 0;
        std::uint64_t ziplineReplicatorReads = 0;
        std::uint64_t ziplineSubsystemReads = 0;
        std::uint64_t baseCoreReplicationHelperReads = 0;
    };

    // Observe one native GetSaveData call.
    //
    // The section-name identity is captured before invoking the native
    // function because its by-value FString argument is destroyed by the
    // callee. Successful certified sections are inspected synchronously after
    // native conversion and before control returns to the native consumer.
    bool ObserveGetSaveData(
        GetSaveDataFn original,
        void* saveSubsystem,
        void* sectionName,
        const void* structType,
        void* destination);

    // Starts a fresh passive observation generation immediately before
    // UCrSaveSubsystem broadcasts OnPreSaveLoaded for an incoming map.
    // This prevents observations from a previous world/load from being
    // reused as coverage for the new generation.
    void BeginLoadGeneration(
        void* saveSubsystem);

    CoverageSnapshot GetCoverageSnapshot();

    // Writes a compact observational snapshot. The phase label identifies
    // whether the snapshot was taken at Mass OnSaveLoaded or at world end.
    void LogCoverage(const char* phase);

    void Reset();
}
