#include "plugin.h"
#include "PersistentIdReuse.h"
#include "PersistentIdNativeLookup.h"

#include <cstdint>

IPluginSelf* g_self = nullptr;

static PersistentIdReuse g_persistentIdReuse;

static HookHandle g_getOrAddIDForHandleHook = nullptr;
static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr; // Not actively used, but is an SDK requirement.
static SetIDHandlePairFn g_setIDHandlePair = nullptr;

static uintptr_t g_getOrAddIDForHandleAddress = 0;
static uintptr_t g_getOrAddFingerprintMapAddress = 0;
static uintptr_t g_getOrAddFingerprintBucketAddress = 0;
static uintptr_t g_getOrAddFingerprintElementAddress = 0;
static uintptr_t g_getOrAddFingerprintMaxIDAddress = 0;

static uintptr_t g_setIDHandlePairAddress = 0;

static bool g_reusePoolReady = false;

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#ifdef MODLOADER_SERVER_BUILD
#define PERSISTENT_ID_FIX_TARGET PLUGIN_TARGET_SERVER
#else
#define PERSISTENT_ID_FIX_TARGET PLUGIN_TARGET_CLIENT
#endif

static PluginInfo s_pluginInfo = {
    "PersistentIdFix",
    MODLOADER_BUILD_TAG,
    "Kian369",
    "Reuses unused persistent IDs from the loaded save to prevent persistent ID exhaustion",
    PLUGIN_INTERFACE_VERSION,
    PERSISTENT_ID_FIX_TARGET
};

// ---------------------------------------------------------------------------
// GetOrAddIDForHandle detour.
//
// The native function returns FCrMassPersistentEntityID by value. The MSVC
// ABI passes the hidden return buffer as the second argument:
//
//   RCX = subsystem
//   RDX = return-value buffer
//   R8  = FMassEntityHandle
//
// Existing handles are resolved directly through the native HandleIDMap
// representation. They are not forwarded to the native implementation.
//
// Only genuinely new handles are allowed to consume an ID from our pool.
// If the pool is empty, the native allocation semantics are reproduced
// using MaxID and SetIDHandlePair.
// ---------------------------------------------------------------------------

static SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
    void* subsystem,
    SDK::FCrMassPersistentEntityID* returnValue,
    SDK::FMassEntityHandle handle)
{
    if (returnValue == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: GetOrAddIDForHandle return buffer is null");

        return nullptr;
    }
    *returnValue = {};

    if (subsystem == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: GetOrAddIDForHandle subsystem is null");

        return returnValue;
    }

    auto* persistentIDSubsystem =
        static_cast<SDK::UCrMassPersistentIDSubsystem*>(subsystem);

    /*
     * First reproduce the native HandleIDMap lookup.
     *
     * This must happen before allocating anything.
     */
    const SDK::FCrMassPersistentEntityID* existingId =
        FindPersistentIdByHandle(
            persistentIDSubsystem,
            handle);

    if (existingId != nullptr)
    {
        *returnValue = *existingId;
        return returnValue;
    }

    if (!g_reusePoolReady)
    {
        LOG_ERROR(
            "PersistentIdFix: GetOrAddIDForHandle MISS before reuse pool was built: "
            "handle=(%u,%u) MaxID=%u",
            handle.Index,
            handle.SerialNumber,
            persistentIDSubsystem->MaxID);
    }

    /*
     * No existing mapping.
     *
     * First consume an ID hole that existed when the save was loaded.
     */
    switch (g_persistentIdReuse.TryAllocate(handle, *returnValue))
    {
    case PersistentIdAllocationResult::Allocated:
        return returnValue;

    case PersistentIdAllocationResult::NoReusableId:
        break;

    case PersistentIdAllocationResult::Failed:
        // this is logged inside TryAllocate
        return returnValue;
    }

    /*
     * No reusable IDs remain.
     *
     * Allocate exactly as the native function does.
     */
    if (persistentIDSubsystem->MaxID == UINT32_MAX)
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID space exhausted; "
            "no reusable IDs remain and MaxID is UINT32_MAX");

        return returnValue;
    }
    ++persistentIDSubsystem->MaxID;

    SDK::FCrMassPersistentEntityID persistentId{};

    persistentId.ID =
        persistentIDSubsystem->MaxID;

    persistentId.CachedHandle =
        handle;

    if (g_setIDHandlePair == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: SetIDHandlePair is unavailable");

        return returnValue;
    }

    if (!g_setIDHandlePair(
        persistentIDSubsystem,
        &persistentId,
        handle))
    {
        LOG_ERROR(
            "PersistentIdFix: SetIDHandlePair failed for new ID %u "
            "handle=(%u,%u)",
            persistentId.ID,
            handle.Index,
            handle.SerialNumber);

        const uint8_t* map =
            reinterpret_cast<const uint8_t*>(
                &persistentIDSubsystem->HandleIDMap);

        const uint8_t* elementsData =
            *reinterpret_cast<const uint8_t* const*>(
                map + HandleIDMapElementsDataOffset);

        const int32_t elementsNum =
            *reinterpret_cast<const int32_t*>(
                map + HandleIDMapElementsNumOffset);

        bool foundByLinearScan = false;

        if (elementsData != nullptr && elementsNum > 0)
        {
            for (int32_t i = 0; i < elementsNum; ++i)
            {
                const uint8_t* element =
                    elementsData +
                    static_cast<size_t>(i) *
                    SetElementStride;

                const auto* elementHandle =
                    reinterpret_cast<const SDK::FMassEntityHandle*>(
                        element + SetElementHandleOffset);

                if (elementHandle->Index == handle.Index &&
                    elementHandle->SerialNumber == handle.SerialNumber)
                {
                    const uint32_t existingPersistentId =
                        *reinterpret_cast<const uint32_t*>(
                            element + SetElementPersistentIdOffset);

                    LOG_ERROR(
                        "PersistentIdFix: FAILED allocation handle "
                        "FOUND by linear scan: "
                        "element=%d "
                        "handle=(%u,%u) "
                        "existingPersistentId=%u "
                        "elementsNum=%d",
                        i,
                        elementHandle->Index,
                        elementHandle->SerialNumber,
                        existingPersistentId,
                        elementsNum);

                    foundByLinearScan = true;
                    break;
                }
            }
        }

        if (!foundByLinearScan)
        {
            LOG_ERROR(
                "PersistentIdFix: FAILED allocation handle "
                "NOT FOUND by linear scan: "
                "handle=(%u,%u) "
                "elementsData=%p "
                "elementsNum=%d",
                handle.Index,
                handle.SerialNumber,
                elementsData,
                elementsNum);
        }

        return returnValue;
    }

    *returnValue = persistentId;

    return returnValue;
}

// ---------------------------------------------------------------------------
// Save-loaded diagnostic.
// ---------------------------------------------------------------------------

static void OnSaveLoaded()
{
    g_persistentIdReuse.Clear();

    if (g_self == nullptr || g_self->hooks == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: hooks are unavailable in OnSaveLoaded");
        return;
    }

    auto* walker = g_self->hooks->ObjectWalker;

    if (walker == nullptr || !walker->IsReady())
    {
        LOG_ERROR(
            "PersistentIdFix: ObjectWalker is unavailable");
        return;
    }

    PluginObjectInfo objects[8] = {};

    const int count = walker->FindObjectsByClassNameInto(
        "CrMassPersistentIDSubsystem",
        PluginObjectLookup_InstanceOnly,
        objects,
        8);

    LOG_INFO(
        "PersistentIdFix: found %d CrMassPersistentIDSubsystem instance(s)",
        count);

    if (count <= 0)
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID subsystem was not found");
        return;
    }
    else if (count > 1)
    {
        LOG_ERROR(
            "PersistentIdFix: expected exactly one CrMassPersistentIDSubsystem, found %d",
            count);

        return;
    }

    for (int i = 0; i < count; ++i)
    {
        void* object = objects[i].object;

        LOG_INFO(
            "PersistentIdFix: subsystem[%d] object=%p name=%s",
            i,
            object,
            objects[i].objectName);

        auto* subsystem =
            static_cast<SDK::UCrMassPersistentIDSubsystem*>(object);

        LOG_INFO(
            "PersistentIdFix: IDHandleMap.Num() = %d",
            subsystem->IDHandleMap.Num());

        // Start O(n) scan, only used for debugging:
        uint32_t minId = UINT32_MAX;
        uint32_t maxCurrentId = 0;

        for (const auto& pair : subsystem->IDHandleMap)
        {
            const uint32_t id = pair.Key().ID;

            if (id < minId)
                minId = id;

            if (id > maxCurrentId)
                maxCurrentId = id;
        }

        LOG_INFO(
            "PersistentIdFix: current persistent ID range = %u .. %u",
            minId,
            maxCurrentId);
        // End O(n) scan for debugging.

        uint64_t maxId = 0;
        bool propertyRead = false;

        auto* properties = g_self->hooks->ObjectProperties;

        if (properties != nullptr && properties->IsReady())
        {
            PluginPropertyHandle property =
                properties->FindPropertyOnObject(object, "MaxID");

            if (property != nullptr)
            {
                int64_t value = 0;

                if (properties->GetIntProperty(
                    object,
                    property,
                    &value))
                {
                    if (value < 0 ||
                        static_cast<uint64_t>(value) > UINT32_MAX)
                    {
                        LOG_ERROR(
                            "PersistentIdFix: MaxID FProperty value is outside uint32 range: %lld",
                            static_cast<long long>(value));

                        return;
                    }

                    maxId = static_cast<uint64_t>(value);
                    propertyRead = true;

                    LOG_INFO(
                        "PersistentIdFix: MaxID read through FProperty = %llu",
                        static_cast<unsigned long long>(maxId));
                }
                else
                {
                    LOG_WARN(
                        "PersistentIdFix: MaxID property was found but GetIntProperty failed");
                }
            }
            else
            {
                LOG_WARN(
                    "PersistentIdFix: MaxID was not found as an FProperty");
            }
        }
        else
        {
            LOG_WARN(
                "PersistentIdFix: ObjectProperties is unavailable");
        }

        if (!propertyRead)
        {
            // UCrMassPersistentIDSubsystem::MaxID is documented by the
            // generated SDK at offset 0xD0. Read only; never write here.
            const auto* bytes =
                static_cast<const std::uint8_t*>(object);

            maxId =
                static_cast<uint64_t>(
                    *reinterpret_cast<const std::uint32_t*>(
                        bytes + 0xD0));

            LOG_INFO(
                "PersistentIdFix: MaxID read through SDK offset 0xD0 = %llu",
                static_cast<unsigned long long>(maxId));
        }

        LOG_INFO(
            "PersistentIdFix: UINT32_MAX = %llu",
            static_cast<unsigned long long>(UINT32_MAX));

        LOG_INFO(
            "PersistentIdFix: IDs remaining below UINT32_MAX = %llu",
            static_cast<unsigned long long>(
                UINT32_MAX -
                static_cast<std::uint32_t>(maxId)));

        // Build the reusable pool from holes below the loaded high-water mark.
        g_persistentIdReuse.Initialize(
            subsystem,
            g_setIDHandlePair);

        g_persistentIdReuse.BuildPool();
        g_reusePoolReady = true;

        LOG_INFO(
            "PersistentIdFix: session MaxID = %u",
            g_persistentIdReuse.GetSessionMaxID());

        LOG_INFO(
            "PersistentIdFix: reusable ID count = %llu",
            static_cast<unsigned long long>(
                g_persistentIdReuse.GetReusableIDCount()));

        LOG_INFO(
            "PersistentIdFix: reusable ID ranges = %llu",
            static_cast<unsigned long long>(
                g_persistentIdReuse.GetRangeCount()));

        uint32_t firstRangeFirst = 0;
        uint32_t firstRangeLast = 0;

        if (g_persistentIdReuse.GetFirstRange(
            firstRangeFirst,
            firstRangeLast))
        {
            LOG_INFO(
                "PersistentIdFix: first reusable range = %u .. %u",
                firstRangeFirst,
                firstRangeLast);
        }

        uint32_t lastRangeFirst = 0;
        uint32_t lastRangeLast = 0;

        if (g_persistentIdReuse.GetLastRange(
            lastRangeFirst,
            lastRangeLast))
        {
            LOG_INFO(
                "PersistentIdFix: last reusable range = %u .. %u",
                lastRangeFirst,
                lastRangeLast);
        }
    }
}

// ---------------------------------------------------------------------------
// World-end callback.
//
// The persistent ID subsystem belongs to the current world/save. Clear our
// bookkeeping when the world ends so we never retain a pointer to a subsystem
// that has already been destroyed.
//
// Do not dereference 'world' here. The callback is only used as a lifecycle
// boundary for our own state.
// ---------------------------------------------------------------------------

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
    LOG_INFO(
        "PersistentIdFix: world ended: %s",
        worldName != nullptr ? worldName : "<null>");

    g_persistentIdReuse.Clear();
    g_reusePoolReady = false;
}

// ---------------------------------------------------------------------------
// Pattern resolution.
//
// This is the ONLY place where the scanner is used. The returned addresses
// remain valid after this callback; the scanner table itself does not.
//
// Each Resolve() must produce exactly the expected unique match.
// The fingerprint addresses are then required to resolve back to the same
// GetOrAddIDForHandle function.
//
// If any required fingerprint cannot be resolved, this callback returns
// without completing validation. A successful validation explicitly calls
// ReportFailure() if the fingerprints do not all resolve to the same
// function, preventing the plugin from continuing with an incompatible
// native implementation.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(
    IPluginSelf* self,
    IPluginHookScanner* scanner)
{
    if (self == nullptr || scanner == nullptr)
        return;

    // SetIDHandlePair:
    //
    // This is the native function used to insert a persistent-ID/handle
    // mapping into both directions of the persistent-ID subsystem.
    //
    // Function prologue:
    //
    //   4C 89 44 24 18      mov [rsp+18h],r8
    //                         save FMassEntityHandle argument
    //
    //   48 89 54 24 10      mov [rsp+10h],rdx
    //                         save persistent-ID argument
    //
    //   53                  push rbx
    //   41 54               push r12
    //   41 57               push r15
    //   48 81 EC 80 00 00 00
    //                       sub rsp,80h
    //
    // The function is used by PersistentIdFix for both recycled IDs and
    // newly allocated IDs. It performs the native duplicate checks and
    // inserts the ID/handle pair into the subsystem's maps.
    //
    // Only the function prologue is used as the fingerprint here. The
    // GetOrAddIDForHandle fingerprints above provide the structural
    // compatibility validation for the map implementation that our direct
    // lookup depends on.
    //
    // A successful result must resolve to the function start.

    PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::SetIDHandlePair";
    req.pattern =
        "4C 89 44 24 18 "
        "48 89 54 24 10 "
        "53 "
        "41 54 "
        "41 57 "
        "48 81 EC 80 00 00 00";

    req.kind =
        PLUGIN_SCAN_FUNCTION_START;

    g_setIDHandlePairAddress =
        scanner->Resolve(self, &req);

    if (g_setIDHandlePairAddress == 0)
        return;

    // GetOrAddIDForHandle:
    //
    // The function prologue establishes the native function entry point and
    // captures the incoming FMassEntityHandle in the registers used below.
    //
    //   48 89 5C 24 10      mov [rsp+10h],rbx
    //   4C 89 44 24 18      mov [rsp+18h],r8
    //   57                  push rdi
    //   48 83 EC 40         sub rsp,40h
    //   4C 8B D9            mov r11,rcx
    //   48 8D 99 80 00 00 00
    //                       lea rbx,[rcx+80h]
    //
    //   r11 = UCrMassPersistentIDSubsystem
    //   r8  = incoming FMassEntityHandle
    //   rbx = subsystem + 0x80 = HandleIDMap
    //
    // Only the first part of the prologue is used as the function fingerprint.
    // The remaining instructions are documented above because they establish
    // the native register/layout relationship used by the later fingerprints.
    //
    // The pattern includes the function prologue plus the first instructions
    // that establish the native register/layout relationship:
    //
    //   4C 8B D9            mov r11,rcx
    //                         r11 = UCrMassPersistentIDSubsystem
    //
    //   48 8D 99 80 00 00 00
    //                         lea rbx,[rcx+80h]
    //                         rbx = subsystem + 0x80 = HandleIDMap
    //
    //   4D 8B D0            mov r10,r8
    //                         r10 = incoming FMassEntityHandle
    //
    //   45 8B C8            mov r9d,r8d
    //                         r9d = handle.Index
    //
    //   49 C1 EA 20         shr r10,20h
    //                         r10d = handle.SerialNumber
    //
    // Including these instructions makes the function-start fingerprint
    // sufficiently specific to distinguish GetOrAddIDForHandle from unrelated
    // functions sharing the same compiler-generated prologue.
    //
    // A successful result must resolve to the function start.
    //
    // A successful result must resolve to the function start.
 
    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::GetOrAddIDForHandle";
    req.pattern =
        "48 89 5C 24 10 "
        "4C 89 44 24 18 "
        "57 "
        "48 83 EC 40 "
        "4C 8B D9 "
        "48 8D 99 80 00 00 00 "
        "4D 8B D0 "
        "45 8B C8 "
        "49 C1 EA 20";

    req.kind =
        PLUGIN_SCAN_FUNCTION_START;

    g_getOrAddIDForHandleAddress =
        scanner->Resolve(self, &req);

    if (g_getOrAddIDForHandleAddress == 0)
        return;

    // GetOrAddIDForHandle.MapLayout:
    //
    // This is the native setup of the HandleIDMap hash table.
    //
    //   48 8B 53 40      mov rdx,[rbx+40h]
    //                     rdx = HandleIDMap.Hash.SecondaryData
    //
    //   48 8D 43 38      lea rax,[rbx+38h]
    //                     rax = HandleIDMap.Hash inline storage
    //
    //   8B 4B 48         mov ecx,[rbx+48h]
    //                     ecx = HandleIDMap.HashSize
    //
    //   48 85 D2         test rdx,rdx
    //
    //   48 0F 44 D0      cmove rdx,rax
    //                     use inline hash storage when SecondaryData is null
    //
    //   FF C9            dec ecx
    //                     prepares HashSize - 1 for the bucket mask
    //
    // These offsets correspond to the generated TSet/TMap layout:
    //
    //   +0x38 = inline hash storage
    //   +0x40 = secondary hash storage pointer
    //   +0x48 = hash size
    //
    // The fingerprint therefore verifies that the game is still using the
    // expected native HandleIDMap hash representation.
    //
    // We fingerprint a larger, more distinctive sequence that crosses the hash lookup and 
    // enters the persistent-ID-specific code.

    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::GetOrAddIDForHandle.MapLayout";
    req.pattern =
        "41 3B 83 B4 00 00 00 "
        "74 5D "
        "48 8B 53 40 "
        "48 8D 43 38 "
        "8B 4B 48 "
        "48 85 D2 "
        "48 0F 44 D0 "
        "FF C9";

    req.resultOffset = -0xAE;
    req.kind = PLUGIN_SCAN_IN_FUNCTION;

    g_getOrAddFingerprintMapAddress =
        scanner->Resolve(self, &req);

    if (g_getOrAddFingerprintMapAddress == 0)
        return;

    // GetOrAddIDForHandle.BucketLookup:
    //
    // This is the native hash-table bucket lookup.
    //
    //   FF C9            dec ecx
    //                     ecx = HashSize - 1
    //
    //   41 8B C2         mov eax,r10d
    //                     eax = native handle hash
    //
    //   48 23 C8         and rcx,rax
    //                     bucket = hash & (HashSize - 1)
    //
    //   44 8B 0C 8A      mov r9d,[rdx+rcx*4]
    //                     r9d = sparse-array element index stored in bucket
    //
    //   41 83 F9 FF      cmp r9d,FFFFFFFFh
    //                     0xFFFFFFFF means the bucket is empty
    //
    // This fingerprint verifies the actual bucket calculation used by the
    // native HandleIDMap lookup rather than merely verifying that a TMap exists.

    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::GetOrAddIDForHandle.BucketLookup";
    req.pattern =
        "FF C9 "
        "41 8B C2 "
        "48 23 C8 "
        "44 8B 0C 8A "
        "41 83 F9 FF "
        "74 39";

    req.resultOffset = -0xC9;
    req.kind = PLUGIN_SCAN_IN_FUNCTION;

    g_getOrAddFingerprintBucketAddress =
        scanner->Resolve(self, &req);

    if (g_getOrAddFingerprintBucketAddress == 0)
        return;

    // GetOrAddIDForHandle.ElementLayout:
    //
    // This is the native sparse-array element lookup.
    //
    //   49 63 C1         movsxd rax,r9d
    //                     convert sparse-array element index to 64-bit
    //
    //   48 C1 E0 05      shl rax,5
    //                     element offset = index * 0x20
    //
    //   46 39 04 10      cmp [rax+r10],r8d
    //                     compare FMassEntityHandle.Index at element +0x00
    //
    //   4A 8D 0C 10      lea rcx,[rax+r10]
    //                     rcx = address of the sparse-array element
    //
    // The 0x20-byte stride is the native TSet element size:
    //
    //   +0x00  FMassEntityHandle.Index
    //   +0x04  FMassEntityHandle.SerialNumber
    //   +0x08  FCrMassPersistentEntityID
    //   +0x18  HashNextId
    //   +0x1C  HashIndex
    //
    // The subsequent native instructions compare the SerialNumber at +0x04,
    // then access the persistent ID and hash-chain fields.
    //
    // This fingerprint therefore verifies the element layout required by our
    // direct HandleIDMap lookup.

    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::GetOrAddIDForHandle.ElementLayout";
    req.pattern =
        "49 63 C1 "
        "48 C1 E0 05 "
        "46 39 04 10 "
        "4A 8D 0C 10 "
        "75 09 "
        "39 51 04 "
        "0F 84 8A 00 00 00";

    req.resultOffset = -0xF0;
    req.kind = PLUGIN_SCAN_IN_FUNCTION;

    g_getOrAddFingerprintElementAddress =
        scanner->Resolve(self, &req);

    if (g_getOrAddFingerprintElementAddress == 0)
        return;

    // GetOrAddIDForHandle.MaxID:
    //
    // This is the native persistent-ID allocation operation:
    //
    //   41 FF 83 D0 00 00 00
    //   inc dword ptr [r11+0D0h]
    //
    // r11 is the UCrMassPersistentIDSubsystem, established in the function
    // prologue:
    //
    //   mov r11,rcx
    //
    // Therefore:
    //
    //   subsystem + 0xD0 = MaxID
    //
    // This is the native high-water-mark increment performed when the incoming
    // FMassEntityHandle has no existing entry in HandleIDMap.
    //
    // The fingerprint is important because PersistentIdFix replaces this
    // allocation path. If the location or meaning of MaxID changes in a future
    // game version, the plugin must refuse to install the hook.

    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::GetOrAddIDForHandle.MaxID";
    req.pattern =
        "41 FF 83 D0 00 00 00";

    req.resultOffset = -0x114;
    req.kind = PLUGIN_SCAN_FUNCTION_START;

    g_getOrAddFingerprintMaxIDAddress =
        scanner->Resolve(self, &req);

    if (g_getOrAddIDForHandleAddress == 0 ||
        g_getOrAddFingerprintMapAddress !=
        g_getOrAddIDForHandleAddress ||
        g_getOrAddFingerprintBucketAddress !=
        g_getOrAddIDForHandleAddress ||
        g_getOrAddFingerprintElementAddress !=
        g_getOrAddIDForHandleAddress ||
        g_getOrAddFingerprintMaxIDAddress !=
        g_getOrAddIDForHandleAddress)
    {
        scanner->ReportFailure(
            self,
            "PersistentIdFix::GetOrAddIDForHandle",
            "Native fingerprints did not resolve to the same function");

        g_getOrAddIDForHandleAddress = 0;
    }

    // SetIDHandlePair:
    //
    // This is the native function used to insert a persistent-ID/handle
    // mapping into both directions of the persistent-ID subsystem.
    //
    // Function prologue:
    //
    //   4C 89 44 24 18      mov [rsp+18h],r8
    //                         save FMassEntityHandle argument
    //
    //   48 89 54 24 10      mov [rsp+10h],rdx
    //                         save persistent-ID argument
    //
    //   53                  push rbx
    //   41 54               push r12
    //   41 57               push r15
    //   48 81 EC 80 00 00 00
    //                       sub rsp,80h
    //
    // The function is used by PersistentIdFix for both recycled IDs and
    // newly allocated IDs. It performs the native duplicate checks and
    // inserts the ID/handle pair into the subsystem's maps.
    //
    // Only the function prologue is used as the fingerprint here. The
    // GetOrAddIDForHandle fingerprints above provide the structural
    // compatibility validation for the map implementation that our direct
    // lookup depends on.
    //
    // A successful result must resolve to the function start.

    req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName =
        "PersistentIdFix::SetIDHandlePair";
    req.pattern =
        "4C 89 44 24 18 "
        "48 89 54 24 10 "
        "53 "
        "41 54 "
        "41 57 "
        "48 81 EC 80 00 00 00";

    req.kind =
        PLUGIN_SCAN_FUNCTION_START;

    g_setIDHandlePairAddress =
        scanner->Resolve(self, &req);

    if (g_setIDHandlePairAddress == 0)
        return;
}

// ---------------------------------------------------------------------------
// Plugin lifecycle.
// ---------------------------------------------------------------------------

extern "C"
{

    __declspec(dllexport)
        PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport)
        bool PluginInit(IPluginSelf* self)
    {
        g_self = self;

        if (g_self == nullptr || g_self->hooks == nullptr)
        {
            return false;
        }

        if (g_getOrAddIDForHandleAddress == 0)
        {
            LOG_ERROR(
                "PersistentIdFix: GetOrAddIDForHandle address was not resolved");
            return false;
        }

        if (g_setIDHandlePairAddress == 0)
        {
            LOG_ERROR(
                "PersistentIdFix: SetIDHandlePair address was not resolved");
            return false;
        }

        if (g_self->hooks->Hooks == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: native hook interface is unavailable");
            return false;
        }

        LOG_INFO(
            "PersistentIdFix: GetOrAddIDForHandle resolved at %p",
            reinterpret_cast<void*>(g_getOrAddIDForHandleAddress));

        LOG_INFO(
            "PersistentIdFix: SetIDHandlePair resolved at %p",
            reinterpret_cast<void*>(g_setIDHandlePairAddress));

        g_setIDHandlePair =
            reinterpret_cast<SetIDHandlePairFn>(
                g_setIDHandlePairAddress);

        g_getOrAddIDForHandleHook =
            g_self->hooks->Hooks->Install(
                g_getOrAddIDForHandleAddress,
                reinterpret_cast<void*>(&GetOrAddIDForHandleDetour),
                reinterpret_cast<void**>(
                    &g_originalGetOrAddIDForHandle));

        if (g_getOrAddIDForHandleHook == nullptr ||
            g_originalGetOrAddIDForHandle == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: failed to install GetOrAddIDForHandle hook");

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;
            g_setIDHandlePair = nullptr;

            return false;
        }

        LOG_INFO(
            "PersistentIdFix: GetOrAddIDForHandle hook installed");

        if (g_self->hooks->World == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: World hooks are unavailable");

            g_self->hooks->Hooks->Remove(
                g_getOrAddIDForHandleHook);

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;
            g_setIDHandlePair = nullptr;

            return false;
        }

        g_self->hooks->World->RegisterOnSaveLoaded(
            &OnSaveLoaded);

        LOG_INFO(
            "PersistentIdFix: registered OnSaveLoaded callback");

        g_self->hooks->World->RegisterOnAfterWorldEndPlay(
            &OnAfterWorldEndPlay);

        LOG_INFO(
            "PersistentIdFix: registered OnAfterWorldEndPlay callback");

        LOG_INFO(
            "PersistentIdFix: initialization complete");

        return true;
    }


    __declspec(dllexport)
        void PluginShutdown()
    {
        LOG_INFO(
            "PersistentIdFix: shutting down");

        if (g_self != nullptr &&
            g_self->hooks != nullptr)
        {
            if (g_self->hooks->World != nullptr)
            {
                g_self->hooks->World->UnregisterOnSaveLoaded(
                    &OnSaveLoaded);

                g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                    &OnAfterWorldEndPlay);
            }

            if (g_getOrAddIDForHandleHook != nullptr &&
                g_self->hooks->Hooks != nullptr)
            {
                g_self->hooks->Hooks->Remove(
                    g_getOrAddIDForHandleHook);
            }
        }

        g_persistentIdReuse.Clear();

        g_getOrAddIDForHandleHook = nullptr;
        g_originalGetOrAddIDForHandle = nullptr;
        g_setIDHandlePair = nullptr;

        g_getOrAddIDForHandleAddress = 0;
        g_setIDHandlePairAddress = 0;

        g_self = nullptr;
    }
}


