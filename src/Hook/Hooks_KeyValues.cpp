// Hook KeyValues::ReadAsBinary — entry point for KV-tree manipulation.
// Manifest depot patching has been moved to Hooks_Manifest::BuildDepotDependency.

#include "Hooks_KeyValues.h"
#include "HookMacros.h"
#include "dllmain.h"

namespace {

    HOOK_FUNC(ReadAsBinary, bool, KeyValues* root, void* buf, int depth,
              bool textMode, void* symTable) {
        bool ok = oReadAsBinary(root, buf, depth, textMode, symTable);
        return ok;
    }

} // anonymous namespace

namespace Hooks_KeyValues {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(ReadAsBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ReadAsBinary);
        UNHOOK_END();
    }

} // namespace Hooks_KeyValues
