#pragma once

#include "plugin.h"

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
