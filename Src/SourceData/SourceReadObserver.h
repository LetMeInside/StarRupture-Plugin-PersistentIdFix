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

    struct ObserverActivitySnapshot
    {
        std::uint64_t activeFrames = 0;
        std::uint64_t frameTransitions = 0;
    };

    struct CoverageSnapshot
    {
        std::uint64_t loadGeneration = 0;

        // Original serialized FCrMassSaveData::MaxPersistentID
        // captured synchronously from the successful Mass
        // GetSaveData destination before native PostInitialize
        // resumes and applies it to the live PID subsystem.
        bool savedHighWaterCaptured = false;
        bool savedHighWaterConflict = false;
        std::uint64_t savedHighWaterGeneration = 0;
        std::uint64_t savedHighWaterCaptures = 0;
        std::uint32_t savedHighWater = 0;

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

        // Aggregate coverage applies only to the retained load generation.
        // It does not prove that this generation belongs to the currently
        // active authoritative ChimeraMain world and must not be used as
        // an allocator/reuse activation decision by itself.
        bool sourceCoverageComplete = false;

        // F2 attaches a load generation to the currently active
        // ChimeraMain world. This is still diagnostic state only:
        // authority/NetMode and allocator gates remain separate.
        bool gameWorldAttached = false;
        std::uint64_t attachedLoadGeneration = 0;
        bool coverageAttachedToCurrentGeneration = false;

        std::uint64_t certifiedSections = 0;
        std::uint64_t pendingSections = 0;
        std::uint64_t failedSections = 0;
        std::uint64_t absentUnsupportedSections = 0;
        std::uint64_t inventoryUnavailableSections = 0;

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

    // Called only from the plugin's actual ChimeraMain lifecycle.
    // The begin operation snapshots the current load-generation number;
    // the end operation invalidates only that world attachment.
    void AttachCurrentGenerationToGameWorld();
    void DetachGameWorld();

    CoverageSnapshot GetCoverageSnapshot();
    ObserverActivitySnapshot GetObserverActivitySnapshot();

    // Publication freeze.
    //
    // BeginPublicationFreeze() acquires the observer's exclusive
    // source-data gate. It waits for every in-flight GetSaveData
    // observer frame to finish and prevents any new observer frame or
    // generation reset from beginning until EndPublicationFreeze().
    // The caller must pair a successful begin with exactly one end on
    // the same thread.
    bool BeginPublicationFreeze();
    void EndPublicationFreeze();

    // Writes a compact observational snapshot. The phase label identifies
    // whether the snapshot was taken at Mass OnSaveLoaded or at world end.
    void LogCoverage(const char* phase);

    void Reset();
}
