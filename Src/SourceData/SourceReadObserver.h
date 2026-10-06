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

    struct CoverageSnapshot
    {
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

    CoverageSnapshot GetCoverageSnapshot();

    // Writes a compact observational snapshot. The phase label identifies
    // whether the snapshot was taken at Mass OnSaveLoaded or at world end.
    void LogCoverage(const char* phase);

    void Reset();
}
