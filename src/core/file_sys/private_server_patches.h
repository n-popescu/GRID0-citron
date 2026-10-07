// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace FileSys {

/**
 * Built-in IPS patches an executable needs before it will talk to a private replacement for
 * Nintendo's servers, or none. Keyed on the executable's exact build id (hex, trailing zeros
 * trimmed, any case), and only returned while a private server address is set.
 *
 * Splatoon 3 is the reason this exists: its online client does TLS itself, over raw sockets, with
 * its own statically linked TLS stack and pinned certificates, so trusting a CA in the emulated
 * ssl service never reaches it and the game sits on "connecting" forever. The patches are the real
 * .ips files the GRID0+ toolbox installs on a console as exefs_patches, kept in
 * private_server_patches/ as <category>.<build id>.ips and embedded at build time: the
 * certificate-pinning check forced to pass, the peer-hostname comparison that follows it, and the
 * two further verification patches. Only the game's own patches travel with the emulator; the
 * toolbox's bcat, system ssl and browser patches target modules this emulator emulates instead of
 * running, so they could never match a loaded game. A title update changes the build id and
 * silently stops them applying.
 */
std::vector<std::span<const u8>> GetPrivateServerPatches(std::string_view build_id);

} // namespace FileSys
