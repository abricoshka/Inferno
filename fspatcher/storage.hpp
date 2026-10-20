/*
 * Storage.
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
#include "disk.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

[[noreturn]]
void throw_disk_error(char* err)
{
    std::string msg(err);
    disk_error_free(err);
    throw std::runtime_error(msg);
}

class DiskFileBuf : public std::streambuf
{
    ApfsFile*                 file;
    std::uint64_t             size;
    std::uint64_t             buf_off{0};
    std::array<char, 0x10000> buf{};

    [[nodiscard]]
    auto pos() const -> std::uint64_t
    { return this->buf_off + std::uint64_t(this->gptr() - this->eback()); }

    auto seek_to(const off_type target) -> pos_type
    {
        if (target < 0) { return pos_type(off_type(-1)); }

        const auto off   = std::uint64_t(target);
        const auto avail = std::uint64_t(this->egptr() - this->eback());
        if (off >= this->buf_off && off - this->buf_off <= avail) {
            this->setg(this->eback(), this->eback() + (off - this->buf_off), this->egptr());
        }
        else {
            this->buf_off = off;
            this->setg(this->buf.data(), this->buf.data(), this->buf.data());
        }

        return pos_type(target);
    }

protected:
    auto underflow() -> int_type override
    {
        const auto off = this->pos();
        if (off >= this->size) { return traits_type::eof(); }

        const auto len = std::min(std::uint64_t(this->buf.size()), this->size - off);
        char*      err = nullptr;
        if (!disk_file_pread(this->file, off, len, this->buf.data(), &err)) { throw_disk_error(err); }

        this->buf_off = off;
        this->setg(this->buf.data(), this->buf.data(), this->buf.data() + len);
        return traits_type::to_int_type(this->buf[0]);
    }

    auto seekoff(const off_type off, const std::ios::seekdir dir, const std::ios::openmode /*which*/)
        -> pos_type override
    {
        switch (dir) {
            case std::ios::beg: return this->seek_to(off);
            case std::ios::cur: return this->seek_to(off_type(this->pos()) + off);
            case std::ios::end: return this->seek_to(off_type(this->size) + off);
            default           : return pos_type(off_type(-1));
        }
    }

    auto seekpos(const pos_type pos, const std::ios::openmode /*which*/) -> pos_type override
    { return this->seek_to(off_type(pos)); }

    auto xsputn(const char* const bytes, const std::streamsize nbytes) -> std::streamsize override
    {
        const auto off = this->pos();
        char*      err = nullptr;
        if (!disk_file_pwrite(this->file, off, std::uint64_t(nbytes), bytes, &err)) { throw_disk_error(err); }

        this->buf_off = off + std::uint64_t(nbytes);
        this->setg(this->buf.data(), this->buf.data(), this->buf.data());
        return nbytes;
    }

public:
    explicit DiskFileBuf(ApfsFile* const file) :
        file(file),
        size(disk_file_size(file))
    { this->setg(this->buf.data(), this->buf.data(), this->buf.data()); }
};

class File : public std::iostream
{
    std::unique_ptr<std::streambuf> buf;

public:
    explicit File(std::unique_ptr<std::streambuf> streambuf) :
        std::iostream(streambuf.get()),
        buf(std::move(streambuf))
    { this->exceptions(std::ios::badbit); }

    File(const File&)                    = delete;
    File(File&&)                         = delete;
    auto operator=(const File&) -> File& = delete;
    auto operator=(File&&) -> File&      = delete;
    ~File() override                     = default;
};

class FileSystem
{
public:
    static constexpr std::string_view ORIG_BYTE_FILE_EXT = ".InfernoOriginalBytes";

    FileSystem()                                     = default;
    FileSystem(const FileSystem&)                    = delete;
    FileSystem(FileSystem&&)                         = delete;
    auto operator=(const FileSystem&) -> FileSystem& = delete;
    auto operator=(FileSystem&&) -> FileSystem&      = delete;
    virtual ~FileSystem()                            = default;

    virtual auto open(const std::filesystem::path& path, std::ios::openmode mode = std::ios::in) -> File = 0;

    [[nodiscard]]
    virtual auto host_path(const std::filesystem::path& path) const -> std::filesystem::path = 0;

    [[nodiscard]]
    virtual auto name() const -> std::string_view = 0;

    [[nodiscard]]
    virtual auto canonical() -> FileSystem&
    { return *this; }
};

class FileRef
{
    FileSystem*           fs;
    std::filesystem::path file_path;

public:
    FileRef(FileSystem& fs, std::filesystem::path path) :
        fs(&fs.canonical()),
        file_path(std::move(path))
    { }

    [[nodiscard]]
    auto path() const -> const std::filesystem::path&
    { return this->file_path; }

    [[nodiscard]]
    auto storage() const -> FileSystem&
    { return *this->fs; }

    [[nodiscard]]
    auto open(const std::ios::openmode mode = std::ios::in) const -> File
    { return this->fs->open(this->file_path, mode); }

    [[nodiscard]]
    auto orig_bytes_path() const -> std::filesystem::path
    {
        auto ret  = this->fs->host_path(this->file_path);
        ret      += FileSystem::ORIG_BYTE_FILE_EXT;
        return ret;
    }

    [[nodiscard]]
    auto string() const -> std::string
    { return '[' + std::string(this->fs->name()) + "] " + this->file_path.string(); }

    friend auto operator<(const FileRef& lhs, const FileRef& rhs) -> bool
    {
        if (lhs.file_path != rhs.file_path) { return lhs.file_path < rhs.file_path; }
        return std::less<const FileSystem*>()(lhs.fs, rhs.fs);
    }
};

class Storage : public FileSystem
{
    std::filesystem::path                      disk_path;
    BlockBackend*                              disk{nullptr};
    std::unordered_map<std::string, ApfsFile*> files;

public:
    explicit Storage(const std::filesystem::path& disk_path) :
        disk_path(disk_path)
    {
        if (disk_path.empty()) { return; }

        char* err  = nullptr;
        this->disk = disk_open(disk_path.string().c_str(), &err);
        if (this->disk == nullptr) { throw_disk_error(err); }
    }

    Storage(const Storage&)                    = delete;
    Storage(Storage&&)                         = delete;
    auto operator=(const Storage&) -> Storage& = delete;
    auto operator=(Storage&&) -> Storage&      = delete;

    ~Storage() override
    {
        for (const auto& file : this->files) { disk_file_close(file.second); }
        if (this->disk != nullptr) { disk_close(this->disk); }
    }

    [[nodiscard]]
    auto backend() const -> BlockBackend*
    { return this->disk; }

    auto open(const std::filesystem::path& path, const std::ios::openmode mode = std::ios::in) -> File override
    {
        if (this->disk == nullptr) {
            auto buf = std::make_unique<std::filebuf>();
            if (buf->open(path, mode | std::ios::binary) == nullptr) {
                throw std::runtime_error("failed to open `" + path.string() + '`');
            }
            return File(std::move(buf));
        }

        auto file = this->files.find(path.string());
        if (file == this->files.end()) {
            char* err  = nullptr;
            auto* apfs = disk_file_open(this->disk, path.string().c_str(), &err);
            if (apfs == nullptr) { throw_disk_error(err); }
            file = this->files.emplace(path.string(), apfs).first;
        }

        return File(std::make_unique<DiskFileBuf>(file->second));
    }

    [[nodiscard]]
    auto host_path(const std::filesystem::path& path) const -> std::filesystem::path override
    { return this->host_path(path, {}); }

    [[nodiscard]]
    auto host_path(const std::filesystem::path& path, const std::string_view tag) const -> std::filesystem::path
    {
        if (this->disk == nullptr) { return path; }

        auto ret  = this->disk_path;
        ret      += '.';
        if (!tag.empty()) {
            ret += tag;
            ret += '.';
        }
        ret += path.filename();
        return ret;
    }

    [[nodiscard]]
    auto name() const -> std::string_view override
    { return this->disk == nullptr ? "host" : "system volume"; }
};

class DyldStorage : public FileSystem
{
    Storage&                                   storage;
    ApfsFile*                                  cryptex{nullptr};
    std::unordered_map<std::string, ApfsFile*> files;

public:
    explicit DyldStorage(Storage& storage) :
        storage(storage)
    {
        if (storage.backend() == nullptr) { return; }

        char* err     = nullptr;
        this->cryptex = disk_cryptex_open(storage.backend(), &err);
        if (this->cryptex == nullptr && err != nullptr) { disk_error_free(err); }
    }

    DyldStorage(const DyldStorage&)                    = delete;
    DyldStorage(DyldStorage&&)                         = delete;
    auto operator=(const DyldStorage&) -> DyldStorage& = delete;
    auto operator=(DyldStorage&&) -> DyldStorage&      = delete;

    ~DyldStorage() override
    {
        for (const auto& file : this->files) { disk_file_close(file.second); }
        if (this->cryptex != nullptr) { disk_file_close(this->cryptex); }
    }

    auto open(const std::filesystem::path& path, const std::ios::openmode mode = std::ios::in) -> File override
    {
        if (this->cryptex == nullptr) { return this->storage.open(path, mode); }

        auto file = this->files.find(path.string());
        if (file == this->files.end()) {
            char* err  = nullptr;
            auto* apfs = disk_image_file_open(this->cryptex, path.string().c_str(), &err);
            if (apfs == nullptr) { throw_disk_error(err); }
            file = this->files.emplace(path.string(), apfs).first;
        }

        return File(std::make_unique<DiskFileBuf>(file->second));
    }

    [[nodiscard]]
    auto host_path(const std::filesystem::path& path) const -> std::filesystem::path override
    { return this->cryptex == nullptr ? this->storage.host_path(path) : this->storage.host_path(path, "cryptex"); }

    [[nodiscard]]
    auto name() const -> std::string_view override
    { return this->cryptex == nullptr ? this->storage.name() : "OS cryptex"; }

    [[nodiscard]]
    auto canonical() -> FileSystem& override
    { return this->cryptex == nullptr ? this->storage.canonical() : static_cast<FileSystem&>(*this); }
};
