#pragma once

#include "plugin.h"

#include <vector>

namespace PersistentIdFixSystem
{
    struct AssignmentLedgerSnapshot
    {
        std::uint64_t loadGeneration = 0;
        bool healthy = true;
        std::uint64_t assignedEntries = 0;
        std::uint64_t burnedEntries = 0;
        std::uint64_t currentSubsystemAssigned = 0;
        std::uint64_t currentSubsystemBurned = 0;
        std::uint64_t foreignSubsystemAssigned = 0;
        std::uint64_t foreignSubsystemBurned = 0;
    };

    struct CandidatePoolDiagnostic
    {
        bool valid = false;
        std::uint64_t loadGeneration = 0;
        std::uint32_t loadedHighWater = 0;
        std::uint64_t loadedHandleIds = 0;
        std::uint64_t sourceProtectedIds = 0;
        std::uint64_t ledgerProtectedIds = 0;
        std::uint64_t blockedUniqueIds = 0;
        std::uint64_t sourceOnlyBlockedIds = 0;
        std::uint64_t candidateReusableIds = 0;
        std::uint64_t candidateRanges = 0;
        std::uint64_t legacyReusableIds = 0;
        std::uint64_t legacyRanges = 0;
        std::uint64_t stagedReusableIds = 0;
        std::uint64_t stagedRanges = 0;
        bool stagedHasFirstRange = false;
        std::uint32_t stagedFirst = 0;
        std::uint32_t stagedFirstLast = 0;
        bool stagedHasLastRange = false;
        std::uint32_t stagedLastFirst = 0;
        std::uint32_t stagedLast = 0;
    };

    void Configure(
        IPluginSelf* self,
        SetIDHandlePairFn setIDHandlePair,
        GetOrAddIDForHandleFn originalGetOrAddIDForHandle);

    void Reset();

    // Starts a fresh assignment/burn ledger for the incoming source
    // generation before native OnPreLoadMap begins loading that world.
    void BeginLoadGeneration(std::uint64_t loadGeneration);

    AssignmentLedgerSnapshot GetAssignmentLedgerSnapshot();
    void LogAssignmentLedger(const char* phase);

    CandidatePoolDiagnostic BuildCandidatePoolDiagnostic(
        std::uint64_t loadGeneration,
        const std::vector<std::uint32_t>& sourceProtectedIds);

    // Promotes only an already-staged certified pool for the exact
    // current load generation. Failure leaves reuse fail-closed.
    bool ActivateCertifiedPool(std::uint64_t loadGeneration);

    void SetSessionNetMode(EPluginNetMode netMode);

    // Returns the concrete current game-world role when available.
    // Unknown remains fail-closed; this accessor does not manufacture
    // authority from process type or startup heuristics.
    EPluginNetMode GetSessionNetMode();

    void OnSaveLoaded();

    void Tick();

    void EndSession();

    bool IsSessionActive();

    SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* returnValue,
        SDK::FMassEntityHandle handle);
}
