#include "Culling.h"
#include <cmath>
#include <algorithm>

// ============================================================
//  Извлечение плоскостей фрустума
//
//  Точка внутри фрустума удовлетворяет -w < x' < w и т.д., где
//  (x',y',z',w') = v * VP. Расписав эти неравенства покомпонентно,
//  получаем плоскости как комбинации столбцов VP. Для DirectX
//  ближняя плоскость даёт 0 < z' (диапазон глубины [0,1]),
//  а не -w < z' как в OpenGL — отсюда несимметричность near/far.
// ============================================================
Frustum Frustum::FromViewProj(const XMFLOAT4X4& m)
{
    Frustum f;

    // столбец i матрицы: m(0,i), m(1,i), m(2,i), m(3,i)
    auto col = [&m](int i, int j) { return m.m[i][j]; };

    // left: w' + x' > 0
    f.Planes[0] = { col(0,3) + col(0,0), col(1,3) + col(1,0),
                    col(2,3) + col(2,0), col(3,3) + col(3,0) };
    // right: w' - x' > 0
    f.Planes[1] = { col(0,3) - col(0,0), col(1,3) - col(1,0),
                    col(2,3) - col(2,0), col(3,3) - col(3,0) };
    // bottom: w' + y' > 0
    f.Planes[2] = { col(0,3) + col(0,1), col(1,3) + col(1,1),
                    col(2,3) + col(2,1), col(3,3) + col(3,1) };
    // top: w' - y' > 0
    f.Planes[3] = { col(0,3) - col(0,1), col(1,3) - col(1,1),
                    col(2,3) - col(2,1), col(3,3) - col(3,1) };
    // near: z' > 0   (диапазон глубины D3D — [0,1])
    f.Planes[4] = { col(0,2), col(1,2), col(2,2), col(3,2) };
    // far: w' - z' > 0
    f.Planes[5] = { col(0,3) - col(0,2), col(1,3) - col(1,2),
                    col(2,3) - col(2,2), col(3,3) - col(3,2) };

    // Нормализация: без неё расстояние до плоскости масштабировано,
    // и тест "центр ± радиус" перестаёт быть корректным.
    for (int i = 0; i < 6; ++i)
    {
        Plane& p = f.Planes[i];
        const float len = sqrtf(p.a * p.a + p.b * p.b + p.c * p.c);
        if (len > 1e-8f)
        {
            const float inv = 1.0f / len;
            p.a *= inv; p.b *= inv; p.c *= inv; p.d *= inv;
        }
    }
    return f;
}

// Знаковое расстояние от центра коробки до плоскости и проекция
// полуразмера на нормаль плоскости.
static inline void PlaneDistances(const Plane& p, const AABB& box,
    float& dist, float& radius)
{
    const XMFLOAT3 c = box.Center();
    const XMFLOAT3 e = box.Extent();
    dist = p.a * c.x + p.b * c.y + p.c * c.z + p.d;
    radius = fabsf(p.a) * e.x + fabsf(p.b) * e.y + fabsf(p.c) * e.z;
}

// Допуск на погрешность float. На дальней плоскости при дальности ~100
// единиц расхождение достигает единиц миллиметров — без запаса объекты
// на самой границе начинают мигать между кадрами. Смещаем в сторону
// "лучше нарисовать лишнее, чем пропустить".
static const float kCullEpsilon = 1e-2f;

bool Frustum::Intersects(const AABB& box) const
{
    for (int i = 0; i < 6; ++i)
    {
        float dist, radius;
        PlaneDistances(Planes[i], box, dist, radius);
        // Коробка целиком по отрицательную сторону хотя бы одной плоскости
        if (dist + radius < -kCullEpsilon) return false;
    }
    return true;
}

bool Frustum::Contains(const AABB& box) const
{
    for (int i = 0; i < 6; ++i)
    {
        float dist, radius;
        PlaneDistances(Planes[i], box, dist, radius);
        // Здесь запас в обратную сторону: сомнительный узел лучше разобрать
        // по объектам, чем целиком принять как видимый.
        if (dist - radius < kCullEpsilon) return false;   // пересекает плоскость
    }
    return true;
}

// ============================================================
//  Октодерево
// ============================================================
void Octree::Build(const std::vector<AABB>& bounds, int maxDepth, int minObjects)
{
    nodes_.clear();
    bounds_ = bounds;
    builtDepth_ = 0;
    lastVisited_ = 0;
    if (bounds_.empty()) return;

    // Корневой объём — общий AABB сцены
    AABB root = bounds_[0];
    for (const AABB& b : bounds_)
    {
        root.Min.x = std::min(root.Min.x, b.Min.x);
        root.Min.y = std::min(root.Min.y, b.Min.y);
        root.Min.z = std::min(root.Min.z, b.Min.z);
        root.Max.x = std::max(root.Max.x, b.Max.x);
        root.Max.y = std::max(root.Max.y, b.Max.y);
        root.Max.z = std::max(root.Max.z, b.Max.z);
    }
    // Небольшой запас, чтобы объекты на самой границе гарантированно
    // считались содержащимися
    const float pad = 1e-3f;
    root.Min.x -= pad; root.Min.y -= pad; root.Min.z -= pad;
    root.Max.x += pad; root.Max.y += pad; root.Max.z += pad;

    std::vector<int> all(bounds_.size());
    for (size_t i = 0; i < bounds_.size(); ++i) all[i] = static_cast<int>(i);

    nodes_.reserve(bounds_.size());
    BuildNode(root, all, 0, maxDepth, minObjects);
}

int Octree::BuildNode(const AABB& box, std::vector<int>& items, int depth,
    int maxDepth, int minObjects)
{
    const int idx = static_cast<int>(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[idx].bounds = box;
    if (depth > builtDepth_) builtDepth_ = depth;

    if (depth >= maxDepth || static_cast<int>(items.size()) <= minObjects)
    {
        nodes_[idx].objects = items;
        return idx;
    }

    const XMFLOAT3 c = box.Center();

    // Восемь дочерних объёмов
    AABB childBox[8];
    for (int i = 0; i < 8; ++i)
    {
        childBox[i].Min.x = (i & 1) ? c.x : box.Min.x;
        childBox[i].Max.x = (i & 1) ? box.Max.x : c.x;
        childBox[i].Min.y = (i & 2) ? c.y : box.Min.y;
        childBox[i].Max.y = (i & 2) ? box.Max.y : c.y;
        childBox[i].Min.z = (i & 4) ? c.z : box.Min.z;
        childBox[i].Max.z = (i & 4) ? box.Max.z : c.z;
    }

    // Объект уходит вниз только если ЦЕЛИКОМ помещается в один октант.
    // Пересекающие границу остаются здесь — так индекс встречается
    // ровно один раз во всём дереве.
    std::vector<int> childItems[8];
    std::vector<int> stay;
    for (int objIdx : items)
    {
        const AABB& b = bounds_[objIdx];
        int placed = -1;
        for (int k = 0; k < 8; ++k)
        {
            const AABB& cb = childBox[k];
            if (b.Min.x >= cb.Min.x && b.Max.x <= cb.Max.x &&
                b.Min.y >= cb.Min.y && b.Max.y <= cb.Max.y &&
                b.Min.z >= cb.Min.z && b.Max.z <= cb.Max.z)
            {
                placed = k;
                break;
            }
        }
        if (placed >= 0) childItems[placed].push_back(objIdx);
        else             stay.push_back(objIdx);
    }

    // Если разбиение ничего не дало — оставляем лист, иначе уйдём в
    // бесконечную рекурсию на совпадающих объектах.
    bool anyChild = false;
    for (int k = 0; k < 8; ++k) if (!childItems[k].empty()) { anyChild = true; break; }
    if (!anyChild)
    {
        nodes_[idx].objects = items;
        return idx;
    }

    nodes_[idx].objects = stay;
    for (int k = 0; k < 8; ++k)
    {
        if (childItems[k].empty()) continue;
        const int childIdx = BuildNode(childBox[k], childItems[k], depth + 1,
            maxDepth, minObjects);
        // ВАЖНО: nodes_ мог перевыделиться внутри рекурсии — пишем по индексу
        nodes_[idx].child[k] = childIdx;
    }
    return idx;
}

void Octree::Query(const Frustum& frustum, std::vector<int>& out) const
{
    out.clear();
    lastVisited_ = 0;
    if (nodes_.empty()) return;
    QueryNode(0, frustum, out);
}

void Octree::QueryNode(int nodeIdx, const Frustum& frustum, std::vector<int>& out) const
{
    const Node& node = nodes_[nodeIdx];
    ++lastVisited_;

    if (!frustum.Intersects(node.bounds)) return;   // всё поддерево снаружи

    if (frustum.Contains(node.bounds))
    {
        // Узел целиком внутри — принимаем поддерево без проверок плоскостей
        CollectSubtree(nodeIdx, out);
        return;
    }

    for (int objIdx : node.objects)
        if (frustum.Intersects(bounds_[objIdx]))
            out.push_back(objIdx);

    for (int k = 0; k < 8; ++k)
        if (node.child[k] >= 0)
            QueryNode(node.child[k], frustum, out);
}

void Octree::CollectSubtree(int nodeIdx, std::vector<int>& out) const
{
    const Node& node = nodes_[nodeIdx];
    out.insert(out.end(), node.objects.begin(), node.objects.end());
    for (int k = 0; k < 8; ++k)
        if (node.child[k] >= 0)
            CollectSubtree(node.child[k], out);
}
