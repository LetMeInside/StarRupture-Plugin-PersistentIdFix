#pragma once

#include "Stats/Stats.h"

#include <cstdint>

struct IPluginSelf;

namespace PersistentIdFixNetwork
{
    struct RemoteDisplayState
    {
        PersistentIdFixStats::Snapshot snapshot = {};

        bool serverReady = false;
        bool hasData = false;
        bool missingPlugin = false;
        bool uiSubscriptionActive = false;
    };

    // Stores the plugin interface and validates the network interface.
    // Does not establish whether this process is a server or client.
    bool Initialize(IPluginSelf* self);

    // Establishes the network role for the current game session and
    // registers the appropriate server/client network handlers.
    bool BeginSession();

    // Ends the current network session and unregisters its handlers.
    void ResetSession();

    // Releases all network resources during plugin shutdown.
    void Shutdown();

    // Called by the statistics UI whenever its visibility changes.
    void SetUIVisible(bool visible);

    // True when at least one remote client currently has the statistics
    // window subscribed on the authority.
    bool HasSubscribedClients();

    // Sends the complete current statistics snapshot to every subscribed
    // remote client.
    void SendSnapshotToSubscribedClients(
        const PersistentIdFixStats::Snapshot& snapshot);

    // Returns the latest remote-server snapshot/state for the client UI.
    RemoteDisplayState GetRemoteDisplayState();

    // True after BeginSession() has established the network role.
    bool IsSessionActive();

    // True when the current session is running with network authority.
    bool IsServer();
}