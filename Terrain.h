#pragma once
#include <vector>

struct TerrainNode
{
	int x = 0;
	int z = 0;
	int size = 0;
	int level = 0;
	float minH = 0.0f;
	float maxH = 0.0f;
	int child[4] = { -1, -1, -1, -1 };
};

class Terrain
{
public:
	void BuildFake(int size);
	void BuildQuadtree();

	bool LoadRaw16(const char* path);
	int MapSize() const { return mapSize_; }
	const std::vector<float>& Heights() const { return heights_; }

	void PrintStats() const;
	void Select(float camX, float camY, float camZ, const float* viewProj);
	void  SetOrigin(float x, float z) { originX_ = x; originZ_ = z; }
	float OriginX() const { return originX_; }
	float OriginZ() const { return originZ_; }
	static float OriginY() { return -100.0f; }
	static float HeightScale() { return 100.0f; }

	int MaxLevel() const { return maxLevel_; }
	static void  SetLodBoost(float b) { lodBoost_ = b; }
	static float LodBoost() { return lodBoost_; }
	float LevelHeightScale(int level) const;

	int VisibleCount() const { return (int)visible_.size(); }
	const TerrainNode& VisibleNode(int i) const { return nodes_[visible_[i]]; }
private:
	int BuildNode(int x, int z, int size, int level);
	void SelectNode(int idx, float camX, float camY, float camZ);
	bool InFrustum(const TerrainNode& n) const;
	//баянчики1
	int   maxLevel_ = 0;
	static float lodBoost_;

	float originX_ = -512.0f;
	float originZ_ = -512.0f;

	static const int LEAF_SIZE = 32;

	std::vector<float>	heights_;
	int					mapSize_ = 0;
	std::vector<TerrainNode> nodes_;
	std::vector<int> visible_;
	float planes_[6][4] = {};
};