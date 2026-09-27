#pragma once

#include <cstdint>
#include "ChimeraMassCommon_classes.hpp"

namespace
{
    /*
    mov r10,qword ptr [rbx]       ; TSet Elements.Data
    ...
    mov ecx,dword ptr [rbx+48h]   ; HashSize
    ...
    mov rdx,qword ptr [rbx+40h]   ; secondary hash storage
    lea rax,[rbx+38h]             ; inline hash storage
    ...
    movsxd rax,r9d
    shl rax,5                     ; element index * 0x20
    ...
    cmp dword ptr [rax+r10],r8d   ; handle.Index
    ...
    cmp dword ptr [rcx+4],edx     ; handle.SerialNumber
    ...
    mov r9d,dword ptr [rcx+18h]   ; HashNextId
    */
    constexpr size_t HandleIDMapElementsDataOffset = 0x00;
    constexpr size_t HandleIDMapElementsNumOffset = 0x08;

    constexpr size_t HandleIDMapHashInlineOffset = 0x38;
    constexpr size_t HandleIDMapHashSecondaryOffset = 0x40;
    constexpr size_t HandleIDMapHashSizeOffset = 0x48;

    constexpr size_t SetElementHandleOffset = 0x00;
    constexpr size_t SetElementPersistentIdOffset = 0x08;
    constexpr size_t SetElementHashNextIdOffset = 0x18;
    constexpr size_t SetElementStride = 0x20;

    constexpr int32_t InvalidHashIndex = -1;

    // for debugging:
    static bool g_loggedFirstPersistentIdLookupMiss = false;
    static uint32_t g_persistentIdLookupMissCount = 0;
 

    static uint32_t HashMassEntityHandle(
        const SDK::FMassEntityHandle& handle)
    {
        uint32_t a = handle.SerialNumber;
        uint32_t b = handle.Index;
        uint32_t c = 0x9E3779B9u;
        uint32_t t;

        c -= a;
        b -= a;

        t = a >> 13;
        b ^= t;

        c -= b;

        t = b << 8;
        c ^= t;

        a -= c;

        t = c >> 13;
        a -= b;
        a ^= t;

        b -= c;
        b -= a;

        t = a >> 12;
        b ^= t;

        c -= b;

        t = b << 16;
        c -= a;
        c ^= t;

        a -= c;

        t = c >> 5;
        a -= b;
        a ^= t;

        b -= c;
        b -= a;

        t = a >> 3;
        b ^= t;

        c -= b;

        t = b << 10;
        c -= a;
        c ^= t;

        a -= c;

        t = c >> 15;
        a -= b;

        a ^= t;

        return a;
    }

    static const SDK::FCrMassPersistentEntityID* FindPersistentIdByHandle(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        const SDK::FMassEntityHandle& handle)
    {
    #ifdef TRACE_NATIVE_LOOKUP
        static bool loggedFirstLookup = false;
    #endif

        if (subsystem == nullptr)
            return nullptr;

        const uint8_t* map =
            reinterpret_cast<const uint8_t*>(&subsystem->HandleIDMap);

        auto readPtr =
            [](const uint8_t* address) -> const uint8_t*
            {
                return *reinterpret_cast<const uint8_t* const*>(address);
            };

        const uint8_t* elementsData =
            readPtr(map + HandleIDMapElementsDataOffset);

        const int32_t elementsNum =
            *reinterpret_cast<const int32_t*>(
                map + HandleIDMapElementsNumOffset);

        const uint8_t* hashSecondary =
            readPtr(map + HandleIDMapHashSecondaryOffset);

        const int32_t hashSize =
            *reinterpret_cast<const int32_t*>(
                map + HandleIDMapHashSizeOffset);

        if (elementsData == nullptr ||
            elementsNum <= 0 ||
            hashSize <= 0)
        {
    #ifdef TRACE_NATIVE_LOOKUP
            if (!loggedFirstLookup)
            {
                loggedFirstLookup = true;

                LOG_ERROR(
                    "PersistentIdFix: FIRST lookup diagnostic: "
                    "invalid map state; "
                    "handle=(%u,%u) "
                    "elementsData=%p "
                    "elementsNum=%d "
                    "hashSecondary=%p "
                    "hashSize=%d",
                    handle.Index,
                    handle.SerialNumber,
                    elementsData,
                    elementsNum,
                    hashSecondary,
                    hashSize);
            }
    #endif

            return nullptr;
        }

        const uint8_t* hashData =
            hashSecondary != nullptr
            ? hashSecondary
            : map + HandleIDMapHashInlineOffset;

        const uint32_t hash =
            HashMassEntityHandle(handle);

        const uint32_t bucket =
            hash & static_cast<uint32_t>(hashSize - 1);

        const int32_t bucketIndex =
            *reinterpret_cast<const int32_t*>(
                hashData + bucket * sizeof(int32_t));

        int32_t elementIndex = bucketIndex;

        while (elementIndex != InvalidHashIndex)
        {
            if (elementIndex < 0 ||
                elementIndex >= elementsNum)
            {
    #ifdef TRACE_NATIVE_LOOKUP
                if (!loggedFirstLookup)
                {
                    loggedFirstLookup = true;

                    LOG_ERROR(
                        "PersistentIdFix: FIRST lookup miss: "
                        "invalid chain index; "
                        "handle=(%u,%u) "
                        "hash=%u "
                        "hashSize=%d "
                        "bucket=%u "
                        "bucketIndex=%d "
                        "elementIndex=%d "
                        "elementsData=%p "
                        "elementsNum=%d "
                        "hashData=%p "
                        "hashSecondary=%p",
                        handle.Index,
                        handle.SerialNumber,
                        hash,
                        hashSize,
                        bucket,
                        bucketIndex,
                        elementIndex,
                        elementsData,
                        elementsNum,
                        hashData,
                        hashSecondary);
                }
    #endif

                return nullptr;
            }

            const uint8_t* element =
                elementsData +
                static_cast<size_t>(elementIndex) *
                SetElementStride;

            const auto* elementHandle =
                reinterpret_cast<const SDK::FMassEntityHandle*>(
                    element + SetElementHandleOffset);

            const uint32_t persistentId =
                *reinterpret_cast<const uint32_t*>(
                    element + SetElementPersistentIdOffset);

            const int32_t nextElementIndex =
                *reinterpret_cast<const int32_t*>(
                    element + SetElementHashNextIdOffset);

            if (elementHandle->Index == handle.Index &&
                elementHandle->SerialNumber == handle.SerialNumber)
            {
                return reinterpret_cast<const SDK::FCrMassPersistentEntityID*>(
                    element + SetElementPersistentIdOffset);
            }

    #ifdef TRACE_NATIVE_LOOKUP
            /*
             * Log every chain entry for the first lookup that eventually
             * misses. Normal non-matching chain entries are not themselves
             * considered errors; they are only diagnostic information.
             */
            if (!loggedFirstLookup)
            {
                LOG_ERROR(
                    "PersistentIdFix: FIRST lookup chain: "
                    "handle=(%u,%u) "
                    "hash=%u "
                    "hashSize=%d "
                    "bucket=%u "
                    "bucketIndex=%d "
                    "elementIndex=%d "
                    "elementHandle=(%u,%u) "
                    "persistentId=%u "
                    "next=%d",
                    handle.Index,
                    handle.SerialNumber,
                    hash,
                    hashSize,
                    bucket,
                    bucketIndex,
                    elementIndex,
                    elementHandle->Index,
                    elementHandle->SerialNumber,
                    persistentId,
                    nextElementIndex);
            }
    #endif

            elementIndex = nextElementIndex;
        }

    #ifdef TRACE_NATIVE_LOOKUP
        if (!loggedFirstLookup)
        {
            loggedFirstLookup = true;

            LOG_ERROR(
                "PersistentIdFix: FIRST lookup miss: "
                "chain ended; "
                "handle=(%u,%u) "
                "hash=%u "
                "hashSize=%d "
                "bucket=%u "
                "bucketIndex=%d "
                "elementsData=%p "
                "elementsNum=%d "
                "hashData=%p "
                "hashSecondary=%p",
                handle.Index,
                handle.SerialNumber,
                hash,
                hashSize,
                bucket,
                bucketIndex,
                elementsData,
                elementsNum,
                hashData,
                hashSecondary);
        }
    #endif

        return nullptr;
    }


}