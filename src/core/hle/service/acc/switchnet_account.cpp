// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <memory>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#ifdef ENABLE_WEB_SERVICE
#include <httplib.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/acc/switchnet_account.h"
#include "core/hle/service/ssl/switchnet_ca.h"

namespace Service::Account::SwitchNet {

namespace {

// The hostname a console uses for BAAS. /login is served on this same host: SwitchNet's edge
// routes on SNI and Host, so this is the name to present even though the socket is dialled at
// the configured server address.
constexpr std::string_view BaasHost = "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";

// A token that expires while a match is being set up fails inside the game, where the reason is
// invisible. Refreshing early costs one request nobody notices.
constexpr auto ExpiryMargin = std::chrono::minutes(5);

std::mutex mutex;
std::string cached_token;
std::string cached_access_token;
std::chrono::system_clock::time_point cached_expiry;
std::string cached_for; // internal credentials/trust identity; never log this
std::set<std::string> reported_failures;
// A game asks for its token repeatedly; after a failed login, do not hammer the server (whose
// login is rate limited) with the same wrong password on every call.
constexpr auto RetryAfterFailure = std::chrono::seconds(30);
std::chrono::steady_clock::time_point last_failure;

void ReportFailure(const std::string& reason) {
    if (reported_failures.insert(reason).second) {
        LOG_WARNING(Service_ACC,
                    "SwitchNet login failed: {}. The game gets no id token and will fail to reach "
                    "the game server",
                    reason);
    }
}

std::optional<std::string> DecodeBase64Url(std::string_view in) {
    std::string out;
    u32 buffer = 0;
    int bits = 0;
    for (const char c : in) {
        int value;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '-' || c == '+') {
            value = 62;
        } else if (c == '_' || c == '/') {
            value = 63;
        } else if (c == '=') {
            break;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// Reads 'exp' without verifying the signature. That is safe: the token is not being checked, it
// was just received over a verified connection to be handed straight back to the server that
// signed it, and the only thing read is when to ask for another.
std::chrono::system_clock::time_point ReadExpiry(const std::string& jwt) {
    const auto fallback = std::chrono::system_clock::now() + std::chrono::minutes(30);

    const auto first = jwt.find('.');
    const auto second = jwt.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return fallback;
    }

    const auto decoded =
        DecodeBase64Url(std::string_view{jwt}.substr(first + 1, second - first - 1));
    if (!decoded) {
        return fallback;
    }

    const auto claims = nlohmann::json::parse(*decoded, nullptr, false);
    if (!claims.is_object() || !claims.contains("exp") || !claims["exp"].is_number_integer()) {
        return fallback;
    }
    return std::chrono::system_clock::time_point{std::chrono::seconds{claims["exp"].get<s64>()}};
}

#ifdef ENABLE_WEB_SERVICE

// The server's certificate is verified against the same CA the guest trusts: the configured
// bundle, or else the built-in SwitchNet Local CA. This request carries the user's password, so
// it is not sent to a server whose certificate does not check out.
X509_STORE* BuildTrustStore() {
    X509_STORE* store = X509_STORE_new();
    if (!store) {
        return nullptr;
    }

    const std::string& bundle = Settings::values.private_server_ca_bundle.GetValue();
    if (!bundle.empty()) {
        if (X509_STORE_load_locations(store, bundle.c_str(), nullptr) != 1) {
            X509_STORE_free(store);
            return nullptr;
        }
        return store;
    }

    const auto& pem = Service::SSL::SwitchNetLocalCaPem;
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        X509_STORE_add_cert(store, cert);
        X509_free(cert);
    }
    BIO_free(bio);
    return store;
}

struct Endpoint {
    std::string host;
    int port = 443;
};

std::optional<Endpoint> ParseServer(const std::string& server) {
    Endpoint endpoint;
    const auto colon = server.rfind(':');
    if (colon == std::string::npos || server.find(':') != colon) {
        endpoint.host = server;
    } else {
        endpoint.host = server.substr(0, colon);
        try {
            endpoint.port = std::stoi(server.substr(colon + 1));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (endpoint.host.empty() || endpoint.port <= 0 || endpoint.port > 65535) {
        return std::nullopt;
    }
    return endpoint;
}

// A client for one of Nintendo's hostnames, dialled at the configured server instead. Every
// request keeps the Nintendo hostname, so the Host header and SNI are what the server routes on.
std::unique_ptr<httplib::SSLClient> MakeClient(const std::string& host, int timeout_seconds) {
    const std::string& server = Settings::values.switchnet_server.GetValue();
    const auto endpoint = ParseServer(server);
    if (!endpoint) {
        ReportFailure(fmt::format("'{}' is not a host or host:port", server));
        return nullptr;
    }
    auto client = std::make_unique<httplib::SSLClient>(host, endpoint->port);
    client->set_hostname_addr_map({{host, endpoint->host}});
    client->set_connection_timeout(timeout_seconds);
    client->set_read_timeout(timeout_seconds);
    client->enable_server_certificate_verification(true);

    X509_STORE* store = BuildTrustStore();
    if (!store) {
        ReportFailure("the CA to verify the server with could not be loaded");
        return nullptr;
    }
    client->set_ca_cert_store(store); // owned by the client from here
    return client;
}

struct Tokens {
    std::string id;
    std::string access;
};

std::optional<Tokens> Login() {
    const std::string& server = Settings::values.switchnet_server.GetValue();
    auto client_ptr = MakeClient(std::string{BaasHost}, 10);
    if (!client_ptr) {
        return std::nullopt;
    }
    auto& client = *client_ptr;

    const httplib::Params form{
        {"username", Settings::values.switchnet_username.GetValue()},
        {"password", Settings::values.switchnet_password.GetValue()},
    };

    const auto response = client.Post("/login", form);
    if (!response) {
        ReportFailure(fmt::format("could not reach {} at {}: {}", BaasHost, server,
                                  httplib::to_string(response.error())));
        return std::nullopt;
    }

    const auto body = nlohmann::json::parse(response->body, nullptr, false);
    if (response->status != 200) {
        std::string reason = response->body.substr(0, 200);
        if (body.is_object() && body.contains("error") && body["error"].is_string()) {
            reason = body["error"].get<std::string>();
        }
        ReportFailure(fmt::format("login was refused ({}): {}", response->status, reason));
        return std::nullopt;
    }

    if (!body.is_object() || !body.contains("idToken") || !body["idToken"].is_string() ||
        body["idToken"].get<std::string>().empty()) {
        ReportFailure("the login response carried no idToken");
        return std::nullopt;
    }

    Tokens tokens{body["idToken"].get<std::string>(), {}};
    if (body.contains("accessToken") && body["accessToken"].is_string()) {
        tokens.access = body["accessToken"].get<std::string>();
    }
    return tokens;
}

#else

std::optional<Tokens> Login() {
    ReportFailure("this build has no HTTPS client (built without ENABLE_WEB_SERVICE)");
    return std::nullopt;
}

#endif

} // namespace

bool IsConfigured() {
    return !Settings::values.switchnet_server.GetValue().empty() &&
           !Settings::values.switchnet_username.GetValue().empty() &&
           !Settings::values.switchnet_password.GetValue().empty();
}

namespace {
// Logs in if the cached tokens are missing or about to expire. Holds `mutex`.
bool EnsureLoggedIn() {
    // Changing a password or trust bundle must invalidate both cached tokens
    // and a failed-login cooldown. This identity is internal and never logged.
    const std::string identity = Settings::values.switchnet_server.GetValue() + '\n' +
                                 Settings::values.switchnet_username.GetValue() + '\n' +
                                 Settings::values.switchnet_password.GetValue() + '\n' +
                                 Settings::values.private_server_ca_bundle.GetValue();
    if (!cached_token.empty() && cached_for == identity &&
        std::chrono::system_clock::now() + ExpiryMargin < cached_expiry) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (cached_for == identity && last_failure != std::chrono::steady_clock::time_point{} &&
        now - last_failure < RetryAfterFailure) {
        return false;
    }

    auto tokens = Login();
    if (!tokens) {
        last_failure = now;
        cached_for = identity;
        cached_token.clear();
        cached_access_token.clear();
        return false;
    }
    last_failure = {};

    LOG_INFO(Service_ACC, "Logged in to GRID0+ as {}",
             Settings::values.switchnet_username.GetValue());
    reported_failures.clear();
    cached_token = std::move(tokens->id);
    cached_access_token = std::move(tokens->access);
    cached_expiry = ReadExpiry(cached_token);
    cached_for = identity;
    return true;
}
} // namespace

std::optional<u64> GetNetworkServiceAccountId() {
    const auto token = GetIdToken();
    if (!token) {
        return std::nullopt;
    }
    const auto first = token->find('.');
    const auto second = token->find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return std::nullopt;
    }
    const auto decoded =
        DecodeBase64Url(std::string_view{*token}.substr(first + 1, second - first - 1));
    if (!decoded) {
        return std::nullopt;
    }
    const auto claims = nlohmann::json::parse(*decoded, nullptr, false);
    if (!claims.is_object() || !claims.contains("sub") || !claims["sub"].is_string()) {
        return std::nullopt;
    }
    const auto subject = claims["sub"].get<std::string>();
    if (subject.empty() || subject.size() > 16 ||
        subject.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return std::nullopt;
    }
    return std::stoull(subject, nullptr, 16);
}

std::optional<std::string> GetIdToken() {
    if (!IsConfigured()) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    if (!EnsureLoggedIn()) {
        return std::nullopt;
    }
    return cached_token;
}

std::optional<std::string> GetAccessToken() {
    if (!IsConfigured()) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    if (!EnsureLoggedIn() || cached_access_token.empty()) {
        return std::nullopt;
    }
    return cached_access_token;
}

std::optional<Response> Request(const std::string& method, const std::string& path,
                                const std::string& json_body) {
#ifdef ENABLE_WEB_SERVICE
    const auto token = GetAccessToken();
    if (!token) {
        return std::nullopt;
    }
    auto client = MakeClient(std::string{BaasHost}, 8);
    if (!client) {
        return std::nullopt;
    }
    const httplib::Headers headers{{"Authorization", "Bearer " + *token}};
    httplib::Result result;
    if (method == "GET") {
        result = client->Get(path, headers);
    } else if (method == "DELETE") {
        result = client->Delete(path, headers);
    } else {
        result = client->Post(path, headers, json_body, "application/json");
    }
    if (!result) {
        return std::nullopt;
    }
    return Response{result->status, result->body};
#else
    return std::nullopt;
#endif
}

std::optional<std::string> Fetch(const std::string& url) {
#ifdef ENABLE_WEB_SERVICE
    // Only https URLs on hosts SwitchNet answers for: the host is redirected to the server.
    constexpr std::string_view Scheme = "https://";
    if (!url.starts_with(Scheme)) {
        return std::nullopt;
    }
    const auto rest = url.substr(Scheme.size());
    const auto slash = rest.find('/');
    const std::string host = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (!host.ends_with(".nintendo.com") && !host.ends_with(".nintendo.net")) {
        return std::nullopt;
    }
    auto client = MakeClient(host, 8);
    if (!client) {
        return std::nullopt;
    }
    auto result = client->Get(path);
    if (!result || result->status != 200) {
        return std::nullopt;
    }
    return result->body;
#else
    return std::nullopt;
#endif
}

} // namespace Service::Account::SwitchNet
