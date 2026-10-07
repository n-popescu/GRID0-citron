// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Service::Sockets {

/**
 * The address a guest DNS lookup for @p host should be answered with, when a private replacement
 * for Nintendo's servers (such as SwitchNet) is configured, or nullopt to resolve it normally.
 *
 * Only Nintendo-owned domains are redirected, so turning this on can never send a game's traffic
 * for an unrelated service to the private server. Every Nintendo host goes there, including ones
 * the server does not serve: those then fail against the private server rather than reaching
 * Nintendo, which is the point of running one.
 */
/// The private server address in effect: the setting, or else the GRID0+ login's server.
/// Empty when neither is set, which turns redirection off.
std::string PrivateServerAddress();

std::optional<std::string> PrivateServerRedirect(std::string_view host);

/**
 * Arms a short trace of the guest's socket calls. Called when a lookup is answered with the
 * private server's address: the calls that follow are the connection to it, which is where a game
 * that fails to go online stops. The trace is logged at Warning, so it is in the log even with the
 * default filter, which hides every other socket call.
 */
void ArmPrivateServerTrace();
/// Persistent diagnostic tracing, rate-limited per second; off by default.
void SetPrivateServerTraceEnabled(bool enabled);

/// Whether to log the next traced socket call. Each call uses up one of a fixed budget, so a busy
/// connection cannot flood the log.
bool TakePrivateServerTrace();

} // namespace Service::Sockets
