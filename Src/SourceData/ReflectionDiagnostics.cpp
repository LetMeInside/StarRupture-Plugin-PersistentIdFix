#include "ReflectionDiagnostics.h"
#include "SourceMemory.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "SDK/Chimera_structs.hpp"
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
        g_step28Store = nullptr;
        g_step28ItemId = nullptr;
        g_step28Guid = nullptr;
        g_step28Json = nullptr;
    }
#endif
}
