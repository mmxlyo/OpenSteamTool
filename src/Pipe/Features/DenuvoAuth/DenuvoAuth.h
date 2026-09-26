#pragma once

#include "Pipe/PipeTypes.h"

namespace PipeManager::DenuvoAuth {

    // Per-handshake entry point: runs the one-time Denuvo detection (cached per
    // process) and arms the adaptive authorization time window.
    void Apply(const PipeContext& ctx);

    // True only while the pipe belongs to a Denuvo process and is currently
    // within the active authorization window (startup grace period or ticket lease).
    bool IsAuthorizedPipe(const CPipeClient* pipe);

    // True if the pipe belongs to a process detected or forced as Denuvo.
    bool IsDenuvoPipe(const CPipeClient* pipe);

    // Returns the AppId tracked for this pipe, or k_uAppIdInvalid if unknown.
    AppId_t GetAuthorizedAppId(const CPipeClient* pipe);

    // Refreshes the authorization window when an ownership or encrypted ticket is requested.
    void OnTicketRequested(const CPipeClient* pipe, AppId_t appId = k_uAppIdInvalid);

} // namespace PipeManager::DenuvoAuth
