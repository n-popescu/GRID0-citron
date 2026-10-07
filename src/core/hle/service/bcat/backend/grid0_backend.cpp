// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/switchnet_account.h"
#include "core/hle/service/bcat/backend/grid0_backend.h"

namespace Service::BCAT {

namespace Account = Service::Account::SwitchNet;

namespace {

bool IsSafeName(const std::string& name) {
    return !name.empty() && name.size() <= 31 && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           });
}

std::string Sha256Hex(const void* data, size_t size) {
    std::array<u8, 32> hash{};
    unsigned int len = static_cast<unsigned int>(hash.size());
    EVP_Digest(data, size, hash.data(), &len, EVP_sha256(), nullptr);
    return Common::HexToString(hash, false);
}

} // namespace

Grid0BcatBackend::Grid0BcatBackend(DirectoryGetter getter) : BcatBackend(std::move(getter)) {}

Grid0BcatBackend::~Grid0BcatBackend() = default;

bool Grid0BcatBackend::Synchronize(TitleIDVersion title, ProgressServiceBackend& progress) {
    return Sync(title, nullptr, progress);
}

bool Grid0BcatBackend::SynchronizeDirectory(TitleIDVersion title, std::string name,
                                            ProgressServiceBackend& progress) {
    return Sync(title, &name, progress);
}

bool Grid0BcatBackend::Sync(TitleIDVersion title, const std::string* only_directory,
                            ProgressServiceBackend& progress) {
    progress.StartConnecting();
    const auto title_hex = fmt::format("{:016x}", title.title_id);
    const auto list = Account::Request("GET", "/emulator/v1/bcat/" + title_hex);
    if (!list || list->status != 200) {
        // No login, an unreachable server, or one without the BCAT API: keep what the cache
        // holds, as a console does when the BCAT server cannot be reached.
        LOG_INFO(Service_BCAT, "GRID0+: no BCAT list for {} ({})", title_hex,
                 list ? list->status : 0);
        progress.FinishDownload(ResultSuccess);
        return true;
    }
    const auto body = nlohmann::json::parse(list->body, nullptr, false);
    if (!body.is_object() || !body.contains("directories") || !body["directories"].is_array()) {
        LOG_WARNING(Service_BCAT, "GRID0+: invalid BCAT manifest for {}; keeping cache", title_hex);
        progress.FinishDownload(ResultSuccess);
        return true;
    }

    // Validate the entire manifest before modifying the delivery cache. A malformed entry must
    // not throw from json::value(), or make a partial list look like deleted server content.
    for (const auto& dir : body["directories"]) {
        if (!dir.is_object() || !dir.contains("name") || !dir["name"].is_string() ||
            !IsSafeName(dir["name"].get<std::string>()) || !dir.contains("files") ||
            !dir["files"].is_array()) {
            LOG_WARNING(Service_BCAT, "GRID0+: invalid BCAT directory for {}; keeping cache", title_hex);
            progress.FinishDownload(ResultSuccess);
            return true;
        }
        for (const auto& file : dir["files"]) {
            if (!file.is_object() || !file.contains("name") || !file["name"].is_string() ||
                !IsSafeName(file["name"].get<std::string>()) || !file.contains("sha256") ||
                !file["sha256"].is_string()) {
                LOG_WARNING(Service_BCAT, "GRID0+: invalid BCAT file for {}; keeping cache", title_hex);
                progress.FinishDownload(ResultSuccess);
                return true;
            }
            const auto hash = file["sha256"].get<std::string>();
            if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) {
                LOG_WARNING(Service_BCAT, "GRID0+: invalid BCAT hash for {}; keeping cache", title_hex);
                progress.FinishDownload(ResultSuccess);
                return true;
            }
        }
    }

    progress.StartProcessingDataList();
    std::map<std::string, std::map<std::string, std::string>> wanted;
    for (const auto& dir : body["directories"]) {
        const std::string dir_name = dir.value("name", "");
        if (!IsSafeName(dir_name) || (only_directory && *only_directory != dir_name)) {
            continue;
        }
        auto& files = wanted[dir_name];
        if (dir.contains("files") && dir["files"].is_array()) {
            for (const auto& file : dir["files"]) {
                const std::string file_name = file.value("name", "");
                if (IsSafeName(file_name)) {
                    files[file_name] = file.value("sha256", "");
                }
            }
        }
    }

    const auto root = dir_getter(title.title_id);
    if (root == nullptr) {
        progress.FinishDownload(ResultSuccess);
        return false;
    }

    size_t downloaded = 0;
    for (const auto& [dir_name, files] : wanted) {
        auto dir = root->GetSubdirectory(dir_name);
        if (dir == nullptr) {
            dir = root->CreateSubdirectory(dir_name);
        }
        if (dir == nullptr) {
            continue;
        }
        for (const auto& [file_name, sha256] : files) {
            if (const auto local = dir->GetFile(file_name); local != nullptr) {
                const auto bytes = local->ReadAllBytes();
                if (Sha256Hex(bytes.data(), bytes.size()) == sha256) {
                    continue;
                }
            }
            const auto data = Account::Request(
                "GET", fmt::format("/emulator/v1/bcat/{}/{}/{}", title_hex, dir_name, file_name));
            if (!data || data->status != 200 ||
                Sha256Hex(data->body.data(), data->body.size()) != sha256) {
                LOG_WARNING(Service_BCAT, "GRID0+: {}/{} did not download intact", dir_name,
                            file_name);
                continue;
            }
            progress.StartDownloadingFile(dir_name, file_name, data->body.size());
            dir->DeleteFile(file_name);
            auto out = dir->CreateFile(file_name);
            if (out == nullptr) {
                continue;
            }
            out->Resize(data->body.size());
            out->WriteBytes(std::vector<u8>(data->body.begin(), data->body.end()));
            progress.UpdateFileProgress(data->body.size());
            progress.FinishDownloadingFile();
            ++downloaded;
        }
        // A file the server no longer has leaves the cache, as on a console.
        for (const auto& local : dir->GetFiles()) {
            if (!files.contains(local->GetName())) {
                dir->DeleteFile(local->GetName());
            }
        }
        progress.CommitDirectory(dir_name);
    }
    if (only_directory == nullptr) {
        for (const auto& local : root->GetSubdirectories()) {
            if (!wanted.contains(local->GetName())) {
                root->DeleteSubdirectoryRecursive(local->GetName());
            }
        }
    }

    LOG_INFO(Service_BCAT, "GRID0+: {} BCAT folder(s) for {}, {} file(s) downloaded",
             wanted.size(), title_hex, downloaded);
    progress.FinishDownload(ResultSuccess);
    return true;
}

bool Grid0BcatBackend::Clear(u64 title_id) {
    const auto root = dir_getter(title_id);
    if (root == nullptr) {
        return true;
    }
    for (const auto& dir : root->GetSubdirectories()) {
        root->DeleteSubdirectoryRecursive(dir->GetName());
    }
    return true;
}

void Grid0BcatBackend::SetPassphrase(u64 title_id, const Passphrase& passphrase) {
    // GRID0+ hands over the files themselves; the passphrase only decrypts a container.
}

std::optional<std::vector<u8>> Grid0BcatBackend::GetLaunchParameter(TitleIDVersion title) {
    return std::nullopt;
}

} // namespace Service::BCAT
