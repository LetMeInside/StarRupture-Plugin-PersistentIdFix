#include "ReflectionDiagnostics.h"
#include "SourceMemory.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "SDK/Chimera_structs.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace PersistentIdFixReflectionDiagnostics
{
    using namespace SDK;
    using namespace PersistentIdFixReflectionWalker;
    using PersistentIdFixSourceMemory::IsReadableRange;

    bool AnalyzeShadowPayload(const FInstancedStruct& payload, ShadowSchemaSummary& summary)
    {
        return PersistentIdFixReflectionWalker::AnalyzeShadowPayload(payload, summary);
    }

    void LogPayloadShape(const FInstancedStruct& payload, bool shadowSupported,
        const ShadowSchemaSummary& shadow)
    {
    LOG_INFO(
        "PersistentIdFix: device payload reflection shadow: "
        "schema=%p size=%d supportedShape=%u "
        "properties=%llu arrays=%llu structs=%llu "
        "integralLeaves=%llu enumLeaves=%llu "
        "unsupported=%llu maxDepth=%llu",
        static_cast<const void*>(
            payload.ScriptStruct),
        payload.ScriptStruct->Size,
        shadowSupported ? 1u : 0u,
        static_cast<unsigned long long>(
            shadow.properties),
        static_cast<unsigned long long>(
            shadow.arrays),
        static_cast<unsigned long long>(
            shadow.structs),
        static_cast<unsigned long long>(
            shadow.integralLeaves),
        static_cast<unsigned long long>(
            shadow.enumLeaves),
        static_cast<unsigned long long>(
            shadow.unsupported),
        static_cast<unsigned long long>(
            shadow.maxDepth));
    }

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
namespace
{
    const UScriptStruct* g_reflectionTestPersistentIdDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestMapMenuDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestPlayerDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestAntennaDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestMassDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestStabilityDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestPidArrayDescriptor = nullptr;
    const UScriptStruct* g_sparseGraphDescriptor = nullptr;
    const UScriptStruct* g_sparseNodeDescriptor = nullptr;
    // Patch 28: diagnostic-only canonical identities; never used for PID certification.
    const UScriptStruct* g_step28Store = nullptr;
    const UScriptStruct* g_step28ItemId = nullptr;
    const UScriptStruct* g_step28Guid = nullptr;
    const UScriptStruct* g_step28Json = nullptr;

    void LogShadowParentFixtureIdentity(
        const UStruct* descriptor, const FProperty* property, std::uint32_t depth)
    {
        LOG_WARN(
            "PersistentIdFix: reflection step28 parent identity: "
            "depth=%u storeReady=%u exactStore=%u mapOffset16=%u "
            "mapStorage80=%u playerReady=%u playerExact=%u "
            "playerOffset=%d",
            depth,
            g_step28Store != nullptr ? 1u : 0u,
            g_step28Store != nullptr &&
                descriptor == g_step28Store ? 1u : 0u,
            property->Offset == 0x10 ? 1u : 0u,
            property->ElementSize == 0x50 ? 1u : 0u,
            g_reflectionTestPlayerDescriptor != nullptr ? 1u : 0u,
            g_reflectionTestPlayerDescriptor != nullptr &&
                descriptor == g_reflectionTestPlayerDescriptor ? 1u : 0u,
            property->Offset);
    }
    void LogShadowMapFixtureDiagnostics(
        const FProperty* property,
        const ShadowNativeMapMetadata& nativeMetadata,
        const ShadowHeapMapValidation& heapMap,
        const ShadowMapSlotValidation& slots,
        const UScriptStruct* persistentIdDescriptor,
        std::uint32_t depth)
    {
        const auto& nativeLayout = nativeMetadata.Layout;
        LOG_WARN(
            "PersistentIdFix: reflection native map metadata: "
            "property=%p descriptorReadable=%u nativeDescriptorSize=%llu "
            "valueOffset=%d hashNextIdOffset=%d hashIndexOffset=%d "
            "setSize=%d sparseAlignment=%d sparseSlotStride=%d "
            "allocatorFlags=0x%02X flagsKnown=%u allocatorSupported=%u "
            "layoutValid=%u metadataValid=%u",
            static_cast<const void*>(property),
            nativeMetadata.DescriptorReadable ? 1u : 0u,
            static_cast<unsigned long long>(kShadowNativeMapDescriptorSize),
            nativeLayout.ValueOffset,
            nativeLayout.HashNextIdOffset,
            nativeLayout.HashIndexOffset,
            nativeLayout.SetSize,
            nativeLayout.SparseAlignment,
            nativeLayout.SparseSlotStride,
            static_cast<unsigned int>(nativeMetadata.AllocatorFlags),
            nativeMetadata.FlagsKnown ? 1u : 0u,
            nativeMetadata.AllocatorSupported ? 1u : 0u,
            nativeMetadata.LayoutValid ? 1u : 0u,
            nativeMetadata.DescriptorReadable &&
                nativeMetadata.FlagsKnown &&
                nativeMetadata.AllocatorSupported &&
                nativeMetadata.LayoutValid ? 1u : 0u);
        LOG_WARN(
            "PersistentIdFix: reflection heap map validation: "
            "property=%p headerReadable=%u headerBytes=%llu "
            "arrayNum=%d arrayMax=%d numFreeIndices=%d firstFreeIndex=%d "
            "liveEntries=%d numBits=%d maxBits=%d hashSize=%d "
            "bitmapMode=%s occupiedBits=%llu headerValid=%u "
            "bitmapValid=%u valid=%u reason=%s",
            static_cast<const void*>(property),
            heapMap.HeaderReadable ? 1u : 0u,
            static_cast<unsigned long long>(sizeof(ShadowHeapMapHeader)),
            heapMap.Header.ArrayNum, heapMap.Header.ArrayMax,
            heapMap.Header.NumFreeIndices, heapMap.Header.FirstFreeIndex,
            heapMap.LiveEntries, heapMap.Header.NumBits,
            heapMap.Header.MaxBits, heapMap.Header.HashSize,
            heapMap.BitmapMode,
            static_cast<unsigned long long>(heapMap.OccupiedBits),
            heapMap.HeaderValid ? 1u : 0u,
            heapMap.BitmapValid ? 1u : 0u,
            heapMap.Valid ? 1u : 0u,
            heapMap.FailureReason);
        const bool mapReadable =
            IsReadableRange(property, sizeof(FMapProperty));
        const auto* mapProperty = mapReadable
            ? static_cast<const FMapProperty*>(property)
            : nullptr;
        const FProperty* key = mapProperty != nullptr
            ? mapProperty->KeyProperty : nullptr;
        const FProperty* value = mapProperty != nullptr
            ? mapProperty->ValueProperty : nullptr;
        const bool keyReadable = key != nullptr &&
            IsReadableRange(key, sizeof(FProperty));
        const bool valueReadable = value != nullptr &&
            IsReadableRange(value, sizeof(FProperty));
        LOG_WARN(
            "PersistentIdFix: reflection map slot validation: "
            "property=%p arrayNum=%d expectedOccupied=%llu scannedSlots=%llu "
            "occupiedSlots=%llu validatedEntries=%llu stride=%d valueOffset=%d "
            "keyBytes=%llu valueBytes=%llu keyAlignment=%llu valueAlignment=%llu "
            "extentsValid=%u alignmentValid=%u allValidated=%u failedSlot=%d reason=%s",
            static_cast<const void*>(property), heapMap.Header.ArrayNum,
            static_cast<unsigned long long>(heapMap.OccupiedBits),
            static_cast<unsigned long long>(slots.ScannedSlots),
            static_cast<unsigned long long>(slots.OccupiedSlots),
            static_cast<unsigned long long>(slots.ValidatedEntries),
            nativeLayout.SparseSlotStride, nativeLayout.ValueOffset,
            static_cast<unsigned long long>(slots.Key.Bytes),
            static_cast<unsigned long long>(slots.Value.Bytes),
            static_cast<unsigned long long>(slots.Key.Alignment),
            static_cast<unsigned long long>(slots.Value.Alignment),
            slots.ExtentsValid ? 1u : 0u, slots.AlignmentValid ? 1u : 0u,
            slots.AllValidated ? 1u : 0u, slots.FailedSlot, slots.FailureReason);
        const unsigned long long keyFlags =
            keyReadable && key->ClassPrivate != nullptr &&
            IsReadableRange(key->ClassPrivate, sizeof(FFieldClass))
                ? static_cast<unsigned long long>(
                    key->ClassPrivate->CastFlags)
                : 0ULL;
        const unsigned long long valueFlags =
            valueReadable && value->ClassPrivate != nullptr &&
            IsReadableRange(value->ClassPrivate, sizeof(FFieldClass))
                ? static_cast<unsigned long long>(
                    value->ClassPrivate->CastFlags)
                : 0ULL;
        LOG_WARN(
            "PersistentIdFix: reflection map key/value metadata: "
            "depth=%u storageSize=%d mapReadable=%u "
            "descriptorSize=%llu keyReadable=%u keySize=%d "
            "keyOffset=%d keyFlags=0x%llX "
            "valueReadable=%u valueSize=%d valueOffset=%d "
            "valueFlags=0x%llX",
            depth,
            property->ElementSize,
            mapReadable ? 1u : 0u,
            static_cast<unsigned long long>(sizeof(FMapProperty)),
            keyReadable ? 1u : 0u,
            keyReadable ? key->ElementSize : -1,
            keyReadable ? key->Offset : -1,
            keyFlags,
            valueReadable ? 1u : 0u,
            valueReadable ? value->ElementSize : -1,
            valueReadable ? value->Offset : -1,
            valueFlags);
        // Metadata only. Do not inspect map storage or entry contents.
        const bool keyStructProperty = keyReadable &&
            HasCastFlag(key, EClassCastFlags::StructProperty) &&
            IsReadableRange(key, sizeof(FStructProperty));
        const bool valueStructProperty = valueReadable &&
            HasCastFlag(value, EClassCastFlags::StructProperty) &&
            IsReadableRange(value, sizeof(FStructProperty));
        const UStruct* keyDescriptor = keyStructProperty
            ? static_cast<const FStructProperty*>(key)->Struct : nullptr;
        const UStruct* valueDescriptor = valueStructProperty
            ? static_cast<const FStructProperty*>(value)->Struct : nullptr;
        const bool keyDescriptorReadable = keyDescriptor != nullptr &&
            IsReadableRange(keyDescriptor, sizeof(UStruct));
        const bool valueDescriptorReadable = valueDescriptor != nullptr &&
            IsReadableRange(valueDescriptor, sizeof(UStruct));
        LOG_WARN(
            "PersistentIdFix: reflection map struct descriptors: "
            "depth=%u keyIsStruct=%u keyDescriptorReadable=%u "
            "keyStructSize=%d keyExactPid=%u "
            "valueIsStruct=%u valueDescriptorReadable=%u "
            "valueStructSize=%d valueExactPid=%u",
            depth,
            keyStructProperty ? 1u : 0u,
            keyDescriptorReadable ? 1u : 0u,
            keyDescriptorReadable ? keyDescriptor->Size : -1,
            keyDescriptorReadable &&
                keyDescriptor == persistentIdDescriptor ? 1u : 0u,
            valueStructProperty ? 1u : 0u,
            valueDescriptorReadable ? 1u : 0u,
            valueDescriptorReadable ? valueDescriptor->Size : -1,
            valueDescriptorReadable &&
                valueDescriptor == persistentIdDescriptor ? 1u : 0u);
        LOG_WARN(
            "PersistentIdFix: reflection step28 map identity: "
            "property=%p keyReady=%u keyExactItemId=%u "
            "valueReady=%u valueExactJson=%u",
            static_cast<const void*>(property),
            g_step28ItemId != nullptr ? 1u : 0u,
            keyDescriptorReadable && g_step28ItemId != nullptr &&
                keyDescriptor == g_step28ItemId ? 1u : 0u,
            g_step28Json != nullptr ? 1u : 0u,
            valueDescriptorReadable && g_step28Json != nullptr &&
                valueDescriptor == g_step28Json ? 1u : 0u);
        // Metadata-only bounded child-property inspection. No map data reads.
        const UStruct* mapDescriptors[2] = {
            keyDescriptorReadable ? keyDescriptor : nullptr,
            valueDescriptorReadable ? valueDescriptor : nullptr
        };
        for (unsigned int side = 0; side < 2u; ++side)
        {
            const UStruct* descriptor = mapDescriptors[side];
            if (descriptor == nullptr || descriptor->Size <= 0)
                continue;

            const FField* child = descriptor->ChildProperties;
            unsigned int inspected = 0;
            while (child != nullptr && inspected < 8u)
            {
                const bool fieldReadable =
                    IsReadableRange(child, sizeof(FField));
                const bool propertyType = fieldReadable &&
                    HasCastFlag(child, EClassCastFlags::Property);
                const auto* nested = propertyType
                    ? reinterpret_cast<const FProperty*>(child)
                    : nullptr;
                const bool propertyReadable = nested != nullptr &&
                    IsReadableRange(nested, sizeof(FProperty));
                unsigned long long castFlags = 0ULL;
                if (fieldReadable && child->ClassPrivate != nullptr &&
                    IsReadableRange(child->ClassPrivate, sizeof(FFieldClass)))
                {
                    castFlags = static_cast<unsigned long long>(
                        child->ClassPrivate->CastFlags);
                }
                LOG_WARN(
                    "PersistentIdFix: reflection map child property: "
                    "side=%u index=%u fieldReadable=%u propertyReadable=%u "
                    "offset=%d size=%d arrayDim=%d castFlags=0x%llX",
                    side, inspected, fieldReadable ? 1u : 0u,
                    propertyReadable ? 1u : 0u,
                    propertyReadable ? nested->Offset : -1,
                    propertyReadable ? nested->ElementSize : -1,
                    propertyReadable ? nested->ArrayDim : -1,
                    castFlags);
                // Descriptor-only nested struct inspection; never read map entries.
                if (propertyReadable &&
                    HasCastFlag(nested, EClassCastFlags::StructProperty))
                {
                    const bool structPropertyReadable =
                        IsReadableRange(nested, sizeof(FStructProperty));
                    const auto* structProp = structPropertyReadable
                        ? static_cast<const FStructProperty*>(nested) : nullptr;
                    const UStruct* nestedDescriptor = structProp != nullptr
                        ? structProp->Struct : nullptr;
                    const bool descriptorReadable = nestedDescriptor != nullptr &&
                        IsReadableRange(nestedDescriptor, sizeof(UStruct));
                    LOG_WARN(
                        "PersistentIdFix: reflection map nested child struct: "
                        "side=%u index=%u structPropertyReadable=%u "
                        "descriptorReadable=%u descriptorSize=%d "
                        "exactPid=%u hasChildren=%u",
                        side, inspected,
                        structPropertyReadable ? 1u : 0u,
                        descriptorReadable ? 1u : 0u,
                        descriptorReadable ? nestedDescriptor->Size : -1,
                        descriptorReadable &&
                            nestedDescriptor == persistentIdDescriptor ? 1u : 0u,
                        descriptorReadable &&
                            nestedDescriptor->ChildProperties != nullptr ? 1u : 0u);
                }
                // Patch 23: descriptor-only inspection of the nested
                // map-key structure (side 0), limited to eight children.
                // Do not read map storage or dereference object values.
                if (side == 0u && propertyReadable &&
                    HasCastFlag(nested, EClassCastFlags::StructProperty) &&
                    IsReadableRange(nested, sizeof(FStructProperty)))
                {
                    const UStruct* innerDescriptor =
                        static_cast<const FStructProperty*>(nested)->Struct;
                    if (innerDescriptor != nullptr &&
                        IsReadableRange(innerDescriptor, sizeof(UStruct)) &&
                        innerDescriptor->Size > 0)
                    {
                        LOG_WARN(
                            "PersistentIdFix: reflection step28 nested identity: "
                            "keyReady=%u keyExactItemId=%u guidReady=%u "
                            "nestedOffset=%d nestedSize=%d nestedArrayDim=%d "
                            "nestedExactGuid=%u",
                            g_step28ItemId != nullptr ? 1u : 0u,
                            keyDescriptorReadable && g_step28ItemId != nullptr &&
                                keyDescriptor == g_step28ItemId ? 1u : 0u,
                            g_step28Guid != nullptr ? 1u : 0u,
                            nested->Offset, nested->ElementSize, nested->ArrayDim,
                            g_step28Guid != nullptr &&
                                innerDescriptor == g_step28Guid ? 1u : 0u);
                        const FField* inner = innerDescriptor->ChildProperties;
                        unsigned int innerIndex = 0u;
                        while (inner != nullptr && innerIndex < 8u)
                        {
                            if (!IsReadableRange(inner, sizeof(FField)))
                            {
                                LOG_WARN(
                                    "PersistentIdFix: reflection map nested key field unreadable: "
                                    "index=%u", innerIndex);
                                break;
                            }
                            const bool innerIsProperty =
                                HasCastFlag(inner, EClassCastFlags::Property);
                            const FProperty* innerProperty = innerIsProperty
                                ? reinterpret_cast<const FProperty*>(inner)
                                : nullptr;
                            const bool innerReadable =
                                innerProperty != nullptr &&
                                IsReadableRange(innerProperty, sizeof(FProperty));
                            const bool classReadable =
                                inner->ClassPrivate != nullptr &&
                                IsReadableRange(inner->ClassPrivate, sizeof(FFieldClass));
                            const unsigned long long innerFlags = classReadable
                                ? static_cast<unsigned long long>(
                                    inner->ClassPrivate->CastFlags)
                                : 0ULL;
                            LOG_WARN(
                                "PersistentIdFix: reflection map nested key property: "
                                "index=%u readable=%u offset=%d size=%d "
                                "arrayDim=%d flags=0x%llX integral=%u "
                                "struct=%u array=%u exactPidStruct=%u",
                                innerIndex,
                                innerReadable ? 1u : 0u,
                                innerReadable ? innerProperty->Offset : -1,
                                innerReadable ? innerProperty->ElementSize : -1,
                                innerReadable ? innerProperty->ArrayDim : -1,
                                innerFlags,
                                innerReadable &&
                                    IsSupportedIntegralProperty(innerProperty) ? 1u : 0u,
                                innerReadable &&
                                    HasCastFlag(innerProperty, EClassCastFlags::StructProperty) ? 1u : 0u,
                                innerReadable &&
                                    HasCastFlag(innerProperty, EClassCastFlags::ArrayProperty) ? 1u : 0u,
                                innerReadable &&
                                    HasCastFlag(innerProperty, EClassCastFlags::StructProperty) &&
                                    IsReadableRange(innerProperty, sizeof(FStructProperty)) &&
                                    static_cast<const FStructProperty*>(innerProperty)->Struct ==
                                        persistentIdDescriptor ? 1u : 0u);

                            if (!innerReadable)
                                break;
                            inner = inner->Next;
                            ++innerIndex;
                        }
                        LOG_WARN(
                            "PersistentIdFix: reflection map nested key scan: "
                            "inspected=%u remaining=%u descriptorSize=%d",
                            innerIndex, inner != nullptr ? 1u : 0u,
                            innerDescriptor->Size);
                    }
                }
                if (!fieldReadable || !propertyReadable)
                    break;
                child = child->Next;
                ++inspected;
            }
            LOG_WARN(
                "PersistentIdFix: reflection map child scan: "
                "side=%u inspected=%u truncated=%u",
                side, inspected, child != nullptr ? 1u : 0u);
        }
    }

    void LogShadowWarning(const char* message)
    {
        LOG_WARN("%s", message);
    }

    const ShadowDiagnosticSink kDiagnosticSink{
        LogShadowWarning, LogShadowParentFixtureIdentity, LogShadowMapFixtureDiagnostics};
}

    const FProperty* FindFirstIntegralArrayProperty(
        const UStruct* descriptor)
    {
        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;

        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
            {
                return nullptr;
            }

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;

            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                {
                    return nullptr;
                }

                if (HasCastFlag(field, EClassCastFlags::ArrayProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FProperty*>(field);

                    if (!IsReadableRange(
                            property,
                            sizeof(FArrayProperty)))
                    {
                        return nullptr;
                    }

                    const auto* arrayProperty =
                        static_cast<const FArrayProperty*>(property);

                    if (arrayProperty->InnerProperty != nullptr &&
                        IsSupportedIntegralProperty(
                            arrayProperty->InnerProperty))
                    {
                        return property;
                    }
                }

                field = field->Next;
            }

            current = current->SuperStruct;
        }

        return nullptr;
    }

    const FProperty* FindIntegralArrayPropertyByOffset(
        const UStruct* descriptor,
        std::int32_t offset)
    {
        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;

        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
            {
                return nullptr;
            }

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;

            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                {
                    return nullptr;
                }

                if (HasCastFlag(field, EClassCastFlags::ArrayProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FProperty*>(field);

                    if (!IsReadableRange(property, sizeof(FArrayProperty)))
                        return nullptr;

                    if (property->Offset == offset)
                    {
                        const auto* arrayProperty =
                            static_cast<const FArrayProperty*>(property);

                        if (arrayProperty->InnerProperty != nullptr &&
                            IsReadableRange(
                                arrayProperty->InnerProperty,
                                sizeof(FProperty)) &&
                            IsSupportedIntegralProperty(
                                arrayProperty->InnerProperty))
                        {
                            return property;
                        }
                    }
                }

                field = field->Next;
            }

            current = current->SuperStruct;
        }

        return nullptr;
    }

    // Diagnostic-only: find a nested reflected struct field by a known
    // typed member offset, without trusting a property name or raw bytes.
    const FStructProperty* FindStructPropertyByOffset(
        const UStruct* descriptor,
        std::int32_t offset,
        const UScriptStruct* expectedStruct)
    {
        if (descriptor == nullptr || expectedStruct == nullptr || offset < 0)
            return nullptr;

        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;
        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
                return nullptr;

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;
            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                    return nullptr;

                if (HasCastFlag(field, EClassCastFlags::StructProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FStructProperty*>(field);
                    if (!IsReadableRange(property, sizeof(FStructProperty)))
                        return nullptr;

                    if (property->Offset == offset &&
                        property->ArrayDim == 1 &&
                        property->Struct == expectedStruct &&
                        expectedStruct->Size > 0 &&
                        property->ElementSize >= expectedStruct->Size)
                        return property;
                }
                field = field->Next;
            }
            current = current->SuperStruct;
        }
        return nullptr;
    }

    const FMapProperty* FindShadowTestMapPropertyByOffset(
        const UStruct* descriptor,
        std::int32_t offset)
    {
        std::uint32_t inheritanceGuard = 0;
        while (descriptor != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(descriptor, sizeof(UStruct)))
                return nullptr;
            const FField* field = descriptor->ChildProperties;
            std::uint64_t chainGuard = 0;
            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                    return nullptr;
                if (HasCastFlag(field, EClassCastFlags::MapProperty))
                {
                    const auto* property = reinterpret_cast<const FMapProperty*>(field);
                    if (!IsReadableRange(property, sizeof(FMapProperty)))
                        return nullptr;
                    if (property->Offset == offset && property->ArrayDim == 1 &&
                        property->ElementSize == sizeof(ShadowHeapMapHeader))
                        return property;
                }
                field = field->Next;
            }
            descriptor = descriptor->SuperStruct;
        }
        return nullptr;
    }

    void RunForcedReflectionShapeTests()
    {
        ShadowSchemaSummary persistentIdSummary{};
        const bool persistentIdSupported =
            g_reflectionTestPersistentIdDescriptor != nullptr &&
            AnalyzeShadowStructShape(
                g_reflectionTestPersistentIdDescriptor,
                0,
                persistentIdSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection test: target=CrMassPersistentEntityID "
            "supportedShape=%u properties=%llu arrays=%llu structs=%llu "
            "integralLeaves=%llu enumLeaves=%llu unsupported=%llu maxDepth=%llu",
            persistentIdSupported ? 1u : 0u,
            static_cast<unsigned long long>(persistentIdSummary.properties),
            static_cast<unsigned long long>(persistentIdSummary.arrays),
            static_cast<unsigned long long>(persistentIdSummary.structs),
            static_cast<unsigned long long>(persistentIdSummary.integralLeaves),
            static_cast<unsigned long long>(persistentIdSummary.enumLeaves),
            static_cast<unsigned long long>(persistentIdSummary.unsupported),
            static_cast<unsigned long long>(persistentIdSummary.maxDepth));

        ShadowSchemaSummary arraySummary{};
        const FProperty* arrayProperty =
            FindFirstIntegralArrayProperty(
                g_reflectionTestMapMenuDescriptor);

        bool arraySupported = false;
        if (arrayProperty != nullptr)
        {
            arraySummary.properties = 1;
            arraySupported =
                AnalyzeShadowPropertyShape(
                    arrayProperty,
                    0,
                    arraySummary);
        }
        else
        {
            arraySummary.supported = false;
            ++arraySummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection test: "
            "target=CrPlayersMapMenuState.firstIntegralArray "
            "propertyFound=%u supportedShape=%u properties=%llu arrays=%llu "
            "structs=%llu integralLeaves=%llu enumLeaves=%llu "
            "unsupported=%llu maxDepth=%llu",
            arrayProperty != nullptr ? 1u : 0u,
            arraySupported ? 1u : 0u,
            static_cast<unsigned long long>(arraySummary.properties),
            static_cast<unsigned long long>(arraySummary.arrays),
            static_cast<unsigned long long>(arraySummary.structs),
            static_cast<unsigned long long>(arraySummary.integralLeaves),
            static_cast<unsigned long long>(arraySummary.enumLeaves),
            static_cast<unsigned long long>(arraySummary.unsupported),
            static_cast<unsigned long long>(arraySummary.maxDepth));
    }

    ShadowValueSummary MakeShadowFixtureSummary()
    {
        ShadowValueSummary summary{};
        summary.OpaqueStructDescriptor = g_step28Json;
        summary.Diagnostics = &kDiagnosticSink;
        // An unavailable descriptor does not block unrelated map traversal.
        // Reflected completeness still never certifies opaque/custom serialization.
        return summary;
    }

    void RunForcedReflectionValueTests(
        const FCrCharacterPlayerBaseSaveDataPerPlayer& player)
    {
        // Diagnostic only. Traverse the entire reflected parent, rather
        // than a single property selected by its typed field offset.
        // This deliberately does not classify encountered integers as PIDs.
        ShadowValueSummary parentSummary = MakeShadowFixtureSummary();
        const bool parentEligible =
            g_reflectionTestPlayerDescriptor != nullptr &&
            IsReadableRange(
                g_reflectionTestPlayerDescriptor,
                sizeof(UScriptStruct)) &&
            g_reflectionTestPlayerDescriptor->Size > 0 &&
            static_cast<std::size_t>(
                g_reflectionTestPlayerDescriptor->Size) <= sizeof(player);

        const bool parentComplete =
            parentEligible &&
            TraverseShadowStructValue(
                g_reflectionTestPlayerDescriptor,
                &player,
                0,
                g_reflectionTestPersistentIdDescriptor,
                parentSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=PlayerSave.wholeParent eligible=%u complete=%u "
            "exactPid=%llu integral=%llu candidates=%llu "
            "arrays=%llu elements=%llu structs=%llu "
            "unsupported=%llu maxDepth=%llu "
            "floorExpected=%u",
            parentEligible ? 1u : 0u,
            parentComplete ? 1u : 0u,
            static_cast<unsigned long long>(
                parentSummary.exactPersistentIds),
            static_cast<unsigned long long>(
                parentSummary.integralValues),
            static_cast<unsigned long long>(
                parentSummary.candidateValues),
            static_cast<unsigned long long>(parentSummary.arrays),
            static_cast<unsigned long long>(parentSummary.arrayElements),
            static_cast<unsigned long long>(parentSummary.structs),
            static_cast<unsigned long long>(parentSummary.unsupported),
            static_cast<unsigned long long>(parentSummary.maxDepth),
            player.FloorPersistentEntityID.ID);

        // Explicit test input only: the generic adapter has no item-store schema
        // dependency. Reach it separately to report the populated-map fixture
        // independently of wholeParent's first unsupported member.
        const auto fixturePlayerBase = reinterpret_cast<std::uintptr_t>(&player);
        const auto fixtureStoreBase =
            reinterpret_cast<std::uintptr_t>(&player.ItemsStoreState);
        const auto fixtureMapAddress = reinterpret_cast<std::uintptr_t>(
            &player.ItemsStoreState.SlotedItemsInstancesSaveData);
        const bool fixtureOffsetsValid =
            fixtureStoreBase >= fixturePlayerBase && fixtureMapAddress >= fixtureStoreBase &&
            fixtureStoreBase - fixturePlayerBase <=
                static_cast<std::uintptr_t>((std::numeric_limits<std::int32_t>::max)()) &&
            fixtureMapAddress - fixtureStoreBase <=
                static_cast<std::uintptr_t>((std::numeric_limits<std::int32_t>::max)());
        const bool fixtureStoreValid = parentEligible && fixtureOffsetsValid &&
            g_step28Store != nullptr && IsReadableRange(g_step28Store, sizeof(UStruct)) &&
            g_step28Store->Size == sizeof(player.ItemsStoreState) &&
            FindStructPropertyByOffset(
                g_reflectionTestPlayerDescriptor,
                static_cast<std::int32_t>(fixtureStoreBase - fixturePlayerBase),
                g_step28Store) != nullptr;
        const auto* fixtureMapProperty = fixtureStoreValid
            ? FindShadowTestMapPropertyByOffset(g_step28Store,
                static_cast<std::int32_t>(fixtureMapAddress - fixtureStoreBase))
            : nullptr;
        ShadowValueSummary fixtureSummary = MakeShadowFixtureSummary();
        const bool fixtureComplete = fixtureMapProperty != nullptr &&
            TraverseShadowPropertyValue(
                fixtureMapProperty,
                &player.ItemsStoreState.SlotedItemsInstancesSaveData,
                0, g_reflectionTestPersistentIdDescriptor, fixtureSummary);
        LOG_INFO(
            "PersistentIdFix: forced reflection map fixture: "
            "target=PlayerSave.ItemsStoreState.SlotedItemsInstancesSaveData "
            "property=%p propertyFound=%u complete=%u",
            fixtureMapProperty, fixtureMapProperty != nullptr ? 1u : 0u,
            fixtureComplete ? 1u : 0u);

        ShadowValueSummary pidSummary = MakeShadowFixtureSummary();

        const bool pidComplete =
            g_reflectionTestPersistentIdDescriptor != nullptr &&
            TraverseShadowStructValue(
                g_reflectionTestPersistentIdDescriptor,
                &player.FloorPersistentEntityID,
                0,
                g_reflectionTestPersistentIdDescriptor,
                pidSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=FloorPersistentEntityID complete=%u "
            "exactPid=%llu integral=%llu candidates=%llu "
            "first=%u last=%u expected=%u zero=%llu sentinel=%llu "
            "unsupported=%llu maxDepth=%llu",
            pidComplete ? 1u : 0u,
            static_cast<unsigned long long>(pidSummary.exactPersistentIds),
            static_cast<unsigned long long>(pidSummary.integralValues),
            static_cast<unsigned long long>(pidSummary.candidateValues),
            pidSummary.firstCandidate,
            pidSummary.lastCandidate,
            player.FloorPersistentEntityID.ID,
            static_cast<unsigned long long>(pidSummary.zeros),
            static_cast<unsigned long long>(pidSummary.invalidSentinels),
            static_cast<unsigned long long>(pidSummary.unsupported),
            static_cast<unsigned long long>(pidSummary.maxDepth));

        // Traverse a real nested struct *property* rather than invoking
        // the PID wrapper's struct walker directly. The typed member is
        // the independent value oracle; no PID is emitted to certification.
        const auto typedPlayerBase =
            reinterpret_cast<std::uintptr_t>(&player);
        const auto typedFloorAddress =
            reinterpret_cast<std::uintptr_t>(&player.FloorPersistentEntityID);
        const bool floorOffsetValid =
            typedFloorAddress >= typedPlayerBase &&
            (typedFloorAddress - typedPlayerBase) <=
                static_cast<std::uintptr_t>(
                    (std::numeric_limits<std::int32_t>::max)());

        const FStructProperty* nestedFloorProperty =
            floorOffsetValid
                ? FindStructPropertyByOffset(
                    g_reflectionTestPlayerDescriptor,
                    static_cast<std::int32_t>(
                        typedFloorAddress - typedPlayerBase),
                    g_reflectionTestPersistentIdDescriptor)
                : nullptr;
        ShadowValueSummary nestedSummary = MakeShadowFixtureSummary();
        bool nestedComplete = false;
        if (nestedFloorProperty != nullptr)
        {
            nestedComplete = TraverseShadowPropertyValue(
                nestedFloorProperty,
                &player.FloorPersistentEntityID,
                0,
                g_reflectionTestPersistentIdDescriptor,
                nestedSummary);
        }
        else
        {
            nestedSummary.complete = false;
            ++nestedSummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=FloorPersistentEntityID.nestedProperty "
            "propertyFound=%u complete=%u exactPid=%llu "
            "candidates=%llu first=%u last=%u expected=%u "
            "unsupported=%llu maxDepth=%llu",
            nestedFloorProperty != nullptr ? 1u : 0u,
            nestedComplete ? 1u : 0u,
            static_cast<unsigned long long>(nestedSummary.exactPersistentIds),
            static_cast<unsigned long long>(nestedSummary.candidateValues),
            nestedSummary.firstCandidate,
            nestedSummary.lastCandidate,
            player.FloorPersistentEntityID.ID,
            static_cast<unsigned long long>(nestedSummary.unsupported),
            static_cast<unsigned long long>(nestedSummary.maxDepth));
        const auto playerBase =
            reinterpret_cast<std::uintptr_t>(&player);
        const auto arrayAddress =
            reinterpret_cast<std::uintptr_t>(&player.DiscoveredBuildings);

        bool offsetValid = arrayAddress >= playerBase;
        const std::uint64_t offsetWide =
            offsetValid
                ? static_cast<std::uint64_t>(arrayAddress - playerBase)
                : 0;

        if (offsetWide >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)()))
        {
            offsetValid = false;
        }

        const FProperty* discoveredProperty =
            offsetValid
                ? FindIntegralArrayPropertyByOffset(
                    g_reflectionTestPlayerDescriptor,
                    static_cast<std::int32_t>(offsetWide))
                : nullptr;

        ShadowValueSummary arraySummary = MakeShadowFixtureSummary();
        bool arrayComplete = false;

        if (discoveredProperty != nullptr)
        {
            const auto* arrayStorage =
                reinterpret_cast<const std::uint8_t*>(&player) +
                discoveredProperty->Offset;

            arrayComplete =
                TraverseShadowPropertyValue(
                    discoveredProperty,
                    arrayStorage,
                    0,
                    g_reflectionTestPersistentIdDescriptor,
                    arraySummary);
        }
        else
        {
            arraySummary.complete = false;
            ++arraySummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=DiscoveredBuildings propertyFound=%u complete=%u "
            "typedNum=%d arrays=%llu elements=%llu integral=%llu "
            "candidates=%llu first=%u last=%u zero=%llu sentinel=%llu "
            "unsupported=%llu maxDepth=%llu",
            discoveredProperty != nullptr ? 1u : 0u,
            arrayComplete ? 1u : 0u,
            player.DiscoveredBuildings.Num(),
            static_cast<unsigned long long>(arraySummary.arrays),
            static_cast<unsigned long long>(arraySummary.arrayElements),
            static_cast<unsigned long long>(arraySummary.integralValues),
            static_cast<unsigned long long>(arraySummary.candidateValues),
            arraySummary.firstCandidate,
            arraySummary.lastCandidate,
            static_cast<unsigned long long>(arraySummary.zeros),
            static_cast<unsigned long long>(arraySummary.invalidSentinels),
            static_cast<unsigned long long>(arraySummary.unsupported),
            static_cast<unsigned long long>(arraySummary.maxDepth));
    }

    namespace
    {
        constexpr std::size_t kAntennaObservationCapacity = 4096;
        constexpr std::int32_t kAntennaOracleMaxSlots = 65536;

        // Independent typed SDK path: no native reflection map helper, bitmap
        // decoder or slot-address validator is used to construct the oracle.
        bool CollectAntennaOracle(const FCrAntennaSaveData& data,
            ShadowExactPidSink& oracle, const char*& reason)
        {
            const auto& values = data.Antennas;
            if (!IsReadableRange(&values, sizeof(values)))
            {
                reason = "oracle-header-unreadable";
                return false;
            }
            const auto live = values.Num();
            const auto allocated = values.NumAllocated();
            const auto capacity = values.Max();
            const auto& flags = values.GetAllocationFlags();
            if (live < 0 || allocated < 0 || capacity < 0 || live > allocated ||
                allocated > capacity || allocated > kAntennaOracleMaxSlots ||
                static_cast<std::size_t>(live) > oracle.Capacity ||
                flags.Num() != allocated || flags.Max() < flags.Num())
            {
                reason = "oracle-count-or-capacity-invalid";
                return false;
            }
            if (allocated == 0)
                return live == 0;
            const auto words = (static_cast<std::size_t>(allocated) + 31u) / 32u;
            if (!values.IsValid() || !IsReadableRange(flags.GetData(),
                    words * sizeof(std::uint32_t)))
            {
                reason = "oracle-bitmap-unreadable";
                return false;
            }
            struct SparseDataHeader
            {
                const void* Data;
                std::int32_t Num;
                std::int32_t Max;
            };
            static_assert(sizeof(SparseDataHeader) == 0x10);
            SparseDataHeader sparse{};
            std::memcpy(&sparse, &values, sizeof(sparse));
            using Map = TMap<FCrMassPersistentEntityID, FCrAntennaParams>;
            using Slot = UC::ContainerImpl::SetElement<Map::ElementType>;
            if (sparse.Num != allocated || sparse.Max != capacity ||
                sparse.Data == nullptr ||
                reinterpret_cast<std::uintptr_t>(sparse.Data) % alignof(Slot) != 0 ||
                static_cast<std::size_t>(allocated) >
                    (std::numeric_limits<std::size_t>::max)() / sizeof(Slot) ||
                !IsReadableRange(sparse.Data,
                    static_cast<std::size_t>(allocated) * sizeof(Slot)))
            {
                reason = "oracle-slots-unreadable-or-invalid";
                return false;
            }
            std::int32_t visited = 0;
            for (auto it = begin(values); it != end(values); ++it)
            {
                if (++visited > live || oracle.Count >= oracle.Capacity)
                {
                    reason = "oracle-iteration-bound-exceeded";
                    return false;
                }
                oracle.Observe(it->Key().ID);
            }
            if (visited != live || oracle.Overflow ||
                values.Num() != live || values.NumAllocated() != allocated ||
                values.Max() != capacity)
            {
                reason = "oracle-incomplete-or-header-changed";
                return false;
            }
            return true;
        }
    }

    void RunAntennaMapValueTest(const UScriptStruct* suppliedDescriptor,
        const FCrAntennaSaveData& data) noexcept
    {
        // A diagnostic exception must never change the successful static read.
        try
        {
            std::array<std::uint32_t, kAntennaObservationCapacity> found{};
            std::array<std::uint32_t, kAntennaObservationCapacity> expected{};
            ShadowExactPidSink discoveries{found.data(), found.size()};
            ShadowExactPidSink oracle{expected.data(), expected.size()};
            const char* oracleReason = "none";
            const bool oracleComplete = CollectAntennaOracle(data, oracle, oracleReason);
            const bool descriptorReady = g_reflectionTestAntennaDescriptor != nullptr &&
                suppliedDescriptor == g_reflectionTestAntennaDescriptor &&
                IsReadableRange(g_reflectionTestAntennaDescriptor, sizeof(UStruct)) &&
                g_reflectionTestAntennaDescriptor->Size == sizeof(data);
            const auto* property = descriptorReady
                ? FindShadowTestMapPropertyByOffset(g_reflectionTestAntennaDescriptor, 0)
                : nullptr;
            const bool exactKey = property != nullptr &&
                HasCastFlag(property->KeyProperty, EClassCastFlags::StructProperty) &&
                IsReadableRange(property->KeyProperty, sizeof(FStructProperty)) &&
                g_reflectionTestPersistentIdDescriptor != nullptr &&
                reinterpret_cast<const FStructProperty*>(property->KeyProperty)->Struct ==
                    g_reflectionTestPersistentIdDescriptor;
            auto summary = MakeShadowFixtureSummary();
            summary.ExactPidSink = &discoveries;
            LOG_INFO("PersistentIdFix: reflection antenna map test begin: "
                "descriptorReady=%u mapFound=%u exactKey=%u oracleComplete=%u "
                "oracleReason=%s capacity=%llu",
                descriptorReady ? 1u : 0u, property != nullptr ? 1u : 0u,
                exactKey ? 1u : 0u, oracleComplete ? 1u : 0u, oracleReason,
                static_cast<unsigned long long>(found.size()));
            const bool traversed = exactKey && TraverseShadowPropertyValue(
                property, &data.Antennas, 0, g_reflectionTestPersistentIdDescriptor, summary);
            const bool reflectedComplete = traversed && summary.complete;
            const bool comparisonComplete = oracleComplete && reflectedComplete &&
                !discoveries.Overflow && discoveries.Count == summary.exactPersistentIds &&
                discoveries.Observed == summary.exactPersistentIds;
            std::sort(found.begin(), found.begin() + discoveries.Count);
            std::sort(expected.begin(), expected.begin() + oracle.Count);
            std::size_t missing = 0, unexpected = 0, i = 0, j = 0;
            constexpr std::size_t sampleLimit = 8;
            std::array<std::uint32_t, sampleLimit> missingSample{}, unexpectedSample{};
            // A sorted merge preserves multiplicity; no set/deduplication occurs.
            while (i < oracle.Count || j < discoveries.Count)
            {
                if (i < oracle.Count && j < discoveries.Count && expected[i] == found[j])
                {
                    ++i;
                    ++j;
                }
                else if (j == discoveries.Count ||
                    (i < oracle.Count && expected[i] < found[j]))
                {
                    if (missing < sampleLimit)
                        missingSample[missing] = expected[i];
                    ++missing;
                    ++i;
                }
                else
                {
                    if (unexpected < sampleLimit)
                        unexpectedSample[unexpected] = found[j];
                    ++unexpected;
                    ++j;
                }
            }
            const auto count = [](const auto& values, std::size_t n, std::uint32_t v)
            {
                return static_cast<unsigned long long>(
                    std::count(values.begin(), values.begin() + n, v));
            };
            LOG_INFO("PersistentIdFix: reflection antenna map test: "
                "oracleComplete=%u comparisonComplete=%u exactMatch=%u "
                "expected=%llu observed=%llu stored=%llu overflow=%u "
                "missing=%llu unexpected=%llu oracleZero=%llu discoveredZero=%llu "
                "oracleSentinel=%llu discoveredSentinel=%llu "
                "oracle283336=%llu discovered283336=%llu exactPid=%llu "
                "integral=%llu opaque=%llu unsupported=%llu reflectedComplete=%u "
                "semanticCertified=0 budgetFailures=%llu depthFailures=%llu "
                "maxDepth=%llu reason=%s oracleReason=%s",
                oracleComplete ? 1u : 0u, comparisonComplete ? 1u : 0u,
                comparisonComplete && missing == 0 && unexpected == 0 ? 1u : 0u,
                static_cast<unsigned long long>(oracle.Count),
                static_cast<unsigned long long>(discoveries.Observed),
                static_cast<unsigned long long>(discoveries.Count), discoveries.Overflow ? 1u : 0u,
                static_cast<unsigned long long>(missing), static_cast<unsigned long long>(unexpected),
                count(expected, oracle.Count, 0), count(found, discoveries.Count, 0),
                count(expected, oracle.Count, UINT32_MAX), count(found, discoveries.Count, UINT32_MAX),
                count(expected, oracle.Count, 283336), count(found, discoveries.Count, 283336),
                static_cast<unsigned long long>(summary.exactPersistentIds),
                static_cast<unsigned long long>(summary.integralValues),
                static_cast<unsigned long long>(summary.opaqueBoundaries),
                static_cast<unsigned long long>(summary.unsupported), reflectedComplete ? 1u : 0u,
                static_cast<unsigned long long>(summary.budgetFailures),
                static_cast<unsigned long long>(summary.depthFailures),
                static_cast<unsigned long long>(summary.maxDepth),
                exactKey ? summary.failureReason : "descriptor-or-key-unavailable", oracleReason);
            for (std::size_t n = 0; n < (std::min)(missing, sampleLimit); ++n)
                LOG_WARN("PersistentIdFix: reflection antenna mismatch sample: "
                    "kind=missing pid=%u conclusive=%u", missingSample[n], comparisonComplete ? 1u : 0u);
            for (std::size_t n = 0; n < (std::min)(unexpected, sampleLimit); ++n)
                LOG_WARN("PersistentIdFix: reflection antenna mismatch sample: "
                    "kind=unexpected pid=%u conclusive=%u", unexpectedSample[n], comparisonComplete ? 1u : 0u);
        }
        catch (...)
        {
            try
            {
                LOG_WARN("PersistentIdFix: reflection antenna map test: "
                    "comparisonComplete=0 semanticCertified=0 reason=diagnostic-exception");
            }
            catch (...) {}
        }
    }

    namespace
    {
        constexpr std::size_t kRampObservationCapacity = 4096;
        constexpr std::int32_t kRampOracleMaxSlots = 65536;
        constexpr std::uint64_t kRampOracleMaxNestedElements = 4096;

        bool RampDescriptorReady(const UScriptStruct* descriptor,
            std::size_t size, std::size_t alignment)
        {
            return descriptor != nullptr && IsReadableRange(descriptor, sizeof(UScriptStruct)) &&
                descriptor->Size == size && descriptor->MinAlignment == alignment;
        }

        bool RampStructPropertyMatches(const FProperty* property,
            const UScriptStruct* descriptor, std::size_t size)
        {
            return property != nullptr &&
                HasCastFlag(property, EClassCastFlags::StructProperty) &&
                IsReadableRange(property, sizeof(FStructProperty)) &&
                property->ArrayDim == 1 && property->ElementSize == size &&
                reinterpret_cast<const FStructProperty*>(property)->Struct == descriptor;
        }

        const FArrayProperty* FindRampValuesProperty(const UStruct* descriptor)
        {
            std::uint32_t inheritanceGuard = 0;
            while (descriptor != nullptr)
            {
                if (++inheritanceGuard > kShadowMaxStructDepth ||
                    !IsReadableRange(descriptor, sizeof(UStruct)))
                    return nullptr;
                const FField* field = descriptor->ChildProperties;
                std::uint64_t chainGuard = 0;
                while (field != nullptr)
                {
                    if (++chainGuard > kShadowMaxProperties ||
                        !IsReadableRange(field, sizeof(FField)))
                        return nullptr;
                    if (HasCastFlag(field, EClassCastFlags::ArrayProperty))
                    {
                        const auto* property = reinterpret_cast<const FArrayProperty*>(field);
                        if (!IsReadableRange(property, sizeof(FArrayProperty)))
                            return nullptr;
                        if (property->Offset == offsetof(FCrMassPersistentEntityIDArray, Values) &&
                            property->ArrayDim == 1 &&
                            property->ElementSize == sizeof(TArray<FCrMassPersistentEntityID>))
                            return property;
                    }
                    field = field->Next;
                }
                descriptor = descriptor->SuperStruct;
            }
            return nullptr;
        }

        // Independent SDK iterator/indexing oracle. No reflected container
        // helper or native reflection map-slot decoder participates here.
        bool CollectRampOracle(const FCrMassSaveData& mass, ShadowExactPidSink& oracle,
            std::uint64_t& mapEntries, std::uint64_t& nestedElements, const char*& reason)
        {
            const auto fail = [&](const char* value) { reason = value; return false; };
            const auto& values = mass.StabilitySubsystemState.RampConnectionData;
            if (!IsReadableRange(&values, sizeof(values)))
                return fail("oracle-header-unreadable");
            const auto live = values.Num();
            const auto allocated = values.NumAllocated();
            const auto capacity = values.Max();
            const auto& flags = values.GetAllocationFlags();
            if (live < 0 || allocated < 0 || capacity < 0 || live > allocated ||
                allocated > capacity || allocated > kRampOracleMaxSlots ||
                static_cast<std::size_t>(live) > oracle.Capacity ||
                flags.Num() != allocated || flags.Max() < flags.Num())
                return fail("oracle-count-or-capacity-invalid");
            if (allocated == 0)
                return live == 0;
            const auto words = (static_cast<std::size_t>(allocated) + 31u) / 32u;
            if (!values.IsValid() || words >
                    (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t) ||
                !IsReadableRange(flags.GetData(), words * sizeof(std::uint32_t)))
                return fail("oracle-bitmap-unreadable");
            struct SparseDataHeader
            {
                const void* Data;
                std::int32_t Num;
                std::int32_t Max;
            };
            static_assert(sizeof(SparseDataHeader) == 0x10);
            SparseDataHeader sparse{};
            std::memcpy(&sparse, &values, sizeof(sparse));
            using Map = TMap<FCrMassPersistentEntityID, FCrMassPersistentEntityIDArray>;
            using Slot = UC::ContainerImpl::SetElement<Map::ElementType>;
            if (sparse.Num != allocated || sparse.Max != capacity || sparse.Data == nullptr ||
                reinterpret_cast<std::uintptr_t>(sparse.Data) % alignof(Slot) != 0 ||
                static_cast<std::size_t>(allocated) >
                    (std::numeric_limits<std::size_t>::max)() / sizeof(Slot) ||
                !IsReadableRange(sparse.Data, static_cast<std::size_t>(allocated) * sizeof(Slot)))
                return fail("oracle-slots-unreadable-or-invalid");
            for (auto it = begin(values); it != end(values); ++it)
            {
                if (mapEntries >= static_cast<std::uint64_t>(live) ||
                    oracle.Count >= oracle.Capacity)
                    return fail("oracle-entry-or-occurrence-limit");
                ++mapEntries;
                oracle.Observe(it->Key().ID);
                const auto& array = it->Value().Values;
                if (!IsReadableRange(&array, sizeof(array)))
                    return fail("oracle-array-header-unreadable");
                const auto count = array.Num();
                const auto maximum = array.Max();
                const auto* data = array.GetDataPtr();
                if (count < 0 || maximum < 0 || count > maximum ||
                    (maximum > 0 && data == nullptr))
                    return fail("oracle-array-count-or-storage-invalid");
                const auto countWide = static_cast<std::size_t>(count);
                if (countWide > kRampOracleMaxNestedElements - nestedElements ||
                    countWide > oracle.Capacity - oracle.Count)
                    return fail("oracle-array-or-occurrence-limit");
                if (count != 0 && (data == nullptr ||
                    reinterpret_cast<std::uintptr_t>(data) % alignof(FCrMassPersistentEntityID) != 0 ||
                    countWide > (std::numeric_limits<std::size_t>::max)() /
                        sizeof(FCrMassPersistentEntityID) ||
                    !IsReadableRange(data, countWide * sizeof(FCrMassPersistentEntityID))))
                    return fail("oracle-array-alignment-or-extent-invalid");
                for (std::int32_t index = 0; index < count; ++index)
                {
                    if (!array.IsValidIndex(index))
                        return fail("oracle-array-index-invalid");
                    oracle.Observe(array[index].ID);
                    ++nestedElements;
                }
                if (array.Num() != count || array.Max() != maximum || array.GetDataPtr() != data)
                    return fail("oracle-array-header-changed");
            }
            if (mapEntries != static_cast<std::uint64_t>(live) || oracle.Overflow ||
                values.Num() != live || values.NumAllocated() != allocated || values.Max() != capacity)
                return fail("oracle-incomplete-or-header-changed");
            return true;
        }
    }

    void RunSparseMapCensus(const UScriptStruct* suppliedDescriptor,
        const FCrMassSaveData& data) noexcept
    {
        static_assert(offsetof(FCrMassSaveData, StabilitySubsystemState) == 0x50);
        static_assert(offsetof(FCrBuildingStabilitySubsystemState, BuildingFoundationData) == 0x150);
        static_assert(offsetof(FCrBuildingStabilitySubsystemState, GraphData) == 0);
        static_assert(offsetof(FCrBuildingGraphData, NodeDatas) == 0x50);
        // Local cumulative preflight work only. No recursive traversal or sinks.
        ShadowMapSlotBudget budget{};
        for (unsigned candidate = 0; candidate != 2; ++candidate)
        {
            const char* name = candidate == 0 ? "BuildingFoundationData" : "NodeDatas";
            try
            {
                const bool rootReady = suppliedDescriptor == g_reflectionTestMassDescriptor &&
                    RampDescriptorReady(g_reflectionTestMassDescriptor, sizeof(data), alignof(FCrMassSaveData)) &&
                    RampDescriptorReady(g_reflectionTestStabilityDescriptor,
                        sizeof(FCrBuildingStabilitySubsystemState), alignof(FCrBuildingStabilitySubsystemState)) &&
                    RampDescriptorReady(g_reflectionTestPersistentIdDescriptor,
                        sizeof(FCrMassPersistentEntityID), alignof(FCrMassPersistentEntityID)) &&
                    IsReadableRange(&data, sizeof(data)) &&
                    reinterpret_cast<std::uintptr_t>(&data) % alignof(FCrMassSaveData) == 0;
                const bool descriptorReady = rootReady && (candidate == 0 ?
                    RampDescriptorReady(g_reflectionTestPidArrayDescriptor,
                        sizeof(FCrMassPersistentEntityIDArray), alignof(FCrMassPersistentEntityIDArray)) :
                    (RampDescriptorReady(g_sparseGraphDescriptor,
                        sizeof(FCrBuildingGraphData), alignof(FCrBuildingGraphData)) &&
                     RampDescriptorReady(g_sparseNodeDescriptor,
                        sizeof(FCrMassEntityStabilityData), alignof(FCrMassEntityStabilityData))));
                const auto* stability = descriptorReady ? FindStructPropertyByOffset(
                    suppliedDescriptor, offsetof(FCrMassSaveData, StabilitySubsystemState),
                    g_reflectionTestStabilityDescriptor) : nullptr;
                bool links = stability != nullptr && stability->ElementSize == sizeof(FCrBuildingStabilitySubsystemState);
                if (links && candidate == 1)
                {
                    const auto* graph = FindStructPropertyByOffset(g_reflectionTestStabilityDescriptor,
                        offsetof(FCrBuildingStabilitySubsystemState, GraphData), g_sparseGraphDescriptor);
                    links = graph != nullptr && graph->ElementSize == sizeof(FCrBuildingGraphData);
                }
                const auto* map = links ? FindShadowTestMapPropertyByOffset(
                    candidate == 0 ? g_reflectionTestStabilityDescriptor : g_sparseGraphDescriptor,
                    candidate == 0 ? offsetof(FCrBuildingStabilitySubsystemState, BuildingFoundationData) :
                        offsetof(FCrBuildingGraphData, NodeDatas)) : nullptr;
                const bool mapFound = map != nullptr && IsReadableRange(map, kShadowNativeMapDescriptorSize);
                bool schemaReady = mapFound &&
                    RampStructPropertyMatches(map->KeyProperty, g_reflectionTestPersistentIdDescriptor,
                        sizeof(FCrMassPersistentEntityID)) &&
                    RampStructPropertyMatches(map->ValueProperty,
                        candidate == 0 ? g_reflectionTestPidArrayDescriptor : g_sparseNodeDescriptor,
                        candidate == 0 ? sizeof(FCrMassPersistentEntityIDArray) : sizeof(FCrMassEntityStabilityData));
                if (schemaReady && candidate == 0)
                {
                    const auto* values = FindRampValuesProperty(g_reflectionTestPidArrayDescriptor);
                    schemaReady = values != nullptr && RampStructPropertyMatches(values->InnerProperty,
                        g_reflectionTestPersistentIdDescriptor, sizeof(FCrMassPersistentEntityID));
                }
                ShadowMapInspection inspection{};
                if (schemaReady)
                {
                    const void* storage = candidate == 0 ?
                        static_cast<const void*>(&data.StabilitySubsystemState.BuildingFoundationData) :
                        static_cast<const void*>(&data.StabilitySubsystemState.GraphData.NodeDatas);
                    inspection = InspectShadowMap(map, storage, budget);
                }
                const auto& heap = inspection.Heap;
                const auto& header = heap.Header;
                const auto freeBits = heap.BitmapValid ?
                    static_cast<std::uint64_t>(header.NumBits) - heap.OccupiedBits : 0;
                const bool holes = inspection.StructuralValid && heap.LiveEntries > 0 &&
                    header.NumFreeIndices > 0 && heap.OccupiedBits == heap.LiveEntries &&
                    freeBits == header.NumFreeIndices;
                const char* reason = !descriptorReady ? "descriptor-unavailable" :
                    !links ? "containing-link-mismatch" : !mapFound ? "map-not-found" :
                    !schemaReady ? "key-value-schema-mismatch" : !inspection.StructuralValid ?
                    inspection.FailureReason : heap.LiveEntries == 0 ? "empty-map" :
                    holes ? "populated-with-free-slots" : "no-free-slots";
                LOG_INFO("PersistentIdFix: reflection sparse map census: "
                    "candidate=%s descriptorReady=%u mapFound=%u allocatorSupported=%u "
                    "structuralValid=%u headerValid=%u bitmapValid=%u slotScanComplete=%u "
                    "arrayNum=%d arrayMax=%d numFreeIndices=%d firstFreeIndex=%d "
                    "liveEntries=%d numBits=%d maxBits=%d bitmapMode=%s "
                    "occupiedBits=%llu bitmapFreeBits=%llu freeListHeadChecked=%u "
                    "freeListChecked=0 holeFixtureSatisfied=%u semanticCertified=0 reason=%s",
                    name, descriptorReady ? 1u : 0u, mapFound ? 1u : 0u,
                    inspection.Metadata.AllocatorSupported ? 1u : 0u,
                    inspection.StructuralValid ? 1u : 0u, heap.HeaderValid ? 1u : 0u,
                    heap.BitmapValid ? 1u : 0u, inspection.Slots.AllValidated ? 1u : 0u,
                    header.ArrayNum, header.ArrayMax, header.NumFreeIndices, header.FirstFreeIndex,
                    heap.LiveEntries, header.NumBits, header.MaxBits, heap.BitmapMode,
                    static_cast<unsigned long long>(heap.OccupiedBits),
                    static_cast<unsigned long long>(freeBits), heap.BitmapValid ? 1u : 0u,
                    holes ? 1u : 0u, reason);
            }
            catch (...)
            {
                LOG_WARN("PersistentIdFix: reflection sparse map census: "
                    "candidate=%s structuralValid=0 freeListChecked=0 "
                    "holeFixtureSatisfied=0 semanticCertified=0 reason=diagnostic-exception", name);
            }
        }
    }

    void RunStabilityRampMapValueTest(const UScriptStruct* suppliedDescriptor,
        const FCrMassSaveData& data) noexcept
    {
        try
        {
            std::array<std::uint32_t, kRampObservationCapacity> found{}, expected{};
            ShadowExactPidSink discoveries{found.data(), found.size()};
            ShadowExactPidSink oracle{expected.data(), expected.size()};
            std::uint64_t mapEntries = 0, nestedElements = 0;
            const bool descriptorReady = suppliedDescriptor == g_reflectionTestMassDescriptor &&
                RampDescriptorReady(g_reflectionTestMassDescriptor, sizeof(data), alignof(FCrMassSaveData)) &&
                RampDescriptorReady(g_reflectionTestStabilityDescriptor,
                    sizeof(FCrBuildingStabilitySubsystemState), alignof(FCrBuildingStabilitySubsystemState)) &&
                RampDescriptorReady(g_reflectionTestPidArrayDescriptor,
                    sizeof(FCrMassPersistentEntityIDArray), alignof(FCrMassPersistentEntityIDArray)) &&
                RampDescriptorReady(g_reflectionTestPersistentIdDescriptor,
                    sizeof(FCrMassPersistentEntityID), alignof(FCrMassPersistentEntityID)) &&
                IsReadableRange(&data, sizeof(data)) &&
                reinterpret_cast<std::uintptr_t>(&data) % alignof(FCrMassSaveData) == 0;
            const auto* stabilityProperty = descriptorReady ? FindStructPropertyByOffset(
                suppliedDescriptor, offsetof(FCrMassSaveData, StabilitySubsystemState),
                g_reflectionTestStabilityDescriptor) : nullptr;
            const bool rootLink = stabilityProperty != nullptr &&
                stabilityProperty->ElementSize == sizeof(FCrBuildingStabilitySubsystemState);
            const auto* property = rootLink ? FindShadowTestMapPropertyByOffset(
                g_reflectionTestStabilityDescriptor,
                offsetof(FCrBuildingStabilitySubsystemState, RampConnectionData)) : nullptr;
            const bool mapFound = property != nullptr &&
                IsReadableRange(property, kShadowNativeMapDescriptorSize);
            const bool exactKey = mapFound && RampStructPropertyMatches(property->KeyProperty,
                g_reflectionTestPersistentIdDescriptor, sizeof(FCrMassPersistentEntityID));
            const bool exactValue = mapFound && RampStructPropertyMatches(property->ValueProperty,
                g_reflectionTestPidArrayDescriptor, sizeof(FCrMassPersistentEntityIDArray));
            const auto* arrayProperty = exactValue ?
                FindRampValuesProperty(g_reflectionTestPidArrayDescriptor) : nullptr;
            const bool exactArrayInner = arrayProperty != nullptr &&
                RampStructPropertyMatches(arrayProperty->InnerProperty,
                    g_reflectionTestPersistentIdDescriptor, sizeof(FCrMassPersistentEntityID));
            const bool schemaReady = descriptorReady && rootLink && mapFound &&
                exactKey && exactValue && exactArrayInner;
            const char* oracleReason = schemaReady ? "none" : "schema-not-validated";
            const bool oracleComplete = schemaReady &&
                CollectRampOracle(data, oracle, mapEntries, nestedElements, oracleReason);
            auto summary = MakeShadowFixtureSummary();
            summary.ExactPidSink = &discoveries;
            LOG_INFO("PersistentIdFix: reflection stability ramp map test begin: "
                "root=%p destination=%p descriptorReady=%u rootLink=%u mapFound=%u "
                "exactKey=%u exactValue=%u exactArrayInner=%u oracleComplete=%u "
                "mapEntries=%llu nestedElements=%llu oracleReason=%s",
                static_cast<const void*>(suppliedDescriptor), static_cast<const void*>(&data),
                descriptorReady ? 1u : 0u, rootLink ? 1u : 0u, mapFound ? 1u : 0u,
                exactKey ? 1u : 0u, exactValue ? 1u : 0u, exactArrayInner ? 1u : 0u,
                oracleComplete ? 1u : 0u, static_cast<unsigned long long>(mapEntries),
                static_cast<unsigned long long>(nestedElements), oracleReason);
            // The complete independent typed preflight (including every array's
            // alignment) must succeed before any reflected fixture visitation.
            const bool traversed = oracleComplete && TraverseShadowPropertyValue(property,
                &data.StabilitySubsystemState.RampConnectionData, 0,
                g_reflectionTestPersistentIdDescriptor, summary);
            const bool reflectedComplete = traversed && summary.complete;
            const bool comparisonComplete = oracleComplete && reflectedComplete &&
                !discoveries.Overflow && discoveries.Count == discoveries.Observed &&
                discoveries.Observed == summary.exactPersistentIds;
            std::sort(found.begin(), found.begin() + discoveries.Count);
            std::sort(expected.begin(), expected.begin() + oracle.Count);
            std::size_t missing = 0, unexpected = 0, i = 0, j = 0;
            constexpr std::size_t sampleLimit = 8;
            std::array<std::uint32_t, sampleLimit> missingSample{}, unexpectedSample{};
            while (i < oracle.Count || j < discoveries.Count)
            {
                if (i < oracle.Count && j < discoveries.Count && expected[i] == found[j])
                {
                    ++i;
                    ++j;
                }
                else if (j == discoveries.Count || (i < oracle.Count && expected[i] < found[j]))
                {
                    if (missing < sampleLimit)
                        missingSample[missing] = expected[i];
                    ++missing;
                    ++i;
                }
                else
                {
                    if (unexpected < sampleLimit)
                        unexpectedSample[unexpected] = found[j];
                    ++unexpected;
                    ++j;
                }
            }
            const auto count = [](const auto& values, std::size_t n, std::uint32_t value)
            {
                return static_cast<unsigned long long>(
                    std::count(values.begin(), values.begin() + n, value));
            };
            const char* reason = !schemaReady ? "descriptor-or-property-schema" :
                !oracleComplete ? "oracle-preflight-incomplete" :
                !reflectedComplete ? summary.failureReason :
                discoveries.Overflow ? "discovery-sink-overflow" :
                !comparisonComplete ? "observation-count-mismatch" :
                missing != 0 || unexpected != 0 ? "multiset-mismatch" : "none";
            LOG_INFO("PersistentIdFix: reflection stability ramp map test: "
                "descriptorReady=%u rootLink=%u mapFound=%u exactKey=%u exactValue=%u "
                "exactArrayInner=%u oracleComplete=%u comparisonComplete=%u exactMatch=%u "
                "mapPopulated=%u mapEntries=%llu nestedElements=%llu expected=%llu "
                "observed=%llu stored=%llu overflow=%u missing=%llu unexpected=%llu "
                "oracleZero=%llu discoveredZero=%llu oracleSentinel=%llu discoveredSentinel=%llu "
                "exactPid=%llu reflectedComplete=%u semanticCertified=0 "
                "arrays=%llu arrayElements=%llu unsupported=%llu opaque=%llu "
                "budgetFailures=%llu depthFailures=%llu maxDepth=%llu reason=%s oracleReason=%s",
                descriptorReady ? 1u : 0u, rootLink ? 1u : 0u, mapFound ? 1u : 0u,
                exactKey ? 1u : 0u, exactValue ? 1u : 0u, exactArrayInner ? 1u : 0u,
                oracleComplete ? 1u : 0u, comparisonComplete ? 1u : 0u,
                comparisonComplete && missing == 0 && unexpected == 0 ? 1u : 0u,
                mapEntries != 0 ? 1u : 0u, static_cast<unsigned long long>(mapEntries),
                static_cast<unsigned long long>(nestedElements), static_cast<unsigned long long>(oracle.Count),
                static_cast<unsigned long long>(discoveries.Observed),
                static_cast<unsigned long long>(discoveries.Count), discoveries.Overflow ? 1u : 0u,
                static_cast<unsigned long long>(missing), static_cast<unsigned long long>(unexpected),
                count(expected, oracle.Count, 0), count(found, discoveries.Count, 0),
                count(expected, oracle.Count, UINT32_MAX), count(found, discoveries.Count, UINT32_MAX),
                static_cast<unsigned long long>(summary.exactPersistentIds), reflectedComplete ? 1u : 0u,
                static_cast<unsigned long long>(summary.arrays), static_cast<unsigned long long>(summary.arrayElements),
                static_cast<unsigned long long>(summary.unsupported), static_cast<unsigned long long>(summary.opaqueBoundaries),
                static_cast<unsigned long long>(summary.budgetFailures), static_cast<unsigned long long>(summary.depthFailures),
                static_cast<unsigned long long>(summary.maxDepth), reason, oracleReason);
            for (std::size_t n = 0; n < (std::min)(missing, sampleLimit); ++n)
                LOG_WARN("PersistentIdFix: reflection stability ramp mismatch sample: "
                    "kind=missing pid=%u conclusive=%u", missingSample[n], comparisonComplete ? 1u : 0u);
            for (std::size_t n = 0; n < (std::min)(unexpected, sampleLimit); ++n)
                LOG_WARN("PersistentIdFix: reflection stability ramp mismatch sample: "
                    "kind=unexpected pid=%u conclusive=%u", unexpectedSample[n], comparisonComplete ? 1u : 0u);
        }
        catch (...)
        {
            try
            {
                LOG_WARN("PersistentIdFix: reflection stability ramp map test: "
                    "comparisonComplete=0 exactMatch=0 semanticCertified=0 reason=diagnostic-exception");
            }
            catch (...) {}
        }
    }

    void InitializeDescriptorRegistry(Resolver findSafe, UClass* scriptStructClass)
    {
        const auto resolveTestDescriptor =
            [&](const wchar_t* path) -> const UScriptStruct*
            {
                UObject* object =
                    findSafe(scriptStructClass, nullptr, path, true);

                return object != nullptr
                    ? reinterpret_cast<const UScriptStruct*>(object)
                    : nullptr;
            };

        g_reflectionTestPersistentIdDescriptor =
            resolveTestDescriptor(
                L"/Script/ChimeraMassCommon.CrMassPersistentEntityID");

        g_reflectionTestMapMenuDescriptor =
            resolveTestDescriptor(
                L"/Script/Chimera.CrPlayersMapMenuState");

        g_reflectionTestPlayerDescriptor =
            resolveTestDescriptor(
                L"/Script/Chimera.CrCharacterPlayerBaseSaveDataPerPlayer");

        // Diagnostic-only lookups; missing or invalid descriptors remain unavailable.
        const auto resolveStep28 = [&](const wchar_t* path, int expectedSize)
            -> const UScriptStruct*
        {
            const UScriptStruct* result = resolveTestDescriptor(path);
            if (result == nullptr ||
                !IsReadableRange(result, sizeof(UScriptStruct)) ||
                result->Size != expectedSize)
                return nullptr;
            return result;
        };
        g_reflectionTestMassDescriptor = resolveStep28(
            L"/Script/Chimera.CrMassSaveData", sizeof(FCrMassSaveData));
        g_reflectionTestStabilityDescriptor = resolveStep28(
            L"/Script/Chimera.CrBuildingStabilitySubsystemState",
            sizeof(FCrBuildingStabilitySubsystemState));
        g_reflectionTestPidArrayDescriptor = resolveStep28(
            L"/Script/ChimeraMassCommon.CrMassPersistentEntityIDArray",
            sizeof(FCrMassPersistentEntityIDArray));
        g_sparseGraphDescriptor = resolveStep28(
            L"/Script/Chimera.CrBuildingGraphData", sizeof(FCrBuildingGraphData));
        g_sparseNodeDescriptor = resolveStep28(
            L"/Script/Chimera.CrMassEntityStabilityData", sizeof(FCrMassEntityStabilityData));
        g_reflectionTestAntennaDescriptor = resolveStep28(
            L"/Script/Chimera.CrAntennaSaveData", sizeof(FCrAntennaSaveData));
        g_step28Store = resolveStep28(
            L"/Script/AuItems.AuItemsStoreComponentState", 0xC0);
        g_step28ItemId = resolveStep28(
            L"/Script/AuItems.AuItemId", 0x10);
        g_step28Guid = resolveStep28(
            L"/Script/CoreUObject.Guid", 0x10);
        g_step28Json = resolveStep28(
            L"/Script/JsonUtilities.JsonObjectWrapper", 0x20);
        LOG_WARN(
            "PersistentIdFix: reflection step28 descriptor identity: "
            "storeReady=%u itemIdReady=%u guidReady=%u jsonReady=%u",
            g_step28Store != nullptr ? 1u : 0u,
            g_step28ItemId != nullptr ? 1u : 0u,
            g_step28Guid != nullptr ? 1u : 0u,
            g_step28Json != nullptr ? 1u : 0u);

        LOG_INFO(
            "PersistentIdFix: reflection test mode descriptor lookup: "
            "persistentId=%p mapMenu=%p player=%p",
            static_cast<const void*>(
                g_reflectionTestPersistentIdDescriptor),
            static_cast<const void*>(
                g_reflectionTestMapMenuDescriptor),
            static_cast<const void*>(
                g_reflectionTestPlayerDescriptor));
    }

    void ResetDescriptorRegistry()
    {
        g_reflectionTestPersistentIdDescriptor = nullptr;
        g_reflectionTestMapMenuDescriptor = nullptr;
        g_reflectionTestPlayerDescriptor = nullptr;
        g_reflectionTestAntennaDescriptor = nullptr;
        g_reflectionTestMassDescriptor = nullptr;
        g_reflectionTestStabilityDescriptor = nullptr;
        g_reflectionTestPidArrayDescriptor = nullptr;
        g_sparseGraphDescriptor = nullptr;
        g_sparseNodeDescriptor = nullptr;
        g_step28Store = nullptr;
        g_step28ItemId = nullptr;
        g_step28Guid = nullptr;
        g_step28Json = nullptr;
    }
#endif
}
