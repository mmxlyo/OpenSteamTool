#include "include/Trap.h"

#include "include/Log.h"

#include <windows.h>
#include <atomic>

namespace OSTPlatform::Trap {
namespace {

std::atomic<Handler> g_handler{nullptr};
std::atomic<HandlerHandle> g_handle{nullptr};

const char* ToString(ExceptionKind kind) {
    switch (kind) {
    case ExceptionKind::Breakpoint: return "Breakpoint";
    case ExceptionKind::SingleStep: return "SingleStep";
    case ExceptionKind::Other: return "Other";
    }
    return "Unknown";
}

ExceptionKind FromWindowsExceptionCode(DWORD code) {
    switch (code) {
    case EXCEPTION_BREAKPOINT: return ExceptionKind::Breakpoint;
    case EXCEPTION_SINGLE_STEP: return ExceptionKind::SingleStep;
    default: return ExceptionKind::Other;
    }
}

LONG CALLBACK VehThunk(PEXCEPTION_POINTERS exceptionInfo) {
    Handler handler = g_handler.load(std::memory_order_acquire);
    if (!handler) {
        OSTP_LOG_TRACE("VehThunk: no handler installed");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!exceptionInfo || !exceptionInfo->ExceptionRecord || !exceptionInfo->ContextRecord) {
        OSTP_LOG_DEBUG("VehThunk: incomplete exception context (info={:#x}, record={:#x}, context={:#x})",
                       reinterpret_cast<uintptr_t>(exceptionInfo),
                       reinterpret_cast<uintptr_t>(exceptionInfo ? exceptionInfo->ExceptionRecord : nullptr),
                       reinterpret_cast<uintptr_t>(exceptionInfo ? exceptionInfo->ContextRecord : nullptr));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Context context(exceptionInfo->ContextRecord);
    const DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    const ExceptionKind kind = FromWindowsExceptionCode(code);
    OSTP_LOG_TRACE("VehThunk: exception code=0x{:08X} kind={} ip={:#x}",
                   code, ToString(kind), context.InstructionPointer());

    const bool handled = handler(
        kind,
        context);
    OSTP_LOG_TRACE("VehThunk: {} at ip={:#x}", handled ? "handled" : "continued", context.InstructionPointer());
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

PCONTEXT AsWindowsContext(void* nativeContext) {
    return static_cast<PCONTEXT>(nativeContext);
}

bool TryReadStackSlotRaw(uintptr_t address, uint64_t& out) {
    __try {
        out = *reinterpret_cast<const uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryReadStackSlot(uintptr_t address, uint64_t& out) {
    if (TryReadStackSlotRaw(address, out)) return true;

    OSTP_LOG_TRACE("TryReadStackSlot({:#x}) failed", address);
    return false;
}

} // namespace

uint64_t Context::InstructionPointer() const {
    PCONTEXT context = AsWindowsContext(nativeContext_);
    if (!context) {
        OSTP_LOG_TRACE("Context::InstructionPointer: native context is null");
        return 0;
    }
    return context->Rip;
}

uint64_t Context::Argument(int index) const {
    PCONTEXT context = AsWindowsContext(nativeContext_);
    if (!context) {
        OSTP_LOG_TRACE("Context::Argument({}): native context is null", index);
        return 0;
    }

    switch (index) {
    case 1: return context->Rcx;
    case 2: return context->Rdx;
    case 3: return context->R8;
    case 4: return context->R9;
    default: {
        if (index <= 0) {
            OSTP_LOG_DEBUG("Context::Argument: invalid 1-based index {}", index);
            return 0;
        }
        const uintptr_t slot = context->Rsp + static_cast<uintptr_t>(index) * 8;
        uint64_t value = 0;
        if (!TryReadStackSlot(slot, value)) {
            OSTP_LOG_TRACE("Context::Argument({}): stack slot {:#x} is unreadable", index, slot);
            return 0;
        }
        return value;
    }
    }
}

void Context::EnableSingleStep() {
    PCONTEXT context = AsWindowsContext(nativeContext_);
    if (!context) {
        OSTP_LOG_TRACE("Context::EnableSingleStep: native context is null");
        return;
    }
    context->EFlags |= 0x100;
    OSTP_LOG_TRACE("Context::EnableSingleStep: TF set at ip={:#x}", context->Rip);
}

HandlerHandle AddVectoredHandler(Handler handler) {
    if (!handler) {
        OSTP_LOG_DEBUG("AddVectoredHandler: handler is null");
        return nullptr;
    }
    if (g_handle.load(std::memory_order_relaxed)) {
        OSTP_LOG_DEBUG("AddVectoredHandler: handler already installed");
        return nullptr;
    }

    g_handler.store(handler, std::memory_order_release);
    HandlerHandle handle = AddVectoredExceptionHandler(1, VehThunk);
    if (!handle) {
        OSTP_LOG_WARN("AddVectoredExceptionHandler failed (error={})", GetLastError());
        g_handler.store(nullptr, std::memory_order_release);
    } else {
        g_handle.store(handle, std::memory_order_release);
        OSTP_LOG_DEBUG("AddVectoredExceptionHandler installed handle={}", handle);
    }
    return handle;
}

void RemoveVectoredHandler(HandlerHandle handle) {
    if (!handle || handle != g_handle.load(std::memory_order_relaxed)) {
        OSTP_LOG_TRACE("RemoveVectoredHandler: ignoring stale handle {}", handle);
        return;
    }

    if (RemoveVectoredExceptionHandler(handle) == 0) {
        OSTP_LOG_WARN("RemoveVectoredExceptionHandler failed (error={})", GetLastError());
        return;
    }
    g_handle.store(nullptr, std::memory_order_release);
    g_handler.store(nullptr, std::memory_order_release);
    OSTP_LOG_DEBUG("RemoveVectoredExceptionHandler removed handle={}", handle);
}

} // namespace OSTPlatform::Trap
