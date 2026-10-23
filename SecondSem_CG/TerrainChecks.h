#pragma once
#include "D3D12Context.h"
#include <filesystem>
#include <string>
#include <vector>

namespace TerrainChecks
{
bool Assets(const std::filesystem::path& directory, std::wstring& error);
bool Capture(D3D12Context& context, UINT frame, UINT width, UINT height,
    std::vector<uint8_t>& rgba, std::wstring& error);
void SavePpm(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgba);
UINT GpuErrors(ID3D12Device* device);
}
