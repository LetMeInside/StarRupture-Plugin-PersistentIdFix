#include "Native/Fingerprints.h"

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

namespace PersistentIdFixFingerprints
{
    bool Resolve(
        IPluginSelf* self,
        IPluginHookScanner* scanner,
        ResolvedAddresses& addresses)
    {
        addresses = {};

        if (self == nullptr || scanner == nullptr)
            return false;

        uintptr_t getOrAddIDForHandleAddress = 0;
        uintptr_t setIDHandlePairAddress = 0;
        uintptr_t getSaveDataAddress = 0;
        uintptr_t onPreLoadMapAddress = 0;

        uintptr_t getOrAddFingerprintMapAddress = 0;
        uintptr_t getOrAddFingerprintBucketAddress = 0;
        uintptr_t getOrAddFingerprintElementAddress = 0;
        uintptr_t getOrAddFingerprintMaxIDAddress = 0;

        PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;

        // UCrSaveSubsystem::GetSaveData:
        //
        // Audited Hotfix 0.3.5 entry sequence, shared by Client and Server.
        //
        //   48 89 6C 24 10      mov [rsp+10h],rbp
        //   48 89 74 24 18      mov [rsp+18h],rsi
        //   57                  push rdi
        //   41 56               push r14
        //   41 57               push r15
        //   48 83 EC 50         sub rsp,50h
        //   8B 42 08            mov eax,[rdx+08h]
        //   48 8D 79 30         lea rdi,[rcx+30h]
        //   49 8B E9            mov rbp,r9
        //   4D 8B F0            mov r14,r8
        //   48 8B F2            mov rsi,rdx
        //
        // The function consumes a by-value FString section name, a UStruct*
        // describing the destination type, and an initialized destination.
        // PersistentIdFix observes successful native reads before the caller
        // resumes; Step 2 installs only the passive hook infrastructure.
        req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName =
            "PersistentIdFix::GetSaveData";
        req.pattern =
            "48 89 6C 24 10 "
            "48 89 74 24 18 "
            "57 "
            "41 56 "
            "41 57 "
            "48 83 EC 50 "
            "8B 42 08 "
            "48 8D 79 30 "
            "49 8B E9 "
            "4D 8B F0 "
            "48 8B F2";

        req.kind =
            PLUGIN_SCAN_FUNCTION_START;

        getSaveDataAddress =
            scanner->Resolve(self, &req);

        if (getSaveDataAddress == 0)
            return false;

        // UCrSaveSubsystem::OnPreLoadMap:
        //
        // Audited Hotfix 0.3.5 entry sequence, shared by Client
        // and Server. The compiler emits a redundant 0x40 REX prefix
        // on push rbx, so include the complete two-byte instruction
        // rather than matching from its second byte.
        //
        //   40 53                    push rbx
        //   48 83 EC 20              sub rsp,20h
        //   48 8B D9                 mov rbx,rcx
        //   48 81 C1 78 01 00 00     add rcx,178h
        //   E8 ?? ?? ?? ??           call OnPreSaveLoaded broadcast helper
        //   48 8B 93 10 01 00 00     mov rdx,[rbx+110h]
        //   48 85 D2                 test rdx,rdx
        //   74 0C                    je cleanup-complete
        //
        // +0x178 is UCrSaveSubsystem::OnPreSaveLoaded and +0x110 is
        // the pending load-map handle. The fixed structural bytes
        // through the +0x110 read/test distinguish this function from
        // the unrelated prologue collision observed in the Client.
        req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName =
            "PersistentIdFix::OnPreLoadMap";
        req.pattern =
            "40 53 "
            "48 83 EC 20 "
            "48 8B D9 "
            "48 81 C1 78 01 00 00 "
            "E8 ?? ?? ?? ?? "
            "48 8B 93 10 01 00 00 "
            "48 85 D2 "
            "74 0C";

        req.kind =
            PLUGIN_SCAN_FUNCTION_START;

        onPreLoadMapAddress =
            scanner->Resolve(self, &req);

        if (onPreLoadMapAddress == 0)
            return false;

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

        setIDHandlePairAddress =
            scanner->Resolve(self, &req);

        if (setIDHandlePairAddress == 0)
            return false;

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

        getOrAddIDForHandleAddress =
            scanner->Resolve(self, &req);

        if (getOrAddIDForHandleAddress == 0)
            return false;

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

        getOrAddFingerprintMapAddress =
            scanner->Resolve(self, &req);

        if (getOrAddFingerprintMapAddress == 0)
            return false;

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

        getOrAddFingerprintBucketAddress =
            scanner->Resolve(self, &req);

        if (getOrAddFingerprintBucketAddress == 0)
            return false;

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

        getOrAddFingerprintElementAddress =
            scanner->Resolve(self, &req);

        if (getOrAddFingerprintElementAddress == 0)
            return false;

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

        getOrAddFingerprintMaxIDAddress =
            scanner->Resolve(self, &req);

        if (getOrAddIDForHandleAddress == 0 ||
            getOrAddFingerprintMapAddress !=
            getOrAddIDForHandleAddress ||
            getOrAddFingerprintBucketAddress !=
            getOrAddIDForHandleAddress ||
            getOrAddFingerprintElementAddress !=
            getOrAddIDForHandleAddress ||
            getOrAddFingerprintMaxIDAddress !=
            getOrAddIDForHandleAddress)
        {
            scanner->ReportFailure(
                self,
                "PersistentIdFix::GetOrAddIDForHandle",
                "Native fingerprints did not resolve to the same function");

            return false;
        }

        addresses.getOrAddIDForHandle = getOrAddIDForHandleAddress;
        addresses.setIDHandlePair = setIDHandlePairAddress;
        addresses.getSaveData = getSaveDataAddress;
        addresses.onPreLoadMap = onPreLoadMapAddress;

        return true;
    }
}
