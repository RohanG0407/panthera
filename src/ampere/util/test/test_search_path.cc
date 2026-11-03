//
// Copyright (c) 2026 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "ampere/util/search_path.h"

#include <gtest/gtest.h>

namespace ampere::util {

namespace fs = std::filesystem;

class SearchPathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (const char* path = std::getenv("PATH"))
            saved_path = path;
        const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::path{::testing::TempDir()} / (std::string{"search_path_"} + test_info->name());
        fs::remove_all(dir);
        fs::create_directories(dir / "first");
        fs::create_directories(dir / "second");
    }

    void TearDown() override
    {
        if (saved_path)
            setenv("PATH", saved_path->c_str(), 1);
        else
            unsetenv("PATH");
        fs::remove_all(dir);
    }

    void set_path(const std::string& value) { setenv("PATH", value.c_str(), 1); }

    static void make_file(const fs::path& file, fs::perms perms)
    {
        std::ofstream{file} << "#!/bin/sh\n";
        fs::permissions(file, perms);
    }

    std::optional<std::string> saved_path;
    fs::path dir;
};

TEST_F(SearchPathTest, FindsFirstMatchInPathOrder)
{
    make_file(dir / "first" / "tool", fs::perms::owner_all);
    make_file(dir / "second" / "tool", fs::perms::owner_all);
    set_path((dir / "first").string() + ":" + (dir / "second").string());

    EXPECT_EQ(dir / "first" / "tool", search_path("tool"));
}

TEST_F(SearchPathTest, SkipsNonExecutableFilesAndDirectories)
{
    make_file(dir / "first" / "tool", fs::perms::owner_read | fs::perms::owner_write);
    fs::create_directories(dir / "second" / "tool");
    make_file(dir / "third", fs::perms::owner_all);
    set_path((dir / "first").string() + ":" + (dir / "second").string() + ":" + dir.string());

    EXPECT_EQ(fs::path{}, search_path("tool"));
    EXPECT_EQ(dir / "third", search_path("third"));
}

TEST_F(SearchPathTest, ReturnsEmptyWhenMissingOrPathUnset)
{
    set_path((dir / "first").string());
    EXPECT_EQ(fs::path{}, search_path("does-not-exist"));

    unsetenv("PATH");
    EXPECT_EQ(fs::path{}, search_path("sh"));
}

} // namespace ampere::util
