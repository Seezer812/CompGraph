#include "ScenePaths.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace ScenePaths
{
namespace
{
std::filesystem::path FindAsset(const std::wstring& executableDirectory,
    const std::vector<std::filesystem::path>& relatives)
{
    std::vector<std::filesystem::path> roots{std::filesystem::path(executableDirectory)};
    try { roots.push_back(std::filesystem::current_path()); } catch (...) {}
    const size_t originalCount = roots.size();
    for (size_t i = 0; i < originalCount; ++i)
        for (auto path = roots[i]; !path.empty();)
        {
            if (std::find(roots.begin(), roots.end(), path) == roots.end()) roots.push_back(path);
            const auto parent = path.parent_path();
            if (parent == path) break;
            path = parent;
        }
    for (const auto& root : roots)
        for (const auto& relative : relatives)
            if (const auto candidate = root / relative; std::filesystem::exists(candidate)) return candidate;
    return {};
}
}

std::filesystem::path FindCerberusObj(const std::wstring& executableDirectory)
{
    return FindAsset(executableDirectory, {
        L"assets/models/cerberus/Cerberus.obj",
        L"models/cerberus/Cerberus.obj"});
}

std::filesystem::path FindSponzaObj(const std::wstring& executableDirectory)
{
    return FindAsset(executableDirectory, {L"Sponza/sponza.obj", L"sponza.obj"});
}

bool UsesAnimatedUv(const std::wstring& materialTexturePath)
{
    std::wstring value = materialTexturePath;
    for (wchar_t& c : value) c = static_cast<wchar_t>(towlower(c));
    const wchar_t* keys[] = {L"fabric", L"curtain", L"banner", L"water", L"flag", L"vines", L"leaf", L"drape"};
    for (const wchar_t* key : keys)
        if (value.find(key) != std::wstring::npos) return true;
    return false;
}
} // namespace ScenePaths
