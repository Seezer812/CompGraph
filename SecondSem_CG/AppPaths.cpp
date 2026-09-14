#include "AppPaths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace AppPaths
{
std::wstring ExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    if (const size_t slash = directory.find_last_of(L"\\/"); slash != std::wstring::npos)
        directory.resize(slash + 1);
    return directory;
}

std::wstring DeferredShaderFile()
{
    return ExecutableDirectory() + L"Deferred.hlsl";
}
} // namespace AppPaths
