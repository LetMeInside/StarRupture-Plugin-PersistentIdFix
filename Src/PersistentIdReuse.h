#pragma once

#include <cstdint>
#include <vector>

#include "ChimeraMassCommon_classes.hpp"
#include "plugin.h"

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

    PersistentIdAllocationResult TryAllocate(
        SDK::FMassEntityHandle handle,
        SDK::FCrMassPersistentEntityID& outId);

    bool IsInitializedFor(
        SDK::UCrMassPersistentIDSubsystem* subsystem) const;

    uint32_t GetSessionMaxID() const;
    uint64_t GetReusableIDCount() const;
    size_t GetRangeCount() const;

    bool GetFirstRange(
        uint32_t& first,
        uint32_t& last) const;

    bool GetLastRange(
        uint32_t& first,
        uint32_t& last) const;

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
    uint64_t reusableIDCount_ = 0;
};