#include "dllmain.h"
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Pipe/PipeManager.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/SteamMetadata/IPCLoader.h"

namespace {

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    //  Handler dispatch table
    struct ResolvedHandler {
        uint64_t              key;        // (static_cast<uint64_t>(interfaceID) << 32) | funcHash
        EIPCInterface         interfaceID;
        uint32                funcHash;
        std::string           name;       // "IClientUser::GetSteamID" — for logs
        uint32                fencepost;
        uint32                argc;
        IPCHandlerFn          pre;
        IPCHandlerFn          post;

        ResolvedHandler(const IPCHandlerEntry& entry, const IPCLoader::Method& method)
            : key((static_cast<uint64_t>(method.interfaceID) << 32) | method.funcHash),
              interfaceID(method.interfaceID),
              funcHash(method.funcHash),
              name(std::string(entry.interfaceName) + "::" + entry.methodName),
              fencepost(method.fencepost),
              argc(method.argc),
              pre(entry.pre),
              post(entry.post) {}

        std::string DebugString() const {
            return std::format("{} -> hash=0x{:08X} fencepost=0x{:08X} argc={}",
                name, funcHash, fencepost, argc);
        }
    };
    std::vector<ResolvedHandler> g_Handlers;

    static const ResolvedHandler* FindHandler(EIPCInterface iface, uint32 funcHash) {
        const uint64_t targetKey = (static_cast<uint64_t>(iface) << 32) | funcHash;
        for (const auto& e : g_Handlers) {
            if (e.key == targetKey) return &e;
        }
        return nullptr;
    }

    static void HandleHandshake(void* pServer, HSteamPipe hSteamPipe, const IPCMessages::IPCRequest& request)
    {
        IPCMessages::IPCHandshakeReq handshake{request.body()};
        if (!handshake.ok()) return;

        CPipeClient* pipe = GetPipe(pServer, hSteamPipe);
        if (!pipe) return;
        // set client PID 
        pipe->m_clientPID = handshake.pid();

        LOG_IPC_DEBUG("Received handshake from {},{}", pipe->DebugString(), handshake.DebugString());
        PipeManager::OnHandshake(pipe);
    }

    HOOK_FUNC(IPCProcessMessage, bool, void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        // Parse request header once for this buffer
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok())
            return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        const EIPCCommand cmd = request.command();

        // 1. Handshake messages
        if (cmd == EIPCCommand::Handshake) {
            HandleHandshake(pServer, hSteamPipe, request);
            return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        }

        // 2. InterfaceCall messages
        if (cmd == EIPCCommand::InterfaceCall) {
            IPCMessages::IPCInterfaceCall call{request.body()};
            if (!call.ok())
                return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

            // Detect the first SteamNetworkingSockets call (interface 46) so GetAppID can
            // flip to 480 for P2P games. Skipped once already seen or when not in onlinefix.
            if (Hooks_Misc::IsOnlineFixActive() && !Hooks_Misc::ShouldReportOnlineFixAppId()) {
                if (call.interfaceID() == EIPCInterface::IClientNetworkingSocketsSerialized)
                    Hooks_Misc::NotifyNetworkingSocketsUsed();
            }

            // Lookup handler by interface ID + method hash.
            // If matched and app is configured in Lua, resolve pipe and invoke handlers.
            if (const auto* handler = FindHandler(call.interfaceID(), call.funcHash())) {
                if (LuaConfig::HasDepot(Hooks_Misc::ResolveAppId())) {
                    if (CPipeClient* pipe = GetPipe(pServer, hSteamPipe)) {
                        LOG_IPC_TRACE("Resolved IPC handler: {} {}", pipe->DebugString(), handler->DebugString());
                        if (handler->pre)
                            handler->pre(pipe, pRead, pWrite);

                        bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

                        if (result && handler->post)
                            handler->post(pipe, pRead, pWrite);

                        return result;
                    }
                }
            }
        }

        // All other IPC messages pass through to the original function.
        return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
    }

} // namespace


namespace Hooks_IPC {

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Each module registers a static array. Hash lookup against the
        // IPCLoader metadata happens inside RegisterHandlers.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        LOG_IPC_INFO("Hooks_IPC: {} handlers registered", g_Handlers.size());

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

    void RegisterHandlers(std::span<const IPCHandlerEntry> entries) {
        for (const auto& e : entries) {
            const auto* m = IPCLoader::Find(e.interfaceName, e.methodName);
            if (!m) {
                LOG_IPC_WARN("[Handler Disabled] no IPC spec for {}",e.DebugString());
                continue;
            }
            auto& handler = g_Handlers.emplace_back(e,*m);
            LOG_IPC_DEBUG("Hooks_IPC: resolved {}", handler.DebugString());
        }
    }

}
