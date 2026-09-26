#pragma once
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

struct AABB
{
    XMFLOAT3 Min{ 0,0,0 };
    XMFLOAT3 Max{ 0,0,0 };

    XMFLOAT3 Center() const
    {
        return { (Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f, (Min.z + Max.z) * 0.5f };
    }
    XMFLOAT3 Extent() const
    {
        return { (Max.x - Min.x) * 0.5f, (Max.y - Min.y) * 0.5f, (Max.z - Min.z) * 0.5f };
    }
};

//  Плоскость ax + by + cz + d = 0, нормаль смотрит внутрь фрустума
struct Plane { float a = 0, b = 0, c = 0, d = 0; };

//  Пирамида видимости
struct Frustum
{
    Plane Planes[6];   // left, right, bottom, top, near, far

    static Frustum FromViewProj(const XMFLOAT4X4& vp);

    bool Intersects(const AABB& box) const;
    
    bool Contains(const AABB& box) const;
};

class Octree
{
public:
    void Build(const std::vector<AABB>& bounds, int maxDepth = 6, int minObjects = 8);

    void Query(const Frustum& frustum, std::vector<int>& outIndices) const;

    bool Empty()      const { return nodes_.empty(); }
    int  NodeCount()  const { return static_cast<int>(nodes_.size()); }
    int  MaxDepth()   const { return builtDepth_; }

    int  LastVisitedNodes() const { return lastVisited_; }

private:
    struct Node
    {
        AABB             bounds;
        int              child[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
        std::vector<int> objects;
        bool IsLeaf() const { return child[0] < 0; }
    };

    int  BuildNode(const AABB& box, std::vector<int>& items, int depth,
        int maxDepth, int minObjects);
    void QueryNode(int nodeIdx, const Frustum& frustum,
        std::vector<int>& out) const;
    void CollectSubtree(int nodeIdx, std::vector<int>& out) const;

    std::vector<Node> nodes_;
    std::vector<AABB> bounds_;
    int  builtDepth_ = 0;
    mutable int lastVisited_ = 0;
};
