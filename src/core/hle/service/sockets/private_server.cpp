// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>

#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/sockets/private_server.h"

namespace Service::Sockets {

namespace {

// Domains Nintendo operates. The same list SwitchNet's own DNS configuration treats as Nintendo's
// (internal/nintendo/hosts.go), so the emulator and a console behind SwitchNet's resolver agree on
// which names belong to the private server.
constexpr std::array<std::string_view, 33> NintendoDomains{
    "nintendo.com",      "nintendo.net",        "nintendo.jp",        "nintendo.co.jp",
    "nintendo.co.uk",    "nintendo-europe.com", "nintendowifi.net",   "nintendo.es",
    "nintendo.co.kr",    "nintendo.tw",         "nintendo.com.hk",    "nintendo.com.au",
    "nintendo.co.nz",    "nintendo.at",         "nintendo.be",        "nintendods.cz",
    "nintendo.dk",       "nintendo.de",         "nintendo.fi",        "nintendo.fr",
    "nintendo.gr",       "nintendo.hu",         "nintendo.it",        "nintendo.nl",
    "nintendo.no",       "nintendo.pt",         "nintendo.ru",        "nintendo.co.za",
    "nintendo.se",       "nintendo.ch",         "nintendoswitch.com", "nintendoswitch.com.cn",
    "nintendoswitch.cn",
};

std::string Normalise(std::string_view host) {
    std::string name{host};
    while (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

bool IsNintendoDomain(std::string_view name) {
    return std::any_of(NintendoDomains.begin(), NintendoDomains.end(), [name](auto domain) {
        if (name == domain) {
            return true;
        }
        return name.size() > domain.size() && name.ends_with(domain) &&
               name[name.size() - domain.size() - 1] == '.';
    });
}

} // namespace

std::string PrivateServerAddress() {
    std::string address = Settings::values.private_server_address.GetValue();
    if (!address.empty()) {
        return address;
    }
    // A config saved before the GRID0+ defaults existed holds an explicit empty address, which
    // overrides them. With a GRID0+ login set, use that login's server: signing in there while
    // the game itself talks to Nintendo fails with 2321-4992.
    if (Settings::values.switchnet_username.GetValue().empty()) {
        return {};
    }
    address = Settings::values.switchnet_server.GetValue();
    if (const auto colon = address.find(':'); colon != std::string::npos) {
        address.resize(colon);
    }
    return address;
}

std::optional<std::string> PrivateServerRedirect(std::string_view host) {
    const std::string primary = PrivateServerAddress();
    if (primary.empty()) {
        return std::nullopt;
    }

    const std::string name = Normalise(host);
    if (!IsNintendoDomain(name)) {
        return std::nullopt;
    }

    // Pia probes nncs1 and nncs2 as two servers, and one NAT-check message has to be answered
    // from a different address: the same address makes the console de-duplicate the probe, and
    // matchmaking then finds matches that never start.
    if (name.starts_with("nncs2")) {
        std::string secondary = Settings::values.private_server_nat_secondary_address.GetValue();
        if (secondary.empty() && primary == Settings::Grid0DefaultAddress) {
            secondary = std::string(Settings::Grid0DefaultNatSecondaryAddress);
        }
        if (!secondary.empty()) {
            return secondary;
        }
        LOG_WARNING(Network,
                    "{} redirected to the primary private server address because no NAT-check "
                    "secondary address is set; Pia NAT detection will not complete",
                    host);
    }

    return primary;
}

namespace {
// Enough for a connection to be opened, its TLS handshake and its first requests, with room
// for a retry; a poll that is re-checked while deferred is not counted until it returns.
constexpr int TraceBudget = 256;
std::atomic<int> trace_remaining{0};
std::atomic<bool> persistent_trace{false};
std::mutex trace_mutex;
std::chrono::steady_clock::time_point trace_window{};
int window_remaining{};
constexpr int PersistentTraceBudget = 512;
} // namespace

void ArmPrivateServerTrace() {
    trace_remaining.store(TraceBudget, std::memory_order_relaxed);
}

void SetPrivateServerTraceEnabled(bool enabled) {
    std::scoped_lock lock{trace_mutex};
    trace_window = {};
    window_remaining = 0;
    persistent_trace.store(enabled, std::memory_order_relaxed);
}

bool TakePrivateServerTrace() {
    if (persistent_trace.load(std::memory_order_relaxed)) {
        std::scoped_lock lock{trace_mutex};
        const auto now = std::chrono::steady_clock::now();
        if (now - trace_window >= std::chrono::seconds(1)) {
            trace_window = now;
            window_remaining = PersistentTraceBudget;
        }
        if (window_remaining == 0)
            return false;
        --window_remaining;
        return true;
    }
    int remaining = trace_remaining.load(std::memory_order_relaxed);
    while (remaining > 0) {
        if (trace_remaining.compare_exchange_weak(remaining, remaining - 1,
                                                  std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

} // namespace Service::Sockets
