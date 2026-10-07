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

    struct SetPairObserverSnapshot
    {
        std::uint64_t loadGeneration = 0;
        std::uint64_t totalCalls = 0;
        std::uint64_t trueReturns = 0;
        std::uint64_t falseReturns = 0;
        std::uint64_t exactPairBefore = 0;
        std::uint64_t exactPairAfter = 0;
        std::uint64_t newInsertions = 0;
        std::uint64_t consistentDuplicates = 0;
        std::uint64_t conflictingAttempts = 0;
        std::uint64_t maxAdvances = 0;
        std::uint64_t maxAdvanceWithoutExactPair = 0;
        std::uint64_t postStateMismatches = 0;
        std::uint64_t nullArgumentCalls = 0;
    };


    struct HighWaterCrossCheckSnapshot
    {
        bool available = false;
        bool restoredAtLeastSaved = false;
        bool exactAccountingRequested = false;
        bool exactAccountingValid = false;
        bool exactAccountingMatch = false;
        bool expectedOverflow = false;
        std::uint64_t sourceGeneration = 0;
        std::uint64_t ledgerGeneration = 0;
        std::uint32_t savedHighWater = 0;
        std::uint32_t liveHighWater = 0;
        std::uint64_t currentAssigned = 0;
        std::uint64_t currentBurned = 0;
        std::uint64_t foreignAssigned = 0;
        std::uint64_t foreignBurned = 0;
        std::uint64_t expectedLiveHighWater = 0;
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

    SetPairObserverSnapshot GetSetPairObserverSnapshot();
    void LogSetPairObserver(const char* phase);

    HighWaterCrossCheckSnapshot LogHighWaterCrossCheck(
        const char* phase,
        std::uint64_t sourceGeneration,
        bool savedHighWaterCaptured,
        bool savedHighWaterConflict,
        std::uint32_t savedHighWater,
        bool requireExactAccounting);

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

    void BeginGameWorld(SDK::UWorld* world);
    void EndGameWorld(SDK::UWorld* world);

    void OnSaveLoaded();

    void Tick();

    void EndSession();

    bool IsSessionActive();

    bool SetIDHandlePairDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* persistentId,
        SDK::FMassEntityHandle handle);

    SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* returnValue,
        SDK::FMassEntityHandle handle);
}
