#include "Network.h"

#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_network_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <typeinfo>
#include <vector>

namespace
{
    constexpr std::uint8_t kNetworkSchemaVersion = 1;

    // The AlienX network interface does not provide a separate "plugin
    // missing" notification. If the UI is open and the remote server has
    // not become ready within this period, the UI may report that the
    // remote PersistentIdFix instance appears to be unavailable.
    constexpr std::chrono::seconds kServerReadyTimeout{ 5 };

#pragma pack(push, 1)

    struct StatsInterestPacket
    {
        std::uint8_t schemaVersion = kNetworkSchemaVersion;
        std::uint8_t visible = 0;
    };

    struct StatsSnapshotPacket
    {
        std::uint8_t schemaVersion = kNetworkSchemaVersion;

        std::uint8_t netMode =
            static_cast<std::uint8_t>(EPluginNetMode::Unknown);

        std::uint8_t issuedIDsPerMinuteAvailable = 0;
        std::uint8_t reserved = 0;

        std::uint32_t totalEntities = 0;
        std::uint32_t idCounterValue = 0;

        std::uint64_t remainingIDCount = 0;
        std::uint64_t reusableIDCount = 0;
        std::uint64_t reusableIDRanges = 0;

        float issuedIDsPerMinute = 0.0f;
    };

#pragma pack(pop)

    static_assert(
        sizeof(StatsInterestPacket) == 2,
        "StatsInterestPacket layout changed unexpectedly");

    static_assert(
        sizeof(StatsSnapshotPacket) == 40,
        "StatsSnapshotPacket layout changed unexpectedly");


    IPluginSelf* g_networkSelf = nullptr;

    std::atomic_bool g_initialized{ false };
    std::atomic_bool g_sessionActive{ false };
    std::atomic_bool g_isServer{ false };

    // Client-side state.
    std::atomic_bool g_uiVisible{ false };
    std::atomic_bool g_serverReady{ false };
    bool g_hasRemoteSnapshot = false;

    PersistentIdFixStats::Snapshot g_remoteSnapshot = {};
    std::mutex g_clientStateMutex;

    std::chrono::steady_clock::time_point
        g_serverReadyWaitStarted{};

    // Authority-side subscription state.
    std::vector<void*> g_subscribedClients;
    std::mutex g_subscribedClientsMutex;

    // Typed network helper callback handles. These are required when
    // unregistering the handlers through the raw SDK functions.
    PluginNetworkServerMessageCallback
        g_statsInterestHandler = nullptr;

    PluginNetworkMessageCallback
        g_statsSnapshotHandler = nullptr;


    void ResetRemoteDisplayData()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_hasRemoteSnapshot = false;
        g_remoteSnapshot = {};
        g_serverReadyWaitStarted = {};
    }


    void ClearRemoteSnapshot()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_hasRemoteSnapshot = false;
    }


    void ClearRemoteSnapshotAndWait()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_hasRemoteSnapshot = false;
        g_serverReadyWaitStarted = {};
    }


    void StartServerReadyWait()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_hasRemoteSnapshot = false;
        g_serverReadyWaitStarted =
            std::chrono::steady_clock::now();
    }


    void StopServerReadyWait()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_serverReadyWaitStarted = {};
    }


    void PublishRemoteSnapshot(
        const PersistentIdFixStats::Snapshot& snapshot)
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);
        g_remoteSnapshot = snapshot;
        g_hasRemoteSnapshot = true;
        g_serverReadyWaitStarted = {};
    }


    void ClearSubscriptions()
    {
        std::lock_guard<std::mutex> lock(g_subscribedClientsMutex);
        g_subscribedClients.clear();
    }


    bool HasNetworkInterface()
    {
        return
            g_networkSelf != nullptr &&
            g_networkSelf->hooks != nullptr &&
            g_networkSelf->hooks->Network != nullptr;
    }


    bool IsSubscribed(void* playerController)
    {
        if (playerController == nullptr)
            return false;

        return std::find(
            g_subscribedClients.begin(),
            g_subscribedClients.end(),
            playerController) !=
            g_subscribedClients.end();
    }


    void AddSubscription(void* playerController)
    {
        std::lock_guard<std::mutex> lock(g_subscribedClientsMutex);

        if (playerController == nullptr)
            return;

        if (IsSubscribed(playerController))
            return;

        g_subscribedClients.push_back(playerController);

        LOG_INFO(
            "PersistentIdFix: statistics subscription added for "
            "player controller %p (subscribers=%zu)",
            playerController,
            g_subscribedClients.size());
    }


    void RemoveSubscription(void* playerController)
    {
        std::lock_guard<std::mutex> lock(g_subscribedClientsMutex);

        if (playerController == nullptr)
            return;

        const std::size_t oldSize =
            g_subscribedClients.size();

        g_subscribedClients.erase(
            std::remove(
                g_subscribedClients.begin(),
                g_subscribedClients.end(),
                playerController),
            g_subscribedClients.end());

        if (g_subscribedClients.size() != oldSize)
        {
            LOG_INFO(
                "PersistentIdFix: statistics subscription removed for "
                "player controller %p (subscribers=%zu)",
                playerController,
                g_subscribedClients.size());
        }
    }


    StatsSnapshotPacket MakeSnapshotPacket(
        const PersistentIdFixStats::Snapshot& snapshot)
    {
        StatsSnapshotPacket packet{};

        packet.schemaVersion =
            kNetworkSchemaVersion;

        packet.netMode =
            static_cast<std::uint8_t>(snapshot.netMode);

        packet.issuedIDsPerMinuteAvailable =
            snapshot.issuedIDsPerMinuteAvailable ? 1 : 0;

        packet.totalEntities =
            snapshot.totalEntities;

        packet.idCounterValue =
            snapshot.idCounterValue;

        packet.remainingIDCount =
            snapshot.remainingIDCount;

        packet.reusableIDCount =
            snapshot.reusableIDCount;

        packet.reusableIDRanges =
            snapshot.reusableIDRanges;

        packet.issuedIDsPerMinute =
            static_cast<float>(
                snapshot.issuedIDsPerMinute);

        return packet;
    }


    void OnStatsInterest(
        void* senderPlayerController,
        const StatsInterestPacket& packet)
    {
        if (!g_initialized ||
            !g_sessionActive ||
            !g_isServer ||
            senderPlayerController == nullptr)
        {
            return;
        }

        if (packet.schemaVersion !=
            kNetworkSchemaVersion)
        {
            LOG_WARN(
                "PersistentIdFix: ignoring statistics interest packet "
                "with unsupported schema version %u from player "
                "controller %p",
                static_cast<unsigned>(packet.schemaVersion),
                senderPlayerController);

            return;
        }

        if (packet.visible != 0)
        {
            AddSubscription(senderPlayerController);

            /*
             * Send a complete snapshot immediately when the client opens
             * the statistics window.
             *
             * The regular once-per-second update is handled by Plugin.cpp.
             */
            const PersistentIdFixStats::Snapshot snapshot =
                PersistentIdFixStats::GetSnapshot();

            if (HasNetworkInterface())
            {
                const StatsSnapshotPacket snapshotPacket =
                    MakeSnapshotPacket(snapshot);

                Network::SendPacketToPlayer(
                    g_networkSelf->hooks,
                    g_networkSelf,
                    senderPlayerController,
                    snapshotPacket);
            }
        }
        else
        {
            RemoveSubscription(senderPlayerController);
        }
    }


    void OnStatsSnapshot(
        const StatsSnapshotPacket& packet)
    {
        if (!g_initialized ||
            !g_sessionActive ||
            g_isServer)
        {
            return;
        }

        if (packet.schemaVersion !=
            kNetworkSchemaVersion)
        {
            LOG_WARN(
                "PersistentIdFix: ignoring statistics snapshot "
                "with unsupported schema version %u",
                static_cast<unsigned>(packet.schemaVersion));

            return;
        }

        PersistentIdFixStats::Snapshot snapshot{};

        snapshot.netMode =
            static_cast<EPluginNetMode>(
                packet.netMode);

        snapshot.totalEntities =
            packet.totalEntities;

        snapshot.idCounterValue =
            packet.idCounterValue;

        snapshot.remainingIDCount =
            packet.remainingIDCount;

        snapshot.reusableIDCount =
            packet.reusableIDCount;

        snapshot.reusableIDRanges =
            packet.reusableIDRanges;

        snapshot.issuedIDsPerMinute =
            static_cast<double>(
                packet.issuedIDsPerMinute);

        snapshot.issuedIDsPerMinuteAvailable =
            packet.issuedIDsPerMinuteAvailable != 0;

        PublishRemoteSnapshot(snapshot);

        /*
         * A snapshot can only arrive through the loader's exact-version
         * plugin network channel, so receipt confirms that the remote
         * PersistentIdFix instance is actually communicating with us.
         */
        g_serverReady = true;
    }


    void OnServerReady(const char* serverBuildTag)
    {
        if (!g_initialized ||
            !g_sessionActive ||
            g_isServer)
        {
            return;
        }

        g_serverReady = true;
        ClearRemoteSnapshotAndWait();

        LOG_INFO(
            "PersistentIdFix: remote server is ready (build tag=%s)",
            serverBuildTag != nullptr
            ? serverBuildTag
            : "<null>");

        /*
         * If the UI was already open when the server became ready,
         * establish the subscription now.
         */
        if (g_uiVisible &&
            HasNetworkInterface())
        {
            StatsInterestPacket packet{};

            packet.schemaVersion =
                kNetworkSchemaVersion;

            packet.visible = 1;

            Network::SendPacketToServer(
                g_networkSelf->hooks,
                g_networkSelf,
                packet);

            LOG_INFO(
                "PersistentIdFix: statistics subscription sent "
                "after remote server became ready");
        }
    }


    void OnPlayerLeft(void* playerController)
    {
        if (!g_initialized ||
            !g_sessionActive ||
            !g_isServer)
        {
            return;
        }

        RemoveSubscription(playerController);
    }


    void UnregisterSessionHandlers()
    {
        if (!HasNetworkInterface())
            return;

        if (g_isServer)
        {
            if (g_networkSelf->hooks->Players != nullptr &&
                g_networkSelf->hooks->Players->
                UnregisterOnPlayerLeft != nullptr)
            {
                g_networkSelf->hooks->Players->
                    UnregisterOnPlayerLeft(
                        &OnPlayerLeft);
            }

            if (g_statsInterestHandler != nullptr &&
                g_networkSelf->hooks->Network->
                UnregisterServerMessageHandler != nullptr)
            {
                g_networkSelf->hooks->Network->
                    UnregisterServerMessageHandler(
                        g_networkSelf,
                        typeid(StatsInterestPacket).name(),
                        g_statsInterestHandler);
            }

            g_statsInterestHandler = nullptr;
        }
        else
        {
            if (g_networkSelf->hooks->Network->
                UnregisterServerReadyCallback != nullptr)
            {
                g_networkSelf->hooks->Network->
                    UnregisterServerReadyCallback(
                        g_networkSelf,
                        &OnServerReady);
            }

            if (g_statsSnapshotHandler != nullptr &&
                g_networkSelf->hooks->Network->
                UnregisterMessageHandler != nullptr)
            {
                g_networkSelf->hooks->Network->
                    UnregisterMessageHandler(
                        g_networkSelf,
                        typeid(StatsSnapshotPacket).name(),
                        g_statsSnapshotHandler);
            }

            g_statsSnapshotHandler = nullptr;
        }

        ClearSubscriptions();
    }
}


namespace PersistentIdFixNetwork
{
    bool Initialize(IPluginSelf* self)
    {
        if (g_initialized)
            return true;

        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->Network == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: network initialization failed: "
                "network hooks unavailable");

            return false;
        }

        g_networkSelf = self;

        /*
         * IMPORTANT:
         *
         * Do not call Network->IsServer() here.
         *
         * PluginInit happens before the actual multiplayer session is
         * established. In particular, a listen host can initially report
         * false and later report true once its game session exists.
         */
        g_initialized = true;
        g_sessionActive = false;
        g_isServer = false;

        g_uiVisible = false;
        g_serverReady = false;
        ResetRemoteDisplayData();


        ClearSubscriptions();

        g_statsInterestHandler = nullptr;
        g_statsSnapshotHandler = nullptr;

        LOG_INFO(
            "PersistentIdFix: network interface initialized; "
            "waiting for game session");

        return true;
    }


    bool BeginSession()
    {
        if (!g_initialized ||
            g_networkSelf == nullptr ||
            g_networkSelf->hooks == nullptr ||
            g_networkSelf->hooks->Network == nullptr)
        {
            return false;
        }

        if (g_sessionActive)
            return true;

        /*
         * This is intentionally the first point where we ask IsServer().
         * Plugin orchestration calls BeginSession() only after the game world
         * becomes active, so listen-host role detection happens at the correct
         * lifecycle point.
         */
        if (g_networkSelf->hooks->Network->IsServer == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: network session initialization failed: "
                "IsServer() unavailable");

            return false;
        }

        g_isServer =
            g_networkSelf->hooks->Network->IsServer();

        g_sessionActive = true;

        g_serverReady = false;
        ResetRemoteDisplayData();

        ClearSubscriptions();

        if (g_isServer)
        {
            /*
             * Authority side:
             *
             * Remote clients explicitly subscribe/unsubscribe by sending
             * StatsInterestPacket.
             */
            g_statsInterestHandler =
                Network::OnServerReceive<StatsInterestPacket>(
                    g_networkSelf->hooks,
                    g_networkSelf,
                    &OnStatsInterest);

            if (g_statsInterestHandler == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to register statistics "
                    "interest packet handler");

                g_sessionActive = false;
                g_isServer = false;

                return false;
            }

            if (g_networkSelf->hooks->Players == nullptr ||
                g_networkSelf->hooks->Players->
                RegisterOnPlayerLeft == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: player-left hook unavailable");

                g_networkSelf->hooks->Network->
                    UnregisterServerMessageHandler(
                        g_networkSelf,
                        typeid(StatsInterestPacket).name(),
                        g_statsInterestHandler);

                g_statsInterestHandler = nullptr;

                g_sessionActive = false;
                g_isServer = false;

                return false;
            }

            g_networkSelf->hooks->Players->
                RegisterOnPlayerLeft(
                    &OnPlayerLeft);

            LOG_INFO(
                "PersistentIdFix: network session initialized "
                "as authority");
        }
        else
        {
            /*
             * Client side:
             *
             * Receive complete snapshots from the remote authority and
             * wait for the loader's server-ready notification before
             * sending subscription packets.
             */
            g_statsSnapshotHandler =
                Network::OnReceive<StatsSnapshotPacket>(
                    g_networkSelf->hooks,
                    g_networkSelf,
                    &OnStatsSnapshot);

            if (g_statsSnapshotHandler == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to register statistics "
                    "snapshot packet handler");

                g_sessionActive = false;

                return false;
            }

            if (g_uiVisible)
            {
                StartServerReadyWait();
            }

            if (g_networkSelf->hooks->Network->
                RegisterServerReadyCallback == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: server-ready callback unavailable");

                g_networkSelf->hooks->Network->
                    UnregisterMessageHandler(
                        g_networkSelf,
                        typeid(StatsSnapshotPacket).name(),
                        g_statsSnapshotHandler);

                g_statsSnapshotHandler = nullptr;

                g_sessionActive = false;

                return false;
            }

            /*
             * The SDK guarantees that this callback fires immediately if
             * the server is already ready, so this also handles a session
             * where BeginSession() happens after the handshake.
             */
            g_networkSelf->hooks->Network->
                RegisterServerReadyCallback(
                    g_networkSelf,
                    &OnServerReady);

            LOG_INFO(
                "PersistentIdFix: network session initialized "
                "as client");
        }

        return true;
    }


    void ResetSession()
    {
        if (!g_initialized)
            return;

        if (g_sessionActive)
        {
            UnregisterSessionHandlers();
        }

        g_sessionActive = false;
        g_isServer = false;

        g_serverReady = false;
        ResetRemoteDisplayData();


        /*
         * Deliberately retain g_uiVisible here.
         *
         * The UI lifecycle owns that state. Normally Hide() is called
         * during world cleanup. If the UI is still logically visible when
         * the next session begins, BeginSession() will establish the
         * appropriate subscription when the remote server becomes ready.
         */
    }


    void Shutdown()
    {
        if (!g_initialized)
            return;

        ResetSession();

        g_uiVisible = false;

        g_initialized = false;
        g_networkSelf = nullptr;
    }


    void SetUIVisible(bool visible)
    {
        g_uiVisible = visible;

        if (!g_initialized)
            return;

        /*
         * There is no network session yet.
         *
         * This is particularly important when Hide() is called from
         * OnAfterWorldEndPlay() while returning to the main menu. We
         * simply remember the UI state; we do not send a packet.
         */
        if (!g_sessionActive)
        {
            return;
        }

        /*
         * A listen host/dedicated server has no remote server to which its
         * own UI should send an interest packet.
         */
        if (g_isServer)
        {
            return;
        }

        if (!HasNetworkInterface())
            return;

        if (visible)
        {
            ClearRemoteSnapshot();

            /*
             * If the server is already ready, subscribe immediately.
             * Otherwise OnServerReady() will do it.
             */
            if (g_networkSelf->hooks->Network->
                IsServerReady != nullptr &&
                g_networkSelf->hooks->Network->
                IsServerReady())
            {
                g_serverReady = true;
                StopServerReadyWait();

                StatsInterestPacket packet{};

                packet.schemaVersion =
                    kNetworkSchemaVersion;

                packet.visible = 1;

                Network::SendPacketToServer(
                    g_networkSelf->hooks,
                    g_networkSelf,
                    packet);
            }
            else
            {
                g_serverReady = false;
                StartServerReadyWait();
            }
        }
        else
        {
            /*
             * Only send the unsubscribe if there is actually a server
             * connection ready to receive it. This prevents the warnings
             * seen when Hide() is called from a world/menu transition.
             */
            if (g_serverReady)
            {
                StatsInterestPacket packet{};

                packet.schemaVersion =
                    kNetworkSchemaVersion;

                packet.visible = 0;

                Network::SendPacketToServer(
                    g_networkSelf->hooks,
                    g_networkSelf,
                    packet);
            }

            ClearRemoteSnapshotAndWait();
        }
    }


    bool HasSubscribedClients()
    {
        if (!g_sessionActive || !g_isServer)
            return false;

        std::lock_guard<std::mutex> lock(g_subscribedClientsMutex);
        return !g_subscribedClients.empty();
    }


    void SendSnapshotToSubscribedClients(
        const PersistentIdFixStats::Snapshot& snapshot)
    {
        if (!g_sessionActive ||
            !g_isServer ||
            !HasNetworkInterface())
        {
            return;
        }

        std::vector<void*> subscribers;
        try
        {
            std::lock_guard<std::mutex> lock(g_subscribedClientsMutex);
            subscribers = g_subscribedClients;
        }
        catch (...)
        {
            LOG_ERROR(
                "PersistentIdFix: failed to snapshot statistics subscriber list");
            return;
        }

        if (subscribers.empty())
            return;

        const StatsSnapshotPacket packet =
            MakeSnapshotPacket(snapshot);

        for (void* playerController : subscribers)
        {
            if (playerController == nullptr)
                continue;

            Network::SendPacketToPlayer(
                g_networkSelf->hooks,
                g_networkSelf,
                playerController,
                packet);
        }
    }


    RemoteDisplayState GetRemoteDisplayState()
    {
        std::lock_guard<std::mutex> lock(g_clientStateMutex);

        RemoteDisplayState state{};

        state.serverReady =
            g_serverReady;

        state.hasData =
            g_hasRemoteSnapshot;

        state.uiSubscriptionActive =
            g_uiVisible;

        if (g_hasRemoteSnapshot)
        {
            state.snapshot =
                g_remoteSnapshot;
        }

        /*
         * This is necessarily an inference. The loader does not tell us
         * "PersistentIdFix is missing"; IsServerReady() can be false because
         * the server is still becoming ready, because the plugin is absent,
         * or because the exact plugin version does not match.
         */
        if (g_sessionActive &&
            !g_isServer &&
            g_uiVisible &&
            !g_serverReady &&
            g_serverReadyWaitStarted !=
            std::chrono::steady_clock::time_point{})
        {
            const auto now =
                std::chrono::steady_clock::now();

            if (now - g_serverReadyWaitStarted >=
                kServerReadyTimeout)
            {
                state.missingPlugin = true;
            }
        }

        return state;
    }


    bool IsSessionActive()
    {
        return
            g_initialized &&
            g_sessionActive;
    }


    bool IsServer()
    {
        return
            g_sessionActive &&
            g_isServer;
    }
}
