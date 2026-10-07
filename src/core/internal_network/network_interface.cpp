// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/bit_cast.h"
#include "common/common_types.h"
#include "common/logging.h"
#include <ranges>
#include "common/settings.h"
#include "common/string_util.h"
#include "core/internal_network/network_interface.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <SystemConfiguration/SystemConfiguration.h>
#include <arpa/inet.h>
#endif
#endif

namespace Network {

#ifdef _WIN32

std::vector<NetworkInterface> GetAvailableNetworkInterfaces() {
    std::vector<IP_ADAPTER_ADDRESSES> adapter_addresses;
    DWORD ret = ERROR_BUFFER_OVERFLOW;
    DWORD buf_size = 0;

    // retry up to 5 times
    for (int i = 0; i < 5 && ret == ERROR_BUFFER_OVERFLOW; i++) {
        ret = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
            nullptr, adapter_addresses.data(), &buf_size);

        if (ret != ERROR_BUFFER_OVERFLOW) {
            break;
        }

        adapter_addresses.resize((buf_size / sizeof(IP_ADAPTER_ADDRESSES)) + 1);
    }

    if (ret != NO_ERROR) {
        LOG_ERROR(Network, "Failed to get network interfaces with GetAdaptersAddresses");
        return {};
    }

    std::vector<NetworkInterface> result;

    for (auto current_address = adapter_addresses.data(); current_address != nullptr;
         current_address = current_address->Next) {
        if (current_address->FirstUnicastAddress == nullptr ||
            current_address->FirstUnicastAddress->Address.lpSockaddr == nullptr) {
            continue;
        }

        if (current_address->OperStatus != IfOperStatusUp) {
            continue;
        }

        const auto ip_addr = Common::BitCast<struct sockaddr_in>(
                                 *current_address->FirstUnicastAddress->Address.lpSockaddr)
                                 .sin_addr;

        ULONG mask = 0;
        if (ConvertLengthToIpv4Mask(current_address->FirstUnicastAddress->OnLinkPrefixLength,
                                    &mask) != NO_ERROR) {
            LOG_ERROR(Network, "Failed to convert IPv4 prefix length to subnet mask");
            continue;
        }

        struct in_addr gateway = {.S_un{.S_addr{0}}};
        if (current_address->FirstGatewayAddress != nullptr &&
            current_address->FirstGatewayAddress->Address.lpSockaddr != nullptr) {
            gateway = Common::BitCast<struct sockaddr_in>(
                          *current_address->FirstGatewayAddress->Address.lpSockaddr)
                          .sin_addr;
        }

        result.emplace_back(NetworkInterface{
            .name{Common::UTF16ToUTF8(std::wstring{current_address->FriendlyName})},
            .ip_address{ip_addr},
            .subnet_mask = in_addr{.S_un{.S_addr{mask}}},
            .gateway = gateway});
    }

    return result;
}

#else

#if defined(__APPLE__) && TARGET_OS_OSX
// Read one routing snapshot per enumeration; offline Macs legitimately have no router.
static std::pair<std::string, u32> GetMacOSDefaultRoute() {
    const auto value = SCDynamicStoreCopyValue(nullptr, CFSTR("State:/Network/Global/IPv4"));
    if (!value) {
        return {};
    }
    std::pair<std::string, u32> result;
    if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        const auto dictionary = static_cast<CFDictionaryRef>(value);
        const auto interface = static_cast<CFStringRef>(
            CFDictionaryGetValue(dictionary, kSCDynamicStorePropNetPrimaryInterface));
        const auto router = static_cast<CFStringRef>(
            CFDictionaryGetValue(dictionary, kSCPropNetIPv4Router));
        char interface_name[IF_NAMESIZE]{};
        char router_address[INET_ADDRSTRLEN]{};
        if (interface && router && CFGetTypeID(interface) == CFStringGetTypeID() &&
            CFGetTypeID(router) == CFStringGetTypeID() &&
            CFStringGetCString(interface, interface_name, sizeof(interface_name), kCFStringEncodingUTF8) &&
            CFStringGetCString(router, router_address, sizeof(router_address), kCFStringEncodingUTF8)) {
            in_addr address{};
            if (inet_pton(AF_INET, router_address, &address) == 1) {
                result = {interface_name, address.s_addr};
            }
        }
    }
    CFRelease(value);
    return result;
}
#endif

std::vector<NetworkInterface> GetAvailableNetworkInterfaces() {
    struct ifaddrs* ifaddr = nullptr;

    if (getifaddrs(&ifaddr) != 0) {
        LOG_ERROR(Network, "Failed to get network interfaces with getifaddrs: {}",
                  std::strerror(errno));
        return {};
    }

    std::vector<NetworkInterface> result;

#if defined(__APPLE__) && TARGET_OS_OSX
    const auto [primary_interface, default_gateway] = GetMacOSDefaultRoute();
#endif

    for (auto ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        u32 gateway{};

#if defined(__APPLE__) && TARGET_OS_OSX
        if (primary_interface == ifa->ifa_name) {
            gateway = default_gateway;
        }
#elif defined(__linux__)
        std::ifstream file{"/proc/net/route"};
        if (!file.is_open()) {
            LOG_ERROR(Network, "Failed to open \"/proc/net/route\"");

            result.emplace_back(NetworkInterface{
                .name{ifa->ifa_name},
                .ip_address{Common::BitCast<struct sockaddr_in>(*ifa->ifa_addr).sin_addr},
                .subnet_mask{Common::BitCast<struct sockaddr_in>(*ifa->ifa_netmask).sin_addr},
                .gateway{in_addr{.s_addr = gateway}}});
            continue;
        }

        // ignore header
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        bool gateway_found = false;

        for (std::string line; std::getline(file, line);) {
            std::istringstream iss{line};

            std::string iface_name;
            iss >> iface_name;
            if (iface_name != ifa->ifa_name) {
                continue;
            }

            iss >> std::hex;

            u32 dest{};
            iss >> dest;
            if (dest != 0) {
                // not the default route
                continue;
            }

            iss >> gateway;

            u16 flags{};
            iss >> flags;

            // flag RTF_GATEWAY (defined in <linux/route.h>)
            if ((flags & 0x2) == 0) {
                continue;
            }

            gateway_found = true;
            break;
        }

        if (!gateway_found) {
            gateway = 0;
        }
#endif

        result.emplace_back(NetworkInterface{
            .name{ifa->ifa_name},
            .ip_address{Common::BitCast<struct sockaddr_in>(*ifa->ifa_addr).sin_addr},
            .subnet_mask{Common::BitCast<struct sockaddr_in>(*ifa->ifa_netmask).sin_addr},
            .gateway{in_addr{.s_addr = gateway}}});
    }

    freeifaddrs(ifaddr);

    return result;
}

#endif

std::optional<NetworkInterface> GetSelectedNetworkInterface() {
    // If airplane mode is enabled, return no interface (similar to Switch's airplane mode)
    if (Settings::values.airplane_mode.GetValue()) {
        return std::nullopt;
    }

    const auto& selected_network_interface = Settings::values.network_interface.GetValue();
    const auto network_interfaces = Network::GetAvailableNetworkInterfaces();
    if (network_interfaces.empty()) {
        LOG_ERROR(Network, "GetAvailableNetworkInterfaces returned no interfaces");
        return std::nullopt;
    }

    // Empty means automatic. Prefer the adapter with the default route rather
    // than an arbitrary bridge/VPN adapter; explicit selections stay explicit.
    if (selected_network_interface.empty()) {
        const auto routed = std::ranges::find_if(network_interfaces, [](const auto& iface) {
            return iface.gateway.s_addr != 0;
        });
        return routed != network_interfaces.end() ? *routed : network_interfaces.front();
    }
    if (selected_network_interface == "None")
        return std::nullopt;

    const auto res =
        std::ranges::find_if(network_interfaces, [&selected_network_interface](const auto& iface) {
            return iface.name == selected_network_interface;
        });

    if (res == network_interfaces.end()) {
        // Only print the error once to avoid log spam
        static bool print_error = true;
        if (print_error) {
            LOG_ERROR(Network, "Couldn't find selected interface \"{}\"",
                      selected_network_interface);
            print_error = false;
        }

        return std::nullopt;
    }

    return *res;
}

void SelectFirstNetworkInterface() {
    const auto network_interfaces = Network::GetAvailableNetworkInterfaces();

    if (network_interfaces.empty()) {
        return;
    }

    Settings::values.network_interface.SetValue(network_interfaces[0].name);
}

} // namespace Network
