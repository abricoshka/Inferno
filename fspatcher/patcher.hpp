/*
 * Patcher.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include "parse.hpp"
#include "storage.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class Patcher
{
    Storage&                                                                                       storage;
    std::unordered_map<std::filesystem::path, std::map<std::streamoff, std::vector<std::uint8_t>>> write_queue;

    static constexpr std::string_view ORIG_BYTE_FILE_EXT = ".InfernoOriginalBytes";

    static constexpr std::uint32_t CS_MAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0;
    static constexpr std::uint32_t CS_MAGIC_CODEDIRECTORY      = 0xFADE0C02;
    static constexpr std::uint32_t CS_SUPPORTSCODELIMIT64      = 0x20300;

    static auto be32(const std::vector<std::uint8_t>& buf, const std::size_t off) -> std::uint32_t
    {
        if (off + 4 > buf.size()) { throw std::out_of_range("code signature is truncated"); }
        return (std::uint32_t(buf[off]) << 24) | (std::uint32_t(buf[off + 1]) << 16)
               | (std::uint32_t(buf[off + 2]) << 8) | std::uint32_t(buf[off + 3]);
    }

    static auto be64(const std::vector<std::uint8_t>& buf, const std::size_t off) -> std::uint64_t
    { return (std::uint64_t(be32(buf, off)) << 32) | be32(buf, off + 4); }

    static auto page_hash(const std::uint8_t hash_type, const std::vector<std::uint8_t>& page)
        -> std::vector<std::uint8_t>
    {
        GChecksumType type;
        switch (hash_type) {
            case 1 : type = G_CHECKSUM_SHA1; break;      // CS_HASHTYPE_SHA1
            case 2 :                                     // CS_HASHTYPE_SHA256
            case 3 : type = G_CHECKSUM_SHA256; break;    // CS_HASHTYPE_SHA256_TRUNCATED
            case 4 : type = G_CHECKSUM_SHA384; break;    // CS_HASHTYPE_SHA384
            default: throw std::runtime_error("unsupported code directory hash type " + std::to_string(hash_type));
        }

        auto* const               sum = g_checksum_new(type);
        std::vector<std::uint8_t> digest(64);
        gsize                     digest_len = digest.size();
        g_checksum_update(sum, page.data(), gssize(page.size()));
        g_checksum_get_digest(sum, digest.data(), &digest_len);
        g_checksum_free(sum);
        digest.resize(digest_len);
        return digest;
    }

    /// Queue new page hashes in the code directory at `cd_off` of `path` for every page that has queued writes.
    void rehash_code_directory(const std::filesystem::path& path, std::iostream& file,
                               const std::vector<std::uint8_t>& sig, const std::streamoff sig_off,
                               const std::size_t                                                  cd_off,
                               std::vector<std::pair<std::streamoff, std::vector<std::uint8_t>>>& out)
    {
        const auto& entries    = this->write_queue.at(path);
        const auto  version    = be32(sig, cd_off + 0x8);
        const auto  hash_off   = be32(sig, cd_off + 0x10);
        const auto  code_slots = be32(sig, cd_off + 0x1C);
        const auto  hash_size  = sig.at(cd_off + 0x24);
        const auto  hash_type  = sig.at(cd_off + 0x25);
        const auto  page_size  = std::uint64_t(1) << sig.at(cd_off + 0x27);
        auto        code_limit = std::uint64_t(be32(sig, cd_off + 0x20));
        if (version >= CS_SUPPORTSCODELIMIT64 && be64(sig, cd_off + 0x38) != 0) {
            code_limit = be64(sig, cd_off + 0x38);
        }

        std::set<std::uint64_t> pages;
        for (const auto& entry : entries) {
            const auto start = std::uint64_t(entry.first);
            const auto end   = start + entry.second.size();
            if (end > code_limit) {
                throw std::runtime_error("write at " + std::to_string(start) + " in `" + path.string()
                                         + "` is not covered by its code signature");
            }
            for (auto page = start / page_size; page * page_size < end; ++page) { pages.emplace(page); }
        }

        for (const auto page : pages) {
            if (page >= code_slots) { throw std::out_of_range("page " + std::to_string(page) + " has no hash slot"); }

            const auto                page_off = page * page_size;
            std::vector<std::uint8_t> data(std::min(page_size, code_limit - page_off));
            read_stream(seek_stream(file, std::streamoff(page_off)), data.data(), std::streamsize(data.size()));

            const auto slot_off = cd_off + hash_off + page * hash_size;
            if (slot_off + hash_size > sig.size()) { throw std::out_of_range("code directory is truncated"); }
            auto old_hash = page_hash(hash_type, data);
            old_hash.resize(hash_size);
            if (!std::equal(old_hash.begin(), old_hash.end(), sig.begin() + std::ptrdiff_t(slot_off))) {
                throw std::runtime_error("page " + std::to_string(page) + " of `" + path.string()
                                         + "` does not match its code directory hash");
            }

            for (const auto& entry : entries) {
                const auto start = std::max(std::uint64_t(entry.first), page_off);
                const auto end   = std::min(std::uint64_t(entry.first) + entry.second.size(), page_off + data.size());
                if (start >= end) { continue; }
                std::copy(entry.second.begin() + std::ptrdiff_t(start - std::uint64_t(entry.first)),
                          entry.second.begin() + std::ptrdiff_t(end - std::uint64_t(entry.first)),
                          data.begin() + std::ptrdiff_t(start - page_off));
            }

            auto new_hash = page_hash(hash_type, data);
            new_hash.resize(hash_size);
            out.emplace_back(sig_off + std::streamoff(slot_off), std::move(new_hash));
        }
    }

public:
    explicit Patcher(Storage& storage) :
        storage(storage)
    { }

    static bool revert(Storage& storage, const std::filesystem::path& path)
    {
        auto orig_bytes_path  = storage.host_path(path);
        orig_bytes_path      += ORIG_BYTE_FILE_EXT;

        if (!std::filesystem::exists(orig_bytes_path)) { return false; }

        std::ifstream orig_bytes_file(orig_bytes_path);
        if (!orig_bytes_file || !orig_bytes_file.is_open()) {
            throw std::runtime_error("failed to open orig bytes file at `" + orig_bytes_path.string() + '`');
        }

        auto file = storage.open(path, std::ios::in | std::ios::out);

        bool        changed = false;
        std::string line;
        while (orig_bytes_file >> line) {
            if (line.back() == ':') {
                file.seekp(std::stoll(line.substr(0, line.size() - 1), nullptr, 16));
                if (!file) { throw std::runtime_error("failed to seek to offset (malformed revert file?)"); }
                continue;
            }
            const auto value = std::stoi(line, nullptr, 16);
            const auto byte  = std::uint8_t(value & 0xFF);
            if (byte != value) { throw std::runtime_error("malformed revert file; byte too large"); }
            file.write(reinterpret_cast<const char*>(&byte), sizeof(byte));
            if (!file) { throw std::runtime_error("failed to restore a byte (malformed revert file?)"); }
            changed = true;
        }

        file.flush();
        orig_bytes_file.close();
        std::filesystem::remove(orig_bytes_path);

        return changed;
    }

    void write(const std::filesystem::path& path, const std::streamoff file_off, const std::uint8_t* bytes,
               const std::size_t nbytes)
    {
        this->write_queue.try_emplace(path);
        this->write_queue[path].emplace(file_off, std::vector(bytes, bytes + nbytes));
    }

    template<const std::size_t N>
    void write(const std::filesystem::path& path, const std::streamoff file_off,
               const std::array<std::uint8_t, N> bytes)
    { this->write(path, file_off, bytes.data(), N); }

    /// Update the code directory page hashes of every page with queued writes, so the pages still validate.
    void rehash()
    {
        for (auto& cache_entries : this->write_queue) {
            auto file = this->storage.open(cache_entries.first);

            // dyld_cache_header.codeSignatureOffset and codeSignatureSize.
            const auto                sig_off  = std::streamoff(read_u64_le(seek_stream(file, 0x28)));
            const auto                sig_size = read_u64_le(file);
            std::vector<std::uint8_t> sig(sig_size);
            read_stream(seek_stream(file, sig_off), sig.data(), std::streamsize(sig.size()));

            if (be32(sig, 0) != CS_MAGIC_EMBEDDED_SIGNATURE) {
                throw std::runtime_error("`" + cache_entries.first.string() + "` has no embedded code signature");
            }

            std::vector<std::pair<std::streamoff, std::vector<std::uint8_t>>> hashes;
            const auto                                                        count = be32(sig, 8);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto blob_off = be32(sig, 12 + std::size_t(i) * 8 + 4);
                if (be32(sig, blob_off) == CS_MAGIC_CODEDIRECTORY) {
                    this->rehash_code_directory(cache_entries.first, file, sig, sig_off, blob_off, hashes);
                }
            }

            for (auto& hash : hashes) { cache_entries.second.emplace(hash.first, std::move(hash.second)); }
        }
    }

    void print_changes() const
    {
        for (const auto& cache_entries : this->write_queue) {
            std::cout << "  " << cache_entries.first << ":\n";
            for (const auto& entry : cache_entries.second) {
                std::cout << "    " << entry.first << ": ";
                for (const auto byte : entry.second) { std::cout << std::uint16_t(byte) << ' '; }
                std::cout << '\n';
            }
        }
    }

    void flush() const
    {
        for (const auto& cache_entries : this->write_queue) {
            auto cache_file       = this->storage.open(cache_entries.first, std::ios::in | std::ios::out);
            auto orig_bytes_path  = this->storage.host_path(cache_entries.first);
            orig_bytes_path      += ORIG_BYTE_FILE_EXT;
            std::ofstream diff_file(orig_bytes_path, std::ios::out | std::ios::trunc);
            if (!diff_file.is_open()) {
                throw std::runtime_error("failed to open orig bytes file suffix=" + cache_entries.first.string());
            }
            diff_file << std::hex;
            for (const auto& entry : cache_entries.second) {
                seek_stream(cache_file, entry.first);
                std::vector<std::uint8_t> orig_bytes(entry.second.size());
                read_stream(cache_file, orig_bytes.data(), std::streamsize(orig_bytes.size()));
                cache_file.seekp(entry.first);
                if (!cache_file) {
                    throw std::runtime_error("failed to seek cache file to " + std::to_string(entry.first));
                }
                cache_file.write(reinterpret_cast<const char*>(entry.second.data()),
                                 std::streamsize(entry.second.size()));
                if (!cache_file) {
                    throw std::runtime_error("failed to write " + std::to_string(entry.second.size())
                                             + " cache file bytes to " + std::to_string(entry.first));
                }
                diff_file << entry.first << ": ";
                for (const auto byte : orig_bytes) { diff_file << std::uint16_t(byte) << ' '; }
                diff_file << '\n';
            }
            cache_file.flush();
            diff_file.flush();
        }
    }
};
