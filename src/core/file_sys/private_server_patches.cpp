// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>

#include "common/settings.h"
#include "core/hle/service/sockets/private_server.h"
#include "core/file_sys/private_server_patches.h"

namespace FileSys {

namespace {

// One .ips file from private_server_patches/, named <category>.<build id>.ips there.
struct EmbeddedPatch {
    std::string_view category;
    std::string_view build_id;
    std::span<const u8> bytes;
};

// The files themselves, as byte arrays, generated at configure time from the directory above
// (CMakeModules/GeneratePrivateServerPatches.cmake) so the bytes a console applies and the bytes
// this emulator applies cannot drift apart. Defines `kEmbeddedPatches`.
#include "private_server_patches_data.inc"

// Older Splatoon 3 builds that have no files of their own, mapped to the build whose files carry
// the same bytes and the categories of those files that apply to them.
//
// These two were covered before the files were embedded, by the same bytes as 11.3.0's
// s3grpcverify_bypass (the pinned-certificate check, 0x00157B20) and s3grpcpeer_bypass (the
// peer-hostname comparison, 0x0014E1B0 and 0x0014DD80): 11.3.0's binary only grew after both
// sites, so 11.2.0 shares them unchanged. The newer categories (s3certpin_bypass,
// s3verifyoption_bypass, s3smallmatch_bypass) sit elsewhere in the binary and have never been
// confirmed for these builds, so they are not applied to them. The oldest build was only ever
// recorded with the certificate bypass -- its peer-hostname offsets were never established -- and
// stays that way rather than being guessed at.
struct SharedSites {
    std::string_view build_id;
    std::string_view patch_build_id;
    std::array<std::string_view, 2> categories;
    size_t count;
};

constexpr std::array<SharedSites, 2> SharedSiteBuilds{{
    // 11.2.0
    {"6830B3A12406CB4716FEC5ADDC35D3E2DC92D212",
     "28C4287AEE36F7499DA60F3E68B54C70DA382D75",
     {"s3grpcverify_bypass", "s3grpcpeer_bypass"},
     2},
    // An older build, certificate-pinning check only.
    {"726D2B882DD9EF10F4A9D73EED088740630FB6C8",
     "28C4287AEE36F7499DA60F3E68B54C70DA382D75",
     {"s3grpcverify_bypass", {}},
     1},
}};

bool SameBuildId(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

} // namespace

std::vector<std::span<const u8>> GetPrivateServerPatches(std::string_view build_id) {
    if (Service::Sockets::PrivateServerAddress().empty()) {
        return {};
    }

    std::string_view patch_build_id = build_id;
    std::span<const std::string_view> only_categories; // empty means every category
    for (const auto& shared : SharedSiteBuilds) {
        if (SameBuildId(shared.build_id, build_id)) {
            patch_build_id = shared.patch_build_id;
            only_categories = {shared.categories.data(), shared.count};
            break;
        }
    }

    std::vector<std::span<const u8>> patches;
    for (const auto& patch : kEmbeddedPatches) {
        if (!SameBuildId(patch.build_id, patch_build_id)) {
            continue;
        }
        if (!only_categories.empty() &&
            std::find(only_categories.begin(), only_categories.end(), patch.category) ==
                only_categories.end()) {
            continue;
        }
        patches.push_back(patch.bytes);
    }
    return patches;
}

} // namespace FileSys
