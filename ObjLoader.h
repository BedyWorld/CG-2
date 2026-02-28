#pragma once
#include <Windows.h>        // UINT
#include <vector>
#include <string>
#include "Types.h"

// Result of loading an OBJ file
struct ObjResult
{
    std::vector<Vertex> vertices;    // flat vertex list
    std::vector<UINT>   indices;     // sequential 0,1,2,...,N-1
    std::wstring        texturePath; // first diffuse texture found (empty if none)
    bool                valid;

    ObjResult() : valid(false) {}
};

// Parse .obj + .mtl files.
// Vertex colors come from MTL Kd; tex-coords from vt entries.
// Returns ObjResult.valid == false if file not found or empty.
ObjResult LoadObj(const std::wstring& path);
