#include "Stats.h"

#include "plugin_helpers.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>

namespace
{
    constexpr std::size_t kStatisticsWindowMinutes = 10;

    struct PersistentIdStatistics
    {
        /*
         * Only completed one-minute buckets are stored here.
         *
         * currentBucketCount is deliberately separate so the incomplete
         * current minute can never accidentally enter the rate calculation.
         */
        std::array<std::uint32_t, kStatisticsWindowMinutes> completedBuckets{};

        std::uint32_t currentBucketCount = 0;

        std::size_t completedBucketWriteIndex = 0;
        std::size_t completedBucketCount = 0;

        /*
         * Running total of the completed buckets currently in the window.
         *
         * This avoids incorrectly summing the physical array order after
         * the ring buffer wraps.
         */
        std::uint64_t completedBucketTotal = 0;

        std::chrono::steady_clock::time_point currentBucketStart{};
        bool started = false;

        /*
         * Cached issued-IDs-per-minute value.
         *
         * This is recalculated only when a one-minute bucket completes.
         */
        double issuedIDsPerMinute = 0.0;
        bool issuedIDsPerMinuteAvailable = false;

        PersistentIdFixStats::Snapshot snapshot{};
    };

    PersistentIdStatistics g_statistics;
    std::mutex g_statisticsMutex;

    const char* GetSessionName(EPluginNetMode netMode)
    {
        switch (netMode)
        {
        case EPluginNetMode::Standalone:
            return "Solo";

        case EPluginNetMode::ListenServer:
            return "Local Multiplayer";

        case EPluginNetMode::DedicatedServer:
            return "Dedicated Server";

        case EPluginNetMode::Client:
            return "Remote Multiplayer";

        default:
            return "Unknown";
        }
    }

    void AddCompletedBucket(std::uint32_t count)
    {
        if (g_statistics.completedBucketCount ==
            kStatisticsWindowMinutes)
        {
            g_statistics.completedBucketTotal -=
                g_statistics.completedBuckets[
                    g_statistics.completedBucketWriteIndex];
        }

        g_statistics.completedBuckets[
            g_statistics.completedBucketWriteIndex] = count;

        g_statistics.completedBucketTotal += count;

        g_statistics.completedBucketWriteIndex =
            (g_statistics.completedBucketWriteIndex + 1) %
            kStatisticsWindowMinutes;

        if (g_statistics.completedBucketCount <
            kStatisticsWindowMinutes)
        {
            ++g_statistics.completedBucketCount;
        }
    }

    void UpdateCachedIssuedIDsPerMinute()
    {
        if (g_statistics.completedBucketCount == 0)
        {
            g_statistics.issuedIDsPerMinute = 0.0;
            g_statistics.issuedIDsPerMinuteAvailable = false;
            return;
        }

        g_statistics.issuedIDsPerMinute =
            static_cast<double>(
                g_statistics.completedBucketTotal) /
            static_cast<double>(
                g_statistics.completedBucketCount);

        g_statistics.issuedIDsPerMinuteAvailable = true;
    }

    bool AdvanceStatistics()
    {
        if (!g_statistics.started)
            return false;

        const auto now = std::chrono::steady_clock::now();

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::minutes>(
                now - g_statistics.currentBucketStart);

        if (elapsed.count() <= 0)
            return false;

        const std::size_t elapsedMinutes =
            static_cast<std::size_t>(elapsed.count());

        /*
         * If more than the entire ten-minute window has elapsed since the
         * current bucket started, all existing completed buckets and the
         * current bucket are too old to contribute to the new rate window.
         */
        if (elapsedMinutes > kStatisticsWindowMinutes)
        {
            g_statistics.completedBuckets.fill(0);
            g_statistics.completedBucketWriteIndex = 0;
            g_statistics.completedBucketCount = 0;
            g_statistics.completedBucketTotal = 0;
            g_statistics.currentBucketCount = 0;
            g_statistics.currentBucketStart = now;

            UpdateCachedIssuedIDsPerMinute();

            return true;
        }

        /*
         * The current bucket has completed.
         */
        AddCompletedBucket(
            g_statistics.currentBucketCount);

        g_statistics.currentBucketCount = 0;

        /*
         * Any additional elapsed minutes were completely inactive.
         */
        for (std::size_t i = 1; i < elapsedMinutes; ++i)
        {
            AddCompletedBucket(0);
        }

        g_statistics.currentBucketStart +=
            std::chrono::minutes(elapsedMinutes);

        UpdateCachedIssuedIDsPerMinute();

        return true;
    }
}

namespace PersistentIdFixStats
{
    void Reset()
    {
        std::lock_guard<std::mutex> lock(g_statisticsMutex);

        g_statistics = {};
    }

    void RecordAssignment()
    {
        std::lock_guard<std::mutex> lock(g_statisticsMutex);

        const auto now =
            std::chrono::steady_clock::now();

        if (!g_statistics.started)
        {
            g_statistics.started = true;
            g_statistics.currentBucketStart = now;
            g_statistics.currentBucketCount = 1;
            return;
        }

        AdvanceStatistics();

        ++g_statistics.currentBucketCount;
    }

    void UpdateSnapshot(
        EPluginNetMode netMode,
        std::uint32_t totalEntities,
        std::uint32_t idCounterValue,
        std::uint64_t reusableIDCount,
        std::uint64_t reusableIDRanges)
    {
        std::lock_guard<std::mutex> lock(g_statisticsMutex);

        AdvanceStatistics();

        g_statistics.snapshot.netMode = netMode;
        g_statistics.snapshot.totalEntities = totalEntities;
        g_statistics.snapshot.idCounterValue = idCounterValue;

        constexpr std::uint32_t kMaxAssignablePersistentId =
            UINT32_MAX - 1u;

        g_statistics.snapshot.remainingIDCount =
            idCounterValue < kMaxAssignablePersistentId
                ? static_cast<std::uint64_t>(
                    kMaxAssignablePersistentId) -
                    static_cast<std::uint64_t>(
                        idCounterValue)
                : 0u;

        g_statistics.snapshot.reusableIDCount =
            reusableIDCount;

        g_statistics.snapshot.reusableIDRanges =
            reusableIDRanges;

        g_statistics.snapshot.issuedIDsPerMinuteAvailable =
            g_statistics.issuedIDsPerMinuteAvailable;

        g_statistics.snapshot.issuedIDsPerMinute =
            g_statistics.issuedIDsPerMinute;
    }

    Snapshot GetSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_statisticsMutex);

        /*
         * This is deliberately allowed to advance the current minute bucket,
         * but the actual IDs/minute calculation is cached and only changes
         * when a minute bucket completes.
         */
        AdvanceStatistics();

        return g_statistics.snapshot;
    }

    void LogStats()
    {
        const Snapshot snapshot =
            GetSnapshot();

        LOG_INFO(
            "PersistentIdFix: Game Session = %s",
            GetSessionName(snapshot.netMode));

        LOG_INFO(
            "PersistentIdFix: Total Entities = %u",
            snapshot.totalEntities);

        LOG_INFO(
            "PersistentIdFix: ID Counter value = %u",
            snapshot.idCounterValue);

        LOG_INFO(
            "PersistentIdFix: Remaining ID count = %llu",
            static_cast<unsigned long long>(
                snapshot.remainingIDCount));

        LOG_INFO(
            "PersistentIdFix: Reusable ID count = %llu",
            static_cast<unsigned long long>(
                snapshot.reusableIDCount));

        LOG_INFO(
            "PersistentIdFix: Reusable ID ranges = %llu",
            static_cast<unsigned long long>(
                snapshot.reusableIDRanges));

        if (snapshot.issuedIDsPerMinuteAvailable)
        {
            LOG_INFO(
                "PersistentIdFix: Issued IDs per minute = %.2f",
                snapshot.issuedIDsPerMinute);
        }
        else
        {
            LOG_INFO(
                "PersistentIdFix: Issued IDs per minute = collecting...");
        }
    }
}
