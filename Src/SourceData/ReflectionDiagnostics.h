#pragma once

#include "ReflectionWalker.h"

namespace SDK { struct FCrCharacterPlayerBaseSaveDataPerPlayer; }
namespace PersistentIdFixReflectionDiagnostics
{
    using ShadowSchemaSummary = PersistentIdFixReflectionWalker::ShadowSchemaSummary;
    bool AnalyzeShadowPayload(const SDK::FInstancedStruct&, ShadowSchemaSummary&);
    void LogPayloadShape(const SDK::FInstancedStruct&, bool supported,
        const ShadowSchemaSummary&);
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    using Resolver = SDK::UObject* (*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
    void InitializeDescriptorRegistry(Resolver findSafe, SDK::UClass* scriptStructClass);
    void ResetDescriptorRegistry();
    void RunForcedReflectionShapeTests();
    void RunForcedReflectionValueTests(const SDK::FCrCharacterPlayerBaseSaveDataPerPlayer&);
#endif
}
