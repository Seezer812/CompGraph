#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Obj
{

struct MeshVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct Submesh
{
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
};

struct Material
{
    std::string name;
    std::wstring diffuseMapRel;
    std::wstring specularMapRel;
    float Kd[3]{1.0f, 1.0f, 1.0f};
    float Ks[3]{0.15f, 0.15f, 0.15f};
    float Ns{32.0f};
    float uvScale[2]{1.0f, 1.0f};
    float uvOffset[2]{0.0f, 0.0f};
};

struct LoadedMesh
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Submesh> submeshes;
    std::vector<Material> materials;
};

bool LoadObj(const std::filesystem::path& objPath, LoadedMesh& out, std::wstring& error);

}
