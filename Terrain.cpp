#include "Terrain.h"
#include <cstdio>
#include <cmath>
#include <windows.h>

static const float kHeightScale = Terrain::HeightScale();
static const float kSplitFactor = 2.0f;
static const float kOriginY = Terrain::OriginY();
//баянчики2
float Terrain::lodBoost_ = 0.0f;

float Terrain::LevelHeightScale(int level) const
{
	if (maxLevel_ <= 0) return kHeightScale;
	float k = (float)(maxLevel_ - level) / (float)maxLevel_;
	return kHeightScale * (1.0f + lodBoost_ * k);
}

void Terrain::BuildFake(int size)
{
	mapSize_ = size;
	heights_.resize(size * size);
	
	for (int z = 0; z < size; ++z)
		{
			for(int x = 0; x < size; ++x)
			{
				float h = 0.5f + 0.5f * sinf(x * 0.02f) * cosf(z * 0.02f);
				heights_[z * size + x] = h;
			}
		}
}

bool Terrain::LoadRaw16(const char* path)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || f == nullptr) return false;

	fseek(f, 0, SEEK_END);
	long bytes = ftell(f);
	fseek(f, 0, SEEK_SET);

	int count = (int)(bytes / 2);
	int side = (int)(sqrtf((float)count) + 0.5f);
	int cells = side - 1;
	int q = cells / LEAF_SIZE;
	bool ok = (side * side == count) && (cells % LEAF_SIZE == 0)
		&& q > 0 && ((q & (q - 1)) == 0);
	if (!ok)
	{
		fclose(f);
		return false;
	}

	std::vector<unsigned short> raw(count);
	size_t got = fread(raw.data(), 2, count, f);
	fclose(f);
	if ((int)got != count) return false;

	mapSize_ = side;
	heights_.resize(count);
	for (int i = 0; i < count; ++i)
		heights_[i] = raw[i] / 65535.0f;
	return true;
}

void Terrain::BuildQuadtree()
{
	nodes_.clear();
	maxLevel_ = 0;
	BuildNode(0, 0, mapSize_ - 1, 0);
}
int Terrain::BuildNode(int x, int z, int size, int level)
{
	TerrainNode node;
	node.x = x;
	node.z = z;
	node.size = size;
	node.level = level;

	float lo = 1e9f;
	float hi = -1e9f;
	for (int zz = z; zz <= z + size; ++zz)
	{
		for (int xx = x; xx <= x + size; ++xx)
		{
			float h = heights_[zz * mapSize_ + xx];
			if (h < lo) lo = h;
			if (h > hi) hi = h;
		}
	}
	node.minH = lo;
	node.maxH = hi;

	if (level > maxLevel_) maxLevel_ = level;

	int myIndex = (int)nodes_.size();
	nodes_.push_back(node);

	if (size > LEAF_SIZE)
	{
		int half = size / 2;
		int c0 = BuildNode(x, z, half, level + 1);
		int c1 = BuildNode(x + half, z, half, level + 1);
		int c2 = BuildNode(x, z + half, half, level + 1);
		int c3 = BuildNode(x + half, z + half, half, level + 1);
		nodes_[myIndex].child[0] = c0;
		nodes_[myIndex].child[1] = c1;
		nodes_[myIndex].child[2] = c2;
		nodes_[myIndex].child[3] = c3;
	}

	return myIndex;
}
void Terrain::PrintStats() const
{
	char buf[256];
	sprintf_s(buf, "terrain: map %d, cells %d\n", mapSize_, (int)heights_.size());
	OutputDebugStringA(buf);

	int counts[16] = {};
	for (size_t i = 0; i < nodes_.size(); ++i)
	{
		int lv = nodes_[i].level;
		if (lv < 16) counts[lv]++;
	}
	for (int lv = 0; lv < 16; ++lv)
	{
		if (counts[lv] > 0)
		{
			sprintf_s(buf, "  level %d: %d nodes\n", lv, counts[lv]);
			OutputDebugStringA(buf);
		}
	}

	sprintf_s(buf, "  total: %d nodes\n", (int)nodes_.size());
	OutputDebugStringA(buf);

	if (!nodes_.empty())
	{
		sprintf_s(buf, "  root minH %.3f maxH %.3f\n", nodes_[0].minH, nodes_[0].maxH);
		OutputDebugStringA(buf);
	}
	int vis[16] = {};
	for (size_t i = 0; i < visible_.size(); ++i)
	{
		int lv = nodes_[visible_[i]].level;
		if (lv < 16) vis[lv]++;
	}
	sprintf_s(buf, "  selected: %d nodes\n", (int)visible_.size());
	OutputDebugStringA(buf);
	for (int lv = 0; lv < 16; ++lv)
	{
		if (vis[lv] > 0)
		{
			sprintf_s(buf, "    level %d: %d\n", lv, vis[lv]);
			OutputDebugStringA(buf);
		}
	}
}

void Terrain::Select(float camX, float camY, float camZ, const float* viewProj)
{
	visible_.clear();
	if (nodes_.empty()) return;

	const float* m = viewProj;
	for (int i = 0; i < 4; ++i)
	{
		planes_[0][i] = m[i * 4 + 3] + m[i * 4 + 0];
		planes_[1][i] = m[i * 4 + 3] - m[i * 4 + 0];
		planes_[2][i] = m[i * 4 + 3] + m[i * 4 + 1];
		planes_[3][i] = m[i * 4 + 3] - m[i * 4 + 1];
		planes_[4][i] = m[i * 4 + 2];
		planes_[5][i] = m[i * 4 + 3] - m[i * 4 + 2];
	}
	for (int p = 0; p < 6; ++p)
	{
		float len = sqrtf(planes_[p][0] * planes_[p][0] + planes_[p][1] * planes_[p][1] + planes_[p][2] * planes_[p][2]);
		for (int i = 0; i < 4; ++i)
		{
			planes_[p][i] /= len;
		}
	}

	SelectNode(0, camX, camY, camZ);
}

void Terrain::SelectNode(int idx, float camX, float camY, float camZ)
{
	const TerrainNode& n = nodes_[idx];

	if (!InFrustum(n)) return;

	float hs = LevelHeightScale(n.level);
	float cx = n.x + n.size * 0.5f + originX_;
	float cz = n.z + n.size * 0.5f + originZ_;
	float cy = (n.minH + n.maxH) * 0.5f * hs + kOriginY;

	float dx = cx - camX;
	float dy = cy - camY;
	float dz = cz - camZ;
	float d = sqrtf(dx * dx + dy * dy + dz * dz);

	bool isLeaf = (n.child[0] < 0);
	if (isLeaf || d > n.size * kSplitFactor)
	{
		visible_.push_back(idx);
		return;
	}

	for (int i = 0; i < 4; ++i)
	{
		SelectNode(n.child[i], camX, camY, camZ);
	}
}

bool Terrain::InFrustum(const TerrainNode& n) const
{
	float hs = LevelHeightScale(n.level);
	float cx = n.x + n.size * 0.5f + originX_;
	float cz = n.z + n.size * 0.5f + originZ_;
	float cy = (n.minH + n.maxH) * 0.5f * hs + kOriginY;

	float ex = n.size * 0.5f;
	float ez = n.size * 0.5f;
	float ey = (n.maxH - n.minH) * 0.5f * hs;

	for (int p = 0; p < 6; ++p)
	{
		float a = planes_[p][0];
		float b = planes_[p][1];
		float c = planes_[p][2];
		float d = planes_[p][3];

		float dist = a * cx + b * cy + c * cz + d;
		float r = ex * fabsf(a) + ey * fabsf(b) + ez * fabsf(c);

		if (dist + r < 0.0f) return false;
	}
	return true;
}