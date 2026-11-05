//
// Copyright (c) 2026 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <unistd.h>

#include "search_path.h"

namespace ampere::util {

std::filesystem::path search_path(std::string_view name)
{
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr)
        return {};

    std::string_view dirs{path_env};
    while (true) {
        auto separator = dirs.find(':');
        auto candidate = std::filesystem::path{dirs.substr(0, separator)} / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (separator == std::string_view::npos)
            return {};
        dirs.remove_prefix(separator + 1);
    }
}

} // namespace ampere::util
