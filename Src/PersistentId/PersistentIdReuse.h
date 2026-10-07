#pragma once

#include <cstdint>
#include <vector>

#include "ChimeraMassCommon_classes.hpp"
#include "plugin.h"
#include "plugin_helpers.h"

enum class PersistentIdAllocationResult
{
    NoReusableId,
    Allocated,
    Failed
};

class PersistentIdReuse
{
public:
    void Initialize(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        SetIDHandlePairFn setIDHandlePair);

    void Clear();

    void BuildPool();

    // G4 diagnostic staging only. This builds a second range vector
    // without replacing or mutating the active allocator pool.
    bool StagePoolFromBlockedIds(
        const std::vector<std::uint32_t>& blockedIds,
        std::uint32_t highWater);

    void ClearStagedPool();

    // Atomically replaces the active legacy ranges with the already
    // validated staged certified ranges. No allocation occurs here.
    bool PromoteStagedPool();

    PersistentIdAllocationResult TryAllocate(
        SDK::FMassEntityHandle handle,
        SDK::FCrMassPersistentEntityID& outId);

    bool IsInitializedFor(
        SDK::UCrMassPersistentIDSubsystem* subsystem) const;

    uint32_t GetSessionMaxID() const;
    uint64_t GetReusableIDCount() const;
    size_t GetRangeCount() const;

    uint64_t GetStagedReusableIDCount() const;
    size_t GetStagedRangeCount() const;

    bool GetStagedFirstRange(
        uint32_t& first,
        uint32_t& last) const;

    bool GetStagedLastRange(
        uint32_t& first,
        uint32_t& last) const;

    bool GetFirstRange(
        uint32_t& first,
        uint32_t& last) const;

    bool GetLastRange(
        uint32_t& first,
        uint32_t& last) const;

    SDK::UCrMassPersistentIDSubsystem* GetSubsystem() const;

private:
    struct IdRange
    {
        uint32_t first;
        uint32_t last;
    };

    SDK::UCrMassPersistentIDSubsystem* subsystem_ = nullptr;
    SetIDHandlePairFn setIDHandlePair_ = nullptr;

    uint32_t sessionMaxID_ = 0;
    std::vector<IdRange> ranges_;
    size_t rangeIndex_ = 0;
    uint64_t reusableIDCount_ = 0;

    std::vector<IdRange> stagedRanges_;
    uint64_t stagedReusableIDCount_ = 0;
    bool stagedPoolReady_ = false;
};