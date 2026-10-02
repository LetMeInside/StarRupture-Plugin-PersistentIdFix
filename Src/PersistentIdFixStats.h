#pragma once

#include "plugin.h"

#include <cstdint>

namespace PersistentIdFixStats
{
    struct Snapshot
    {
        EPluginNetMode netMode = EPluginNetMode::Unknown;
        std::uint32_t totalEntities = 0;
        std::uint32_t idCounterValue = 0;
        std::uint64_t remainingIDCount = 0;
        std::uint64_t reusableIDCount = 0;
        std::uint64_t reusableIDRanges = 0;
        double issuedIDsPerMinute = 0.0;
        bool issuedIDsPerMinuteAvailable = false;
    };

    // Clears all collected statistics and starts a new measurement period.
    void Reset();

    // Records one newly issued persistent ID.
    // Existing-handle lookups must not call this.
    void RecordAssignment();

    // Updates the current non-rate statistics.
    void UpdateSnapshot(
        EPluginNetMode netMode,
        std::uint32_t totalEntities,
        std::uint32_t idCounterValue,
        std::uint64_t reusableIDCount,
        std::uint64_t reusableIDRanges);

    // Returns the latest statistics snapshot.
    Snapshot GetSnapshot();

    // Logs the latest statistics using the same terminology as the UI.
    void LogStats();
}

