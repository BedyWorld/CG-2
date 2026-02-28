#include <Windows.h>        // UINT  -- must be first
#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <map>

using namespace DirectX;

// ---- Material -------------------------------------------------------
struct Material
{
    XMFLOAT4     diffuse;
    std::wstring texturePath;
    Material() : diffuse(1.0f, 1.0f, 1.0f, 1.0f) {}
};

// ---- Helper: split "v/vt/vn" token into component indices -----------
static void ParseFaceVert(const std::string& token,
                           int& p, int& t, int& n)
{
    p = t = n = 0;
    std::vector<std::string> parts;
    std::string cur;
    for (size_t k = 0; k <= token.size(); ++k)
    {
        if (k == token.size() || token[k] == '/')
        {
            parts.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += token[k];
        }
    }
    if (parts.size() > 0 && !parts[0].empty()) p = std::stoi(parts[0]);
    if (parts.size() > 1 && !parts[1].empty()) t = std::stoi(parts[1]);
    if (parts.size() > 2 && !parts[2].empty()) n = std::stoi(parts[2]);
}

// ---- Load MTL file --------------------------------------------------
static std::map<std::string, Material>
LoadMtl(const std::wstring& mtlPath, const std::wstring& dir)
{
    std::map<std::string, Material> mats;
    std::ifstream f(mtlPath);
    if (!f) return mats;

    Material*   cur = NULL;
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "newmtl")
        {
            std::string name; ss >> name;
            mats[name] = Material();
            cur = &mats[name];
        }
        else if (cur && token == "Kd")
        {
            float r, g, b; ss >> r >> g >> b;
            cur->diffuse = XMFLOAT4(r, g, b, 1.0f);
        }
        else if (cur && token == "map_Kd")
        {
            std::string texName; ss >> texName;
            cur->texturePath = dir + std::wstring(texName.begin(), texName.end());
        }
    }
    return mats;
}

// ---- Main OBJ loader ------------------------------------------------
ObjResult LoadObj(const std::wstring& path)
{
    ObjResult out;
    std::ifstream f(path);
    if (!f) return out;

    // Extract directory for relative path resolution
    std::wstring dir;
    {
        size_t sep = path.find_last_of(L"\\/");
        if (sep != std::wstring::npos) dir = path.substr(0, sep + 1);
    }

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;

    std::map<std::string, Material> materials;
    Material curMat;

    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string token;
        ss >> token;

        // --- Positions ---
        if (token == "v")
        {
            float x, y, z; ss >> x >> y >> z;
            positions.push_back(XMFLOAT3(x, y, z));
        }
        // --- Texture coords ---
        else if (token == "vt")
        {
            float u, v; ss >> u >> v;
            texcoords.push_back(XMFLOAT2(u, 1.0f - v)); // flip V for D3D
        }
        // --- Normals ---
        else if (token == "vn")
        {
            float x, y, z; ss >> x >> y >> z;
            normals.push_back(XMFLOAT3(x, y, z));
        }
        // --- Material library ---
        else if (token == "mtllib")
        {
            std::string name; ss >> name;
            std::wstring mtlPath = dir + std::wstring(name.begin(), name.end());
            materials = LoadMtl(mtlPath, dir);
        }
        // --- Use material ---
        else if (token == "usemtl")
        {
            std::string name; ss >> name;
            if (materials.count(name))
            {
                curMat = materials[name];
                if (out.texturePath.empty() && !curMat.texturePath.empty())
                    out.texturePath = curMat.texturePath;
            }
        }
        // --- Faces ---
        else if (token == "f")
        {
            std::vector<int> pIdx, tIdx, nIdx;
            std::string vert;
            while (ss >> vert)
            {
                int p = 0, t = 0, n = 0;
                ParseFaceVert(vert, p, t, n);

                // Handle negative (relative) indices
                if (p < 0) p = (int)positions.size() + p + 1;
                if (t < 0) t = (int)texcoords.size() + t + 1;
                if (n < 0) n = (int)normals.size()   + n + 1;

                pIdx.push_back(p);
                tIdx.push_back(t);
                nIdx.push_back(n);
            }

            // Triangulate as a fan (works for convex polygons)
            for (int i = 1; i + 1 < (int)pIdx.size(); ++i)
            {
                // Replaced range-for with initializer list by explicit loop
                int fan[3] = { 0, i, i + 1 };
                for (int fi = 0; fi < 3; ++fi)
                {
                    int j = fan[fi];
                    Vertex v;
                    v.Position = XMFLOAT3(0, 0, 0);
                    v.Normal   = XMFLOAT3(0, 1, 0);
                    v.Color    = curMat.diffuse;
                    v.TexCoord = XMFLOAT2(0, 0);

                    int pi = pIdx[j];
                    if (pi > 0 && pi <= (int)positions.size())
                        v.Position = positions[pi - 1];

                    int ni = nIdx[j];
                    if (ni > 0 && ni <= (int)normals.size())
                        v.Normal = normals[ni - 1];

                    int ti = tIdx[j];
                    if (ti > 0 && ti <= (int)texcoords.size())
                        v.TexCoord = texcoords[ti - 1];

                    out.indices.push_back((UINT)out.vertices.size());
                    out.vertices.push_back(v);
                }
            }
        }
    }

    if (out.vertices.empty()) return out;

    // --- Compute flat normals if OBJ had none ---
    if (normals.empty())
    {
        for (UINT i = 0; i + 2 < (UINT)out.indices.size(); i += 3)
        {
            Vertex& v0 = out.vertices[out.indices[i]];
            Vertex& v1 = out.vertices[out.indices[i + 1]];
            Vertex& v2 = out.vertices[out.indices[i + 2]];

            XMVECTOR p0 = XMLoadFloat3(&v0.Position);
            XMVECTOR p1 = XMLoadFloat3(&v1.Position);
            XMVECTOR p2 = XMLoadFloat3(&v2.Position);
            XMVECTOR fn = XMVector3Normalize(
                XMVector3Cross(p1 - p0, p2 - p0));
            XMFLOAT3 nf;
            XMStoreFloat3(&nf, fn);
            v0.Normal = nf;
            v1.Normal = nf;
            v2.Normal = nf;
        }
    }

    out.valid = true;
    return out;
}
