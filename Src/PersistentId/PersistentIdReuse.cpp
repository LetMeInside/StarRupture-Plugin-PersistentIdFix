#include "PersistentIdReuse.h"

#include <algorithm>
#include <limits>

void PersistentIdReuse::Initialize(
    SDK::UCrMassPersistentIDSubsystem* subsystem,
    SetIDHandlePairFn setIDHandlePair)
{
    Clear();

    subsystem_ = subsystem;
    setIDHandlePair_ = setIDHandlePair;
}

void PersistentIdReuse::Clear()
{
    subsystem_ = nullptr;
    setIDHandlePair_ = nullptr;

    sessionMaxID_ = 0;
    ranges_.clear();
    rangeIndex_ = 0;
    reusableIDCount_ = 0;

    stagedRanges_.clear();
    stagedReusableIDCount_ = 0;
    stagedPoolReady_ = false;
}

void PersistentIdReuse::BuildPool()
{
    ranges_.clear();
    rangeIndex_ = 0;
    reusableIDCount_ = 0;

    if (subsystem_ == nullptr)
        return;

    sessionMaxID_ = subsystem_->MaxID;

    if (sessionMaxID_ == 0)
        return;

    std::vector<uint32_t> existingIDs;
    existingIDs.reserve(
        static_cast<size_t>(subsystem_->IDHandleMap.Num()));

    for (const auto& pair : subsystem_->IDHandleMap)
    {
        const uint32_t id = pair.Key().ID;

        if (id >= 1 && id < sessionMaxID_)
            existingIDs.push_back(id);
    }

    std::sort(existingIDs.begin(), existingIDs.end());

    existingIDs.erase(
        std::unique(existingIDs.begin(), existingIDs.end()),
        existingIDs.end());

    uint32_t nextID = 1;

    for (const uint32_t existingID : existingIDs)
    {
        if (nextID < existingID)
        {
            const uint32_t first = nextID;
            const uint32_t last = existingID - 1;

            ranges_.push_back({ first, last });

            reusableIDCount_ +=
                static_cast<uint64_t>(last) -
                static_cast<uint64_t>(first) + 1;
        }

        nextID = existingID + 1;
    }

    if (nextID < sessionMaxID_)
    {
        const uint32_t first = nextID;
        const uint32_t last = sessionMaxID_ - 1;

        ranges_.push_back({ first, last });

        reusableIDCount_ +=
            static_cast<uint64_t>(last) -
            static_cast<uint64_t>(first) + 1;
    }
}

bool PersistentIdReuse::StagePoolFromBlockedIds(
    const std::vector<std::uint32_t>& blockedIds,
    std::uint32_t highWater)
{
    stagedRanges_.clear();
    stagedReusableIDCount_ = 0;
    stagedPoolReady_ = false;

    if (highWater == 0)
        return false;

    std::uint32_t previous = 0;

    for (const std::uint32_t id : blockedIds)
    {
        if (id == 0 || id >= highWater)
            return false;

        if (previous != 0 && id <= previous)
            return false;

        previous = id;
    }

    try
    {
        std::uint32_t nextID = 1;

        for (const std::uint32_t blockedID : blockedIds)
        {
            if (nextID < blockedID)
            {
                const std::uint32_t first = nextID;
                const std::uint32_t last = blockedID - 1u;

                stagedRanges_.push_back({ first, last });

                stagedReusableIDCount_ +=
                    static_cast<std::uint64_t>(last) -
                    static_cast<std::uint64_t>(first) + 1u;
            }

            nextID = blockedID + 1u;
        }

        if (nextID < highWater)
        {
            const std::uint32_t first = nextID;
            const std::uint32_t last = highWater - 1u;

            stagedRanges_.push_back({ first, last });

            stagedReusableIDCount_ +=
                static_cast<std::uint64_t>(last) -
                static_cast<std::uint64_t>(first) + 1u;
        }
    }
    catch (...)
    {
        stagedRanges_.clear();
        stagedReusableIDCount_ = 0;
        return false;
    }

    stagedPoolReady_ = true;
    return true;
}

void PersistentIdReuse::ClearStagedPool()
{
    stagedRanges_.clear();
    stagedReusableIDCount_ = 0;
    stagedPoolReady_ = false;
}

bool PersistentIdReuse::PromoteStagedPool()
{
    if (!stagedPoolReady_)
        return false;

    ranges_.swap(stagedRanges_);
    rangeIndex_ = 0;
    reusableIDCount_ = stagedReusableIDCount_;

    stagedRanges_.clear();
    stagedReusableIDCount_ = 0;
    stagedPoolReady_ = false;

    return true;
}

PersistentIdAllocationResult PersistentIdReuse::TryAllocate(
    SDK::FMassEntityHandle handle,
    SDK::FCrMassPersistentEntityID& outId)
{
    if (subsystem_ == nullptr ||
        setIDHandlePair_ == nullptr ||
        rangeIndex_ >= ranges_.size())
    {
        return PersistentIdAllocationResult::NoReusableId;
    }

    IdRange& range = ranges_[rangeIndex_];

    const uint32_t id = range.first;

    // For debugging:
    //LOG_INFO(
    //    "PersistentIdFix: allocating recycled ID %u (remaining before allocation: %llu)",
    //    id,
    //    static_cast<unsigned long long>(reusableIDCount_));

    SDK::FCrMassPersistentEntityID persistentId{};
    persistentId.ID = id;
    persistentId.CachedHandle = handle;

    if (!setIDHandlePair_(
        subsystem_,
        &persistentId,
        handle))
    {
        LOG_ERROR(
            "PersistentIdFix: SetIDHandlePair failed for recycled ID %u",
            id);

        return PersistentIdAllocationResult::Failed;
    }

    outId = persistentId;

    ++range.first;

    --reusableIDCount_;

    if (range.first > range.last)
        ++rangeIndex_;

    // For debugging:
    //LOG_INFO(
    //    "PersistentIdFix: recycled ID %u inserted successfully (remaining: %llu)",
    //    id,
    //    static_cast<unsigned long long>(reusableIDCount_));

    return PersistentIdAllocationResult::Allocated;
}

bool PersistentIdReuse::IsInitializedFor(
    SDK::UCrMassPersistentIDSubsystem* subsystem) const
{
    return subsystem_ == subsystem;
}

uint32_t PersistentIdReuse::GetSessionMaxID() const
{
    return sessionMaxID_;
}

uint64_t PersistentIdReuse::GetReusableIDCount() const
{
    return reusableIDCount_;
}

size_t PersistentIdReuse::GetRangeCount() const
{
    if (rangeIndex_ >= ranges_.size())
        return 0;

    return ranges_.size() - rangeIndex_;
}

uint64_t PersistentIdReuse::GetStagedReusableIDCount() const
{
    return stagedReusableIDCount_;
}

size_t PersistentIdReuse::GetStagedRangeCount() const
{
    return stagedRanges_.size();
}

bool PersistentIdReuse::GetStagedFirstRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (stagedRanges_.empty())
        return false;

    first = stagedRanges_.front().first;
    last = stagedRanges_.front().last;
    return true;
}

bool PersistentIdReuse::GetStagedLastRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (stagedRanges_.empty())
        return false;

    first = stagedRanges_.back().first;
    last = stagedRanges_.back().last;
    return true;
}

bool PersistentIdReuse::GetFirstRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (rangeIndex_ >= ranges_.size())
        return false;

    first = ranges_[rangeIndex_].first;
    last = ranges_[rangeIndex_].last;

    return true;
}

bool PersistentIdReuse::GetLastRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (rangeIndex_ >= ranges_.size())
        return false;

    first = ranges_.back().first;
    last = ranges_.back().last;

    return true;
}

SDK::UCrMassPersistentIDSubsystem*
PersistentIdReuse::GetSubsystem() const
{
    return subsystem_;
}
