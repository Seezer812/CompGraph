#include "SceneRenderer.h"

#include "D3DHelpers.h"
#include "ScenePaths.h"
#include "TextureUtil.h"

#include <cstring>
#include <algorithm>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kCbAlignment = 256;

struct alignas(256) FrameConstants
{
    XMFLOAT4X4 world;
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 timeCamera;
    XMFLOAT4 uvAnimation;
    float padding[24];
};

struct alignas(256) MaterialConstants
{
    XMFLOAT4 kd;
    XMFLOAT2 uvScale;
    XMFLOAT2 uvOffset;
    XMFLOAT3 ks;
    float ns;
    UINT useUvAnimation, hasSpecularMap, isEmissive, hasNormalMap, hasDisplacementMap, enableTessellation;
    float padding[46];
};

static_assert(sizeof(FrameConstants) == kCbAlignment);
static_assert(sizeof(MaterialConstants) == kCbAlignment);

bool IsTessellatedMaterial(const std::string& name)
{
    return name == "floor" || name == "bricks" || name == "arch" || name == "ceiling" ||
        name == "column_a" || name == "column_b" || name == "column_c" || name == "details" ||
        name == "roof" || name == "thorn";
}

bool IsBackgroundMaterial(const Obj::Material& material)
{
    // These three broad, non-architectural meshes in the source asset form its
    // photographed exterior backdrop. They are not part of the Sponza building.
    return material.name == "Material__25" || material.name == "Material__298" ||
        material.name == "Material__47";
}
} // namespace

bool SceneRenderer::Load(
    ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* uploadAllocator,
    ID3D12GraphicsCommandList* uploadCommands, ID3D12DescriptorHeap* srvHeap, UINT srvDescriptorSize,
    const std::filesystem::path& objPath)
{
    m_ready = false;
    m_textures.clear();
    m_materialSrvBase.clear();
    m_srvDescriptorSize = srvDescriptorSize;
    std::wstring error;
    if (!Obj::LoadObj(objPath, m_mesh, error) || m_mesh.vertices.empty() || m_mesh.indices.empty())
        return false;

    // Remove the imported photographic backdrop before any GPU buffers are built.
    // It cannot then reach either the colour or the shadow pass.
    std::erase_if(m_mesh.submeshes, [this](const Obj::Submesh& submesh) {
        return submesh.materialIndex < m_mesh.materials.size() &&
            IsBackgroundMaterial(m_mesh.materials[submesh.materialIndex]);
    });

    m_vertexBuffer = D3DHelpers::CreateUploadBuffer(device, m_mesh.vertices.data(), m_mesh.vertices.size() * sizeof(Obj::MeshVertex));
    m_indexBuffer = D3DHelpers::CreateUploadBuffer(device, m_mesh.indices.data(), m_mesh.indices.size() * sizeof(uint32_t));
    m_vertexView = {m_vertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(m_mesh.vertices.size() * sizeof(Obj::MeshVertex)), sizeof(Obj::MeshVertex)};
    m_indexView = {m_indexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(m_mesh.indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};

    if (FAILED(uploadAllocator->Reset()) || FAILED(uploadCommands->Reset(uploadAllocator, nullptr))) return false;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    ComPtr<ID3D12Resource> white, flatNormal;
    if (!Tex::CreateSolidTexture2D(device, uploadCommands, srvHeap, 0, srvDescriptorSize, 0xFFFFFFFFu, white, uploads)) return false;
    Tex::WriteTexture2DSrv(device, white.Get(), srvHeap, 1, srvDescriptorSize);
    if (!Tex::CreateSolidTexture2D(device, uploadCommands, srvHeap, 2, srvDescriptorSize, 0xFFFF8080u, flatNormal, uploads)) return false;
    m_textures.push_back(white); m_textures.push_back(flatNormal);

    std::unordered_map<std::wstring, ComPtr<ID3D12Resource>> cache;
    auto bind = [&](UINT slot, const std::filesystem::path& path, ID3D12Resource* fallback) {
        if (!std::filesystem::exists(path)) { Tex::WriteTexture2DSrv(device, fallback, srvHeap, slot, srvDescriptorSize); return false; }
        const std::wstring key = path.lexically_normal().wstring();
        if (auto found = cache.find(key); found != cache.end()) { Tex::WriteTexture2DSrv(device, found->second.Get(), srvHeap, slot, srvDescriptorSize); return true; }
        ComPtr<ID3D12Resource> texture; std::wstring loadError;
        if (!Tex::CreateTexture2DFromFile(device, uploadCommands, srvHeap, slot, srvDescriptorSize, path, texture, uploads, loadError)) {
            Tex::WriteTexture2DSrv(device, fallback, srvHeap, slot, srvDescriptorSize); return false;
        }
        cache.emplace(key, texture); m_textures.push_back(texture); return true;
    };

    const auto materialDir = objPath.parent_path();
    std::vector<uint8_t> spec(m_mesh.materials.size()), normal(m_mesh.materials.size()), displacement(m_mesh.materials.size());
    m_materialSrvBase.resize(m_mesh.materials.size());
    UINT slot = 3;
    for (size_t i = 0; i < m_mesh.materials.size(); ++i, slot += 4)
    {
        const auto& material = m_mesh.materials[i]; m_materialSrvBase[i] = slot;
        const auto diffuse = Tex::ResolveTexturePathInTexturesFolder(materialDir, material.diffuseMapRel);
        bind(slot, diffuse, white.Get());
        spec[i] = bind(slot + 1, Tex::ResolveTexturePathInTexturesFolder(materialDir, material.specularMapRel), white.Get());
        std::wstring stem = diffuse.stem().wstring();
        if (stem.ends_with(L"_diff")) stem.resize(stem.size() - 5); else if (stem.ends_with(L"_dif")) stem.resize(stem.size() - 4);
        normal[i] = bind(slot + 2, diffuse.parent_path() / (stem + L"_ddn.tga"), flatNormal.Get());
        displacement[i] = bind(slot + 3, diffuse.parent_path() / (diffuse.stem().wstring() + L"_displacement.tga"), white.Get());
    }
    if (FAILED(uploadCommands->Close()))
        return false;
    ID3D12CommandList* lists[] = {uploadCommands};
    queue->ExecuteCommandLists(1, lists);
    // До повторного использования upload allocator дожидаемся завершения загрузки.
    ComPtr<ID3D12Fence> uploadFence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence)))) return false;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) return false;
    if (FAILED(queue->Signal(uploadFence.Get(), 1)))
    {
        CloseHandle(eventHandle);
        return false;
    }
    if (uploadFence->GetCompletedValue() < 1)
    {
        if (FAILED(uploadFence->SetEventOnCompletion(1, eventHandle)) ||
            WaitForSingleObject(eventHandle, 15000) != WAIT_OBJECT_0)
        {
            CloseHandle(eventHandle);
            return false;
        }
    }
    CloseHandle(eventHandle);
    m_uploads = std::move(uploads);

    const UINT materialCount = (std::max)(1u, static_cast<UINT>(m_mesh.materials.size()));
    m_materialConstants = D3DHelpers::CreateUploadBuffer(device, nullptr, materialCount * kCbAlignment);
    m_frameConstants = D3DHelpers::CreateUploadBuffer(device, nullptr, kCbAlignment);
    m_shadowConstants = D3DHelpers::CreateUploadBuffer(device, nullptr, 4 * kCbAlignment);
    D3D12_RANGE range{0, 0}; m_frameConstants->Map(0, &range, reinterpret_cast<void**>(&m_frameConstantsMapped));
    m_shadowConstants->Map(0, &range, reinterpret_cast<void**>(&m_shadowConstantsMapped));
    uint8_t* mapped = nullptr; m_materialConstants->Map(0, &range, reinterpret_cast<void**>(&mapped));
    for (UINT i = 0; i < materialCount; ++i) {
        MaterialConstants constants{};
        if (i < m_mesh.materials.size()) { const auto& material = m_mesh.materials[i];
            constants.kd = XMFLOAT4(material.Kd[0], material.Kd[1], material.Kd[2], 1); constants.uvScale = XMFLOAT2(material.uvScale[0], material.uvScale[1]); constants.uvOffset = XMFLOAT2(material.uvOffset[0], material.uvOffset[1]);
            constants.ks = XMFLOAT3(material.Ks[0], material.Ks[1], material.Ks[2]); constants.ns = material.Ns;
            constants.useUvAnimation = ScenePaths::UsesAnimatedUv(material.diffuseMapRel); constants.hasSpecularMap = spec[i]; constants.hasNormalMap = normal[i]; constants.hasDisplacementMap = displacement[i]; constants.enableTessellation = displacement[i] && IsTessellatedMaterial(material.name);
        }
        std::memcpy(mapped + i * kCbAlignment, &constants, sizeof(constants));
    }
    m_materialConstants->Unmap(0, nullptr); m_ready = true; return true;
}

void SceneRenderer::Draw(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap, ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& viewProjection, const XMFLOAT3& cameraPosition, float timeSeconds, bool tessellationEnabled) const
{
    if (!m_ready) return;
    FrameConstants frame{}; XMStoreFloat4x4(&frame.world, XMMatrixScaling(.01f, .01f, .01f) * XMMatrixRotationX(XM_PI)); XMStoreFloat4x4(&frame.viewProjection, viewProjection); frame.timeCamera = XMFLOAT4(timeSeconds, cameraPosition.x, cameraPosition.y, cameraPosition.z); frame.uvAnimation = XMFLOAT4(.035f, .022f, tessellationEnabled ? 1.f : 0.f, 0.f); std::memcpy(m_frameConstantsMapped, &frame, sizeof(frame));
    ID3D12DescriptorHeap* heaps[] = {srvHeap}; commandList->SetDescriptorHeaps(1, heaps); commandList->SetGraphicsRootSignature(rootSignature); commandList->SetPipelineState(pipelineState); commandList->SetGraphicsRootConstantBufferView(0, m_frameConstants->GetGPUVirtualAddress()); commandList->IASetPrimitiveTopology(tessellationEnabled ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commandList->IASetVertexBuffers(0, 1, &m_vertexView); commandList->IASetIndexBuffer(&m_indexView);
    const auto base = srvHeap->GetGPUDescriptorHandleForHeapStart();
    for (const auto& submesh : m_mesh.submeshes) { if (submesh.materialIndex >= m_materialSrvBase.size() || IsBackgroundMaterial(m_mesh.materials[submesh.materialIndex])) continue; auto table = base; table.ptr += static_cast<SIZE_T>(m_materialSrvBase[submesh.materialIndex]) * m_srvDescriptorSize; commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress() + static_cast<UINT64>(submesh.materialIndex) * kCbAlignment); commandList->SetGraphicsRootDescriptorTable(2, table); commandList->DrawIndexedInstanced(submesh.indexCount, 1, submesh.indexStart, 0, 0); }
}

void SceneRenderer::DrawShadow(ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& lightViewProjection, UINT cascade) const
{
    if (!m_ready) return;
    struct ShadowConstants { XMFLOAT4X4 world; XMFLOAT4X4 lightViewProjection; } constants{};
    XMStoreFloat4x4(&constants.world, XMMatrixScaling(.01f, .01f, .01f) * XMMatrixRotationX(XM_PI));
    XMStoreFloat4x4(&constants.lightViewProjection, lightViewProjection);
    const UINT shadowSlot = cascade % 4;
    std::memcpy(m_shadowConstantsMapped + static_cast<size_t>(shadowSlot) * kCbAlignment, &constants, sizeof(constants));
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootConstantBufferView(0, m_shadowConstants->GetGPUVirtualAddress() + static_cast<UINT64>(shadowSlot) * kCbAlignment);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexView);
    commandList->IASetIndexBuffer(&m_indexView);
    for (const auto& submesh : m_mesh.submeshes)
    {
        if (submesh.materialIndex < m_mesh.materials.size() && IsBackgroundMaterial(m_mesh.materials[submesh.materialIndex]))
            continue;
        commandList->DrawIndexedInstanced(submesh.indexCount, 1, submesh.indexStart, 0, 0);
    }
}
