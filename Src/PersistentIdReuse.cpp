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
    reusableIDCount_ = 0;
}

void PersistentIdReuse::BuildPool()
{
    ranges_.clear();
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

PersistentIdAllocationResult PersistentIdReuse::TryAllocate(
    SDK::FMassEntityHandle handle,
    SDK::FCrMassPersistentEntityID& outId)
{
    if (subsystem_ == nullptr ||
        setIDHandlePair_ == nullptr ||
        ranges_.empty())
    {
        return PersistentIdAllocationResult::NoReusableId;
    }

    IdRange& range = ranges_.front();

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
        ranges_.erase(ranges_.begin());

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
    return ranges_.size();
}

bool PersistentIdReuse::GetFirstRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (ranges_.empty())
        return false;

    first = ranges_.front().first;
    last = ranges_.front().last;

    return true;
}

bool PersistentIdReuse::GetLastRange(
    uint32_t& first,
    uint32_t& last) const
{
    if (ranges_.empty())
        return false;

    first = ranges_.back().first;
    last = ranges_.back().last;

    return true;
}