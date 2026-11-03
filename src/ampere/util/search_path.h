//
// Copyright (c) 2026 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#pragma once

#include <filesystem>
#include <string_view>

namespace ampere::util {

// Return the first executable regular file named `name` in the directories
// listed in $PATH, or an empty path if there is none.
std::filesystem::path search_path(std::string_view name);

} // namespace ampere::util
