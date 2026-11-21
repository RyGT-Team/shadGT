#pragma once

#include <filesystem>
#include <vector>

namespace Ryty {

struct App0Roots {
    std::filesystem::path top;
    std::filesystem::path ini;
};

std::vector<std::filesystem::path> App0Layers(const std::filesystem::path& root,
                                              const App0Roots& roots);
std::filesystem::path App0File(const std::filesystem::path& root, const App0Roots& roots,
                               const std::filesystem::path& relative);
std::vector<std::filesystem::path> App0ListFiles(const std::filesystem::path& root,
                                                 const App0Roots& roots,
                                                 const std::filesystem::path& relative);

std::vector<std::filesystem::path> App0Layers(const std::filesystem::path& root);

std::filesystem::path App0File(const std::filesystem::path& root,
                               const std::filesystem::path& relative);

std::vector<std::filesystem::path> App0ListFiles(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative);

} // namespace Ryty
