#include "SpatialCulling.h"

#include <random>

using namespace DirectX;
namespace { constexpr uint32_t kNodeCapacity = 20, kMaxDepth = 6; }

void SpatialCulling::Generate(uint32_t count, uint32_t seed)
{
    m_objects.clear(); m_objects.reserve(count); std::mt19937 rng(seed);
    std::uniform_real_distribution<float> xz(-32.0f, 32.0f), y(0.15f, 7.0f), radius(0.09f, 0.22f);
    for (uint32_t i = 0; i < count; ++i) m_objects.push_back({XMFLOAT3{xz(rng), y(rng), xz(rng)}, radius(rng)});
    Build();
}

std::vector<uint32_t> SpatialCulling::FindVisible(const XMMATRIX& vp, bool frustum, bool octree, Stats& stats) const
{
    stats = {}; std::vector<uint32_t> result; result.reserve(m_objects.size());
    if (!frustum) { for (uint32_t i = 0; i < m_objects.size(); ++i) result.push_back(i); stats.visibleObjects = (uint32_t)result.size(); return result; }
    if (octree && !m_nodes.empty()) Visit(0, vp, result, stats);
    else for (uint32_t i = 0; i < m_objects.size(); ++i) { ++stats.objectTests; if (!OutsideFrustum(ObjectBounds(m_objects[i]), vp)) result.push_back(i); }
    stats.visibleObjects = (uint32_t)result.size(); return result;
}

SpatialCulling::Bounds SpatialCulling::ObjectBounds(const Object& o) { const float r=o.radius; return {{o.position.x-r,o.position.y-r,o.position.z-r},{o.position.x+r,o.position.y+r,o.position.z+r}}; }
bool SpatialCulling::OutsideFrustum(const Bounds& b, const XMMATRIX& vp)
{
    XMVECTOR p[8]; int n=0; for(float z:{b.min.z,b.max.z}) for(float y:{b.min.y,b.max.y}) for(float x:{b.min.x,b.max.x}) p[n++]=XMVector4Transform(XMVectorSet(x,y,z,1),vp);
    auto all=[&](auto pred){for(XMVECTOR q:p)if(!pred(q))return false;return true;};
    return all([](XMVECTOR q){return XMVectorGetX(q)<-XMVectorGetW(q);}) || all([](XMVECTOR q){return XMVectorGetX(q)>XMVectorGetW(q);}) ||
           all([](XMVECTOR q){return XMVectorGetY(q)<-XMVectorGetW(q);}) || all([](XMVECTOR q){return XMVectorGetY(q)>XMVectorGetW(q);}) ||
           all([](XMVECTOR q){return XMVectorGetZ(q)<0;}) || all([](XMVECTOR q){return XMVectorGetZ(q)>XMVectorGetW(q);});
}
bool SpatialCulling::Contains(const Bounds& a,const Bounds& b) { return b.min.x>=a.min.x&&b.max.x<=a.max.x&&b.min.y>=a.min.y&&b.max.y<=a.max.y&&b.min.z>=a.min.z&&b.max.z<=a.max.z; }
void SpatialCulling::Build() { m_nodes.clear();m_nodes.emplace_back();m_nodes[0].bounds={{-33,-1,-33},{33,9,33}};for(uint32_t i=0;i<m_objects.size();++i)Insert(i,0,0); }
void SpatialCulling::Split(int index)
{
    const Bounds p=m_nodes[index].bounds; const XMFLOAT3 c{(p.min.x+p.max.x)*.5f,(p.min.y+p.max.y)*.5f,(p.min.z+p.max.z)*.5f};
    for(int i=0;i<8;++i){const bool x=i&1,y=i&2,z=i&4;Node child;child.bounds.min={x?c.x:p.min.x,y?c.y:p.min.y,z?c.z:p.min.z};child.bounds.max={x?p.max.x:c.x,y?p.max.y:c.y,z?p.max.z:c.z};m_nodes[index].children[i]=(int)m_nodes.size();m_nodes.push_back(std::move(child));}
}
void SpatialCulling::Insert(uint32_t objectIndex,int nodeIndex,uint32_t depth)
{
    Node& node=m_nodes[nodeIndex]; if(node.children[0]!=-1){const Bounds b=ObjectBounds(m_objects[objectIndex]);for(int child:node.children)if(Contains(m_nodes[child].bounds,b)){Insert(objectIndex,child,depth+1);return;}node.objects.push_back(objectIndex);return;}
    node.objects.push_back(objectIndex);if(node.objects.size()<=kNodeCapacity||depth>=kMaxDepth)return;std::vector<uint32_t> old=std::move(node.objects);node.objects.clear();Split(nodeIndex);for(uint32_t i:old)Insert(i,nodeIndex,depth);
}
void SpatialCulling::Visit(int nodeIndex,const XMMATRIX& vp,std::vector<uint32_t>& out,Stats& stats) const
{
    const Node& node=m_nodes[nodeIndex];++stats.nodeTests;if(OutsideFrustum(node.bounds,vp))return;for(uint32_t i:node.objects){++stats.objectTests;if(!OutsideFrustum(ObjectBounds(m_objects[i]),vp))out.push_back(i);}for(int child:node.children)if(child!=-1)Visit(child,vp,out,stats);
}
