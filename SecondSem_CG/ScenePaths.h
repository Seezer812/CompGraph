#pragma once

#include <filesystem>
#include <string>

namespace ScenePaths
{
std::filesystem::path FindSponzaObj(const std::wstring& executableDirectory);
bool UsesAnimatedUv(const std::wstring& materialTexturePath);
}
