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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

class File : public std::iostream
{
    std::unique_ptr<std::streambuf> buf;

public:
    explicit File(std::unique_ptr<std::streambuf> streambuf) :
        std::iostream(streambuf.get()),
        buf(std::move(streambuf))
    {
        // Rethrow the buffer's errors instead of only setting badbit.
        this->exceptions(std::ios::badbit);
    }

    File(const File&)                    = delete;
    File(File&&)                         = delete;
    auto operator=(const File&) -> File& = delete;
    auto operator=(File&&) -> File&      = delete;
    ~File() override                     = default;
};

class Storage
{
public:
    auto open(const std::filesystem::path& path, const std::ios::openmode mode = std::ios::in) -> File
    {
        auto buf = std::make_unique<std::filebuf>();
        if (buf->open(path, mode | std::ios::binary) == nullptr) {
            throw std::runtime_error("failed to open `" + path.string() + '`');
        }
        return File(std::move(buf));
    }
};
