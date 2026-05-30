#include <Windows.h>
#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <map>
#include <tuple>

using namespace DirectX;

// ---- Material -------------------------------------------------------
struct Material
{
    XMFLOAT4     diffuse = { 1,1,1,1 };
    std::wstring diffusePath;   // map_Kd
    std::wstring normalPath;    // map_bump / bump / map_Kn
};

// ---- Helper: split "v/vt/vn" token ----------------------------------
static void ParseFaceVert(const std::string& token, int& p, int& t, int& n)
{
    p = t = n = 0;
    std::string parts[3]; int part = 0;
    for (char c : token)
    {
        if (c == '/') { if (++part >= 3) break; }
        else { parts[part] += c; }
    }
    if (!parts[0].empty()) p = std::stoi(parts[0]);
    if (!parts[1].empty()) t = std::stoi(parts[1]);
    if (!parts[2].empty()) n = std::stoi(parts[2]);
}

// ---- Trim helper ----------------------------------------------------
static std::string TrimLine(const std::string& s)
{
    std::string r = s;
    size_t st = r.find_first_not_of(" \t");
    if (st == std::string::npos) return {};
    r = r.substr(st);
    while (!r.empty() && (r.back() == '\r' || r.back() == ' ' || r.back() == '\t'))
        r.pop_back();
    return r;
}

// Конвертирует narrow строку пути в wide и нормализует слеши к '\\'
// Sponza MTL может хранить пути как "textures/file.tga", ".\textures\file.tga", etc.
static std::wstring MakeTexturePath(const std::wstring& dir, const std::string& filename)
{
    std::string fn = filename;
    // Убираем ведущие "./" и ".\"
    if (fn.size() >= 2 && fn[0] == '.' && (fn[1] == '/' || fn[1] == '\\'))
        fn = fn.substr(2);
    // narrow -> wide
    std::wstring wname(fn.begin(), fn.end());
    // Нормализуем '/' -> '\\'
    for (auto& c : wname) if (c == L'/') c = L'\\';
    return dir + wname;
}

// ---- Load MTL -------------------------------------------------------
static std::map<std::string, Material>
LoadMtl(const std::wstring& mtlPath, const std::wstring& dir)
{
    std::map<std::string, Material> mats;
    std::ifstream f(mtlPath);
    if (!f) return mats;

    Material* cur = nullptr;
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string token; ss >> token;

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
            std::string rest; std::getline(ss, rest);
            rest = TrimLine(rest);
            if (!rest.empty())
                cur->diffusePath = MakeTexturePath(dir, rest);
        }
        // normal map — Sponza использует map_bump и bump
        else if (cur && (token == "map_bump" || token == "bump" || token == "map_Kn" || token == "norm"))
        {
            std::string rest; std::getline(ss, rest);
            rest = TrimLine(rest);
            // Пропускаем флаги типа "-bm 1.0" или "-bv 1.0"
            if (!rest.empty() && rest[0] == '-')
            {
                // найдём следующий токен после флагов
                std::istringstream rs(rest);
                std::string flag;
                while (rs >> flag)
                {
                    if (flag[0] == '-') { std::string val; rs >> val; } // пропускаем -flag value
                    else { rest = flag; break; }
                }
            }
            if (!rest.empty() && rest[0] != '-')
            {
                // Берём последний "слой" — имя файла после флагов
                // Ищем последний токен не начинающийся с '-'
                std::istringstream rs2(rest);
                std::string tok, last;
                while (rs2 >> tok)
                {
                    if (tok[0] == '-') { std::string skip; rs2 >> skip; }
                    else               last = tok;
                }
                if (!last.empty())
                    cur->normalPath = MakeTexturePath(dir, last);
            }
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
    std::string curMatName;

    // Дедупликация вершин
    std::map<std::tuple<int, int, int>, UINT> vertCache;

    auto getVertex = [&](int pi, int ti, int ni) -> UINT
        {
            if (pi < 0) pi = (int)positions.size() + pi + 1;
            if (ti < 0) ti = (int)texcoords.size() + ti + 1;
            if (ni < 0) ni = (int)normals.size() + ni + 1;
            auto key = std::make_tuple(pi, ti, ni);
            auto it = vertCache.find(key);
            if (it != vertCache.end()) return it->second;

            Vertex v = {};
            v.Normal = XMFLOAT3(0, 1, 0);
            v.Color = curMat.diffuse;
            if (pi > 0 && pi <= (int)positions.size()) v.Position = positions[pi - 1];
            if (ni > 0 && ni <= (int)normals.size())   v.Normal = normals[ni - 1];
            if (ti > 0 && ti <= (int)texcoords.size()) v.TexCoord = texcoords[ti - 1];
            UINT idx = static_cast<UINT>(out.vertices.size());
            out.vertices.push_back(v);
            vertCache[key] = idx;
            return idx;
        };

    // Текущий SubMesh (открывается при первом usemtl / первом face)
    SubMesh curSub;
    curSub.indexStart = 0;
    bool subOpen = false;

    auto FlushSubMesh = [&]()
        {
            if (!subOpen) return;
            curSub.indexCount = static_cast<UINT>(out.indices.size()) - curSub.indexStart;
            if (curSub.indexCount > 0)
                out.subMeshes.push_back(curSub);
            subOpen = false;
        };

    auto OpenSubMesh = [&](const std::string& matName)
        {
            FlushSubMesh();
            curSub = {};
            curSub.indexStart = static_cast<UINT>(out.indices.size());
            curSub.materialName = matName;
            curSub.diffusePath = curMat.diffusePath;
            curSub.normalPath = curMat.normalPath;
            subOpen = true;
            // При смене материала сбрасываем кэш вершин
            vertCache.clear();
        };

    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string token; ss >> token;

        if (token == "v")
        {
            float x, y, z; ss >> x >> y >> z;
            positions.push_back({ x,y,z });
        }
        else if (token == "vt")
        {
            float u = 0, v = 0; ss >> u >> v;
            texcoords.push_back({ u, 1.0f - v }); // flip V для D3D
        }
        else if (token == "vn")
        {
            float x, y, z; ss >> x >> y >> z;
            normals.push_back({ x,y,z });
        }
        else if (token == "mtllib")
        {
            std::string rest; std::getline(ss, rest);
            rest = TrimLine(rest);
            if (!rest.empty())
            {
                std::wstring mtlPath = dir + std::wstring(rest.begin(), rest.end());
                materials = LoadMtl(mtlPath, dir);
            }
        }
        else if (token == "usemtl")
        {
            std::string name; ss >> name;
            curMatName = name;
            if (materials.count(name)) curMat = materials[name];
            else                       curMat = {};
            OpenSubMesh(name);
            // Первая диффузная — для legacy поля
            if (out.texturePath.empty() && !curMat.diffusePath.empty())
                out.texturePath = curMat.diffusePath;
        }
        else if (token == "f")
        {
            // Открываем субмеш если ещё не открыт (файлы без usemtl)
            if (!subOpen) OpenSubMesh("");

            std::vector<UINT> fv;
            std::string vert;
            while (ss >> vert)
            {
                int p = 0, t = 0, n = 0;
                ParseFaceVert(vert, p, t, n);
                fv.push_back(getVertex(p, t, n));
            }
            // Fan-триангуляция
            for (int i = 1; i + 1 < (int)fv.size(); ++i)
            {
                out.indices.push_back(fv[0]);
                out.indices.push_back(fv[i]);
                out.indices.push_back(fv[i + 1]);
            }
        }
    }

    FlushSubMesh();

    if (out.vertices.empty()) return out;

    // Сглаженные нормали если OBJ не содержал vn
    if (normals.empty())
    {
        for (auto& v : out.vertices) v.Normal = {};
        for (UINT i = 0; i + 2 < (UINT)out.indices.size(); i += 3)
        {
            auto& v0 = out.vertices[out.indices[i]];
            auto& v1 = out.vertices[out.indices[i + 1]];
            auto& v2 = out.vertices[out.indices[i + 2]];
            XMVECTOR p0 = XMLoadFloat3(&v0.Position);
            XMVECTOR fn = XMVector3Cross(
                XMLoadFloat3(&v1.Position) - p0,
                XMLoadFloat3(&v2.Position) - p0);
            XMFLOAT3 a;
            XMStoreFloat3(&a, XMLoadFloat3(&v0.Normal) + fn); v0.Normal = a;
            XMStoreFloat3(&a, XMLoadFloat3(&v1.Normal) + fn); v1.Normal = a;
            XMStoreFloat3(&a, XMLoadFloat3(&v2.Normal) + fn); v2.Normal = a;
        }
        for (auto& v : out.vertices)
        {
            XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.Normal)); XMStoreFloat3(&v.Normal, n);
        }
    }

    out.valid = true;
    return out;
}