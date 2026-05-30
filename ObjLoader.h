#pragma once
#include <Windows.h>        // UINT
#include <vector>
#include <string>
#include "Types.h"

// ============================================================
//  SubMesh — диапазон индексов с одним материалом
//  diffusePath  = map_Kd  (albedo/diffuse текстура)
//  normalPath   = map_bump / bump / map_Kn (normal map, DDN)
// ============================================================
struct SubMesh
{
    UINT         indexStart  = 0;
    UINT         indexCount  = 0;
    std::wstring diffusePath;   // может быть пустой
    std::wstring normalPath;    // может быть пустой
    std::string  materialName;
};

// ============================================================
//  ObjResult
// ============================================================
struct ObjResult
{
    std::vector<Vertex>  vertices;
    std::vector<UINT>    indices;
    std::vector<SubMesh> subMeshes;   // per-material группы
    std::wstring         texturePath; // первая диффузная (legacy)
    bool                 valid = false;
};

// Parse .obj + .mtl files.
// Vertex colors come from MTL Kd; tex-coords from vt entries.
// Returns ObjResult.valid == false if file not found or empty.
ObjResult LoadObj(const std::wstring& path);
