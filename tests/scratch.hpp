// A fresh scratch path under the system temp dir. The random suffix keeps concurrent test processes apart:
// ctest -j, and suites run at once from several worktrees or checkouts, share one temp dir (#53).
#pragma once

#include <filesystem>
#include <random>
#include <string>

inline std::filesystem::path scratch_path(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("splinter-" + name + "-" + std::to_string(std::random_device{}()));
}
