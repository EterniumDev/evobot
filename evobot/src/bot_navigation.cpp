//
// EvoBot - Neoptolemus' Natural Selection bot, based on Botman's HPB bot template
//
// bot_navigation.cpp
// 
// Handles all bot path finding and movement
//

#include "bot_navigation.h"

#include <stdlib.h>
#include <math.h>

#include <extdll.h>
#include <dllapi.h>

#include "DetourNavMesh.h"
#include "DetourCommon.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "DetourNavMeshBuilder.h"
#include "fastlz.c"
#include "DetourAlloc.h"

#include "bot_math.h"
#include "bot_tactical.h"
#include "general_util.h"
#include "bot_util.h"
#include "bot_weapons.h"
#include "game_state.h"
#include "bot_task.h"
#include "bot_bsp.h"
#include "bot_config.h"

extern hive_definition Hives[10];
extern int NumTotalHives;

nav_door NavDoors[32];
nav_weldable NavWeldableObstacles[32];
int NumDoors;
int NumWeldableObstacles;

extern edict_t* clients[MAX_CLIENTS];
extern bot_t bots[MAX_CLIENTS];

struct NavMeshSetHeader
{
	int magic;
	int version;
	int numTiles;
	dtNavMeshParams params;
	int MeshBuildOffset;
};

struct TileCacheSetExportHeader
{
	int magic;
	int version;

	int numRegularTiles;
	dtNavMeshParams regularMeshParams;
	dtTileCacheParams regularCacheParams;

	int numOnosTiles;
	dtNavMeshParams onosMeshParams;
	dtTileCacheParams onosCacheParams;

	int numBuildingTiles;
	dtNavMeshParams buildingMeshParams;
	dtTileCacheParams buildingCacheParams;

	int regularNavOffset;
	int onosNavOffset;
	int buildingNavOffset;

	int NumOffMeshCons;

	int OffMeshConVertsOffset;
	int OffMeshConVertsLength;

	int OffMeshConRadsOffset;
	int OffMeshConRadsLength;

	int OffMeshConDirsOffset;
	int OffMeshConDirsLength;

	int OffMeshConAreasOffset;
	int OffMeshConAreasLength;

	int OffMeshConFlagsOffset;
	int OffMeshConFlagsLength;

	int OffMeshConUserIDsOffset;
	int OffMeshConUserIDsLength;
};

struct TileCacheTileHeader
{
	dtCompressedTileRef tileRef;
	int dataSize;
};

struct NavMeshTileHeader
{
	dtTileRef tileRef;
	int dataSize;
};

struct OffMeshConnectionDef
{
	bool bIsActive = false;
	float spos[3] = { 0.0f, 0.0f, 0.0f };
	float epos[3] = { 0.0f, 0.0f, 0.0f };
	bool bBiDir = false;
	float Rad = 0.0f;
	char Area = 0;
	short Flag = 0;
};

struct FastLZCompressor : public dtTileCacheCompressor
{
	virtual int maxCompressedSize(const int bufferSize)
	{
		return (int)(bufferSize * 1.05f);
	}

	virtual dtStatus compress(const unsigned char* buffer, const int bufferSize,
		unsigned char* compressed, const int /*maxCompressedSize*/, int* compressedSize)
	{
		*compressedSize = fastlz_compress((const void* const)buffer, bufferSize, compressed);
		return DT_SUCCESS;
	}

	virtual dtStatus decompress(const unsigned char* compressed, const int compressedSize,
		unsigned char* buffer, const int maxBufferSize, int* bufferSize)
	{
		*bufferSize = fastlz_decompress(compressed, compressedSize, buffer, maxBufferSize);
		return *bufferSize < 0 ? DT_FAILURE : DT_SUCCESS;
	}
};

struct LinearAllocator : public dtTileCacheAlloc
{
	unsigned char* buffer;
	size_t capacity;
	size_t top;
	size_t high;

	LinearAllocator(const size_t cap) : buffer(0), capacity(0), top(0), high(0)
	{
		resize(cap);
	}

	~LinearAllocator()
	{
		dtFree(buffer);
	}

	void resize(const size_t cap)
	{
		if (buffer) dtFree(buffer);
		buffer = (unsigned char*)dtAlloc(cap, DT_ALLOC_PERM);
		capacity = cap;
	}

	virtual void reset()
	{
		high = dtMax(high, top);
		top = 0;
	}

	virtual void* alloc(const size_t size)
	{
		if (!buffer)
			return 0;
		if (top + size > capacity)
			return 0;
		unsigned char* mem = &buffer[top];
		top += size;
		return mem;
	}

	virtual void free(void* /*ptr*/)
	{
		// Empty
	}
};

struct MeshProcess : public dtTileCacheMeshProcess
{
	int NumOffMeshConns = 0;
	float OffMeshVerts[MAX_OFFMESH_CONNS * 6];
	float OffMeshRads[MAX_OFFMESH_CONNS];
	unsigned char OffMeshDirs[MAX_OFFMESH_CONNS];
	unsigned char OffMeshAreas[MAX_OFFMESH_CONNS];
	unsigned short OffMeshFlags[MAX_OFFMESH_CONNS];
	unsigned int OffMeshIDs[MAX_OFFMESH_CONNS];

	OffMeshConnectionDef ConnectionDefinitions[MAX_OFFMESH_CONNS];

	inline MeshProcess()
	{}

	inline void init(OffMeshConnectionDef* OffMeshConnData, int NumConns)
	{

	}

	int AddOffMeshConnectionDef(Vector Start, Vector End, unsigned char area, unsigned short flag, bool bBiDirectional)
	{
		float spos[3] = { Start.x, Start.z, -Start.y };
		float epos[3] = { End.x, End.z, -End.y };

		if (NumOffMeshConns >= MAX_OFFMESH_CONNS) return -1;
		float* v = &OffMeshVerts[NumOffMeshConns * 3 * 2];
		OffMeshRads[NumOffMeshConns] = 18.0f;
		OffMeshDirs[NumOffMeshConns] = bBiDirectional;
		OffMeshAreas[NumOffMeshConns] = area;
		OffMeshFlags[NumOffMeshConns] = flag;
		OffMeshIDs[NumOffMeshConns] = 1000 + NumOffMeshConns;
		dtVcopy(&v[0], spos);
		dtVcopy(&v[3], epos);
		NumOffMeshConns++;
		return NumOffMeshConns - 1;
	}

	void RemoveOffMeshConnectionDef(int Index)
	{
		if (Index > -1 && Index < MAX_OFFMESH_CONNS)
		{
			NumOffMeshConns--;
			float* src = &OffMeshVerts[NumOffMeshConns * 3 * 2];
			float* dst = &OffMeshVerts[Index * 3 * 2];
			dtVcopy(&dst[0], &src[0]);
			dtVcopy(&dst[3], &src[3]);
			OffMeshRads[Index] = OffMeshRads[NumOffMeshConns];
			OffMeshDirs[Index] = OffMeshDirs[NumOffMeshConns];
			OffMeshAreas[Index] = OffMeshAreas[NumOffMeshConns];
			OffMeshFlags[Index] = OffMeshFlags[NumOffMeshConns];
		}
	}

	void DrawAllConnections()
	{
		Vector StartLine = ZERO_VECTOR;
		Vector EndLine = ZERO_VECTOR;

		for (int i = 0; i < NumOffMeshConns; i++)
		{
			Vector StartLine = Vector(OffMeshVerts[i * 6], -OffMeshVerts[(i * 6) + 2], OffMeshVerts[(i * 6) + 1]);
			Vector EndLine = Vector(OffMeshVerts[(i * 6) + 3], -OffMeshVerts[(i * 6) + 5], OffMeshVerts[(i * 6) + 4]);

			switch (OffMeshFlags[i])
			{
			case SAMPLE_POLYFLAGS_WALK:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 255, 255, 255);
				break;
			case SAMPLE_POLYFLAGS_JUMP:
			case SAMPLE_POLYFLAGS_HIGHJUMP:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 255, 255, 0);
				break;
			case SAMPLE_POLYFLAGS_WALLCLIMB:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 0, 255, 0);
				break;
			case SAMPLE_POLYFLAGS_FALL:
			case SAMPLE_POLYFLAGS_HIGHFALL:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 255, 0, 0);
				break;
			case SAMPLE_POLYFLAGS_LADDER:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 0, 0, 255);
				break;
			case SAMPLE_POLYFLAGS_PHASEGATE:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 255, 128, 128);
				break;
			default:
				UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, 30.0f, 0, 255, 255);
				break;
			}
		}
	}

	virtual void process(struct dtNavMeshCreateParams* params,
		unsigned char* polyAreas, unsigned short* polyFlags)
	{
		// Update poly flags from areas.
		for (int i = 0; i < params->polyCount; ++i)
		{
			if (polyAreas[i] == DT_TILECACHE_WALKABLE_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_GROUND;
				polyFlags[i] = SAMPLE_POLYFLAGS_WALK;
			}
			else if (polyAreas[i] == DT_TILECACHE_CLIMBABLE_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_WALLCLIMB;
				polyFlags[i] = SAMPLE_POLYFLAGS_WALLCLIMB;
			}
			else if (polyAreas[i] == DT_TILECACHE_LADDER_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_LADDER;
				polyFlags[i] = SAMPLE_POLYFLAGS_LADDER;
			}
			else if (polyAreas[i] == DT_TILECACHE_CROUCH_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_CROUCH;
				polyFlags[i] = SAMPLE_POLYFLAGS_CROUCH;
			}
			else if (polyAreas[i] == DT_TILECACHE_BLOCKED_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_BLOCKED;
				polyFlags[i] = SAMPLE_POLYFLAGS_BLOCKED;
			}
			else if (polyAreas[i] == DT_TILECACHE_ASTRUCTURE_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_ASTRUCTURE;
				polyFlags[i] = SAMPLE_POLYFLAGS_ASTRUCTURE;
			}
			else if (polyAreas[i] == DT_TILECACHE_MSTRUCTURE_AREA)
			{
				polyAreas[i] = SAMPLE_POLYAREA_MSTRUCTURE;
				polyFlags[i] = SAMPLE_POLYFLAGS_MSTRUCTURE;
			}
		}

		params->offMeshConAreas = OffMeshAreas;
		params->offMeshConCount = NumOffMeshConns;
		params->offMeshConDir = OffMeshDirs;
		params->offMeshConFlags = OffMeshFlags;
		params->offMeshConRad = OffMeshRads;
		params->offMeshConUserID = OffMeshIDs;
		params->offMeshConVerts = OffMeshVerts;

	}
};

void UTIL_UpdateTileCache()
{
	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].tileCache)
		{
			NavMeshes[i].tileCache->update(0.0f, NavMeshes[i].navMesh);
		}
	}
}


void UTIL_DrawTemporaryObstacles()
{
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtTileCache* m_tileCache = UTIL_GetTileCacheForProfile(ALL_NAV_PROFILE);

	if (m_navMesh)
	{
		int NumObstacles = m_tileCache->getObstacleCount();

		for (int i = 0; i < NumObstacles; i++)
		{
			const dtTileCacheObstacle* ObstacleRef = m_tileCache->getObstacle(i);

			if (!ObstacleRef || ObstacleRef->state != DT_OBSTACLE_PROCESSED) { continue; }

			if (ObstacleRef->type == ObstacleType::DT_OBSTACLE_BOX)
			{
				Vector bMin = Vector(ObstacleRef->box.bmin[0], -ObstacleRef->box.bmin[2], ObstacleRef->box.bmin[1]);
				Vector bMax = Vector(ObstacleRef->box.bmax[0], -ObstacleRef->box.bmax[2], ObstacleRef->box.bmax[1]);

				UTIL_DrawBox(GAME_GetListenServerEdict(), bMin, bMax, 10.0f);
				continue;
			}

			if (ObstacleRef->type == ObstacleType::DT_OBSTACLE_CYLINDER)
			{

				float Radius = ObstacleRef->cylinder.radius;
				float Height = ObstacleRef->cylinder.height;

				// The location of obstacles in Recast are at the bottom of the shape, not the centre
				Vector Centre = Vector(ObstacleRef->cylinder.pos[0], -ObstacleRef->cylinder.pos[2], ObstacleRef->cylinder.pos[1] + (Height * 0.5f));

				if (vDist2DSq(GAME_GetListenServerEdict()->v.origin, Centre) > sqrf(UTIL_MetresToGoldSrcUnits(10.0f))) { continue; }

				Vector bMin = Centre - Vector(Radius, Radius, Height * 0.5f);
				Vector bMax = Centre + Vector(Radius, Radius, (Height * 0.5f));

				UTIL_DrawBox(GAME_GetListenServerEdict(), bMin, bMax, 10.0f);
				continue;
			}

		}
	}
}


Vector UTIL_AdjustPointAwayFromNavWall(const Vector Location, const float MaxDistanceFromWall)
{

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(SKULK_REGULAR_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(SKULK_REGULAR_NAV_PROFILE);

	float Pos[3] = { Location.x, Location.z, -Location.y };

	float HitDist = 0.0f;
	float HitPos[3] = { 0.0f, 0.0f, 0.0f };
	float HitNorm[3] = { 0.0f, 0.0f, 0.0f };

	dtPolyRef StartPoly = UTIL_GetNearestPolyRefForLocation(Location);

	dtStatus Result = m_navQuery->findDistanceToWall(StartPoly, Pos, MaxDistanceFromWall, m_navFilter, &HitDist, HitPos, HitNorm);

	if (dtStatusSucceed(Result))
	{
		float AdjustDistance = MaxDistanceFromWall - HitDist;

		Vector HitPosVector = Vector(HitPos[0], -HitPos[2], HitPos[1]);

		Vector AdjustDir = (HitDist > 0.1f) ? UTIL_GetVectorNormal2D(Location - HitPosVector) : Vector(HitNorm[0], -HitNorm[2], HitNorm[1]);

		Vector AdjustLocation = Location + (AdjustDir * AdjustDistance);

		float AdjustLoc[3] = { AdjustLocation.x, AdjustLocation.z, -AdjustLocation.y };

		if (UTIL_TraceNav(SKULK_REGULAR_NAV_PROFILE, Location, AdjustLocation, 0.1f))
		{
			return AdjustLocation;
		}
		else
		{
			return Location;
		}
	}

	return Location;
}

Vector UTIL_GetNearestPointOnNavWall(bot_t* pBot, const float MaxRadius)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	if (NavProfileIndex < 0) { return ZERO_VECTOR; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	Vector Location = pBot->CurrentFloorPosition;

	float Pos[3] = { Location.x, Location.z, -Location.y };

	float HitDist = 0.0f;
	float HitPos[3] = { 0.0f, 0.0f, 0.0f };
	float HitNorm[3] = { 0.0f, 0.0f, 0.0f };

	dtStatus Result = m_navQuery->findDistanceToWall(pBot->BotNavInfo.CurrentPoly, Pos, MaxRadius, m_navFilter, &HitDist, HitPos, HitNorm);

	if (dtStatusSucceed(Result) && HitDist > 0.0f)
	{
		Vector HitResult = Vector(HitPos[0], -HitPos[2], HitPos[1]);
		return HitResult;
	}

	return ZERO_VECTOR;
}

Vector UTIL_GetNearestPointOnNavWall(const int NavProfileIndex, const Vector Location, const float MaxRadius)
{
	if (NavProfileIndex < 0)
	{
		return ZERO_VECTOR;
	}

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	// Invalid nav profile
	if (!m_navQuery) { return ZERO_VECTOR; }

	dtPolyRef StartPoly = UTIL_GetNearestPolyRefForLocation(NavProfileIndex, Location);

	// Not on the nav mesh
	if (StartPoly == 0) { return Location; }

	float Pos[3] = { Location.x, Location.z, -Location.y };

	float HitDist = 0.0f;
	float HitPos[3] = { 0.0f, 0.0f, 0.0f };
	float HitNorm[3] = { 0.0f, 0.0f, 0.0f };

	dtStatus Result = m_navQuery->findDistanceToWall(StartPoly, Pos, MaxRadius, m_navFilter, &HitDist, HitPos, HitNorm);

	// We hit something
	if (dtStatusSucceed(Result) && HitDist < MaxRadius)
	{
		Vector HitResult = Vector(HitPos[0], -HitPos[2], HitPos[1]);
		return HitResult;
	}

	// Didn't hit anything
	return ZERO_VECTOR;
}

void RecalcAllBotPaths()
{
	for (int i = 0; i < 32; i++)
	{
		if (bots[i].is_used)
		{
			if (bots[i].BotNavInfo.PathSize > 0)
			{
				bots[i].BotNavInfo.bPendingRecalculation = true;
			}
		}
	}
}

unsigned int UTIL_AddTemporaryObstacle(const Vector Location, float Radius, float Height, int area)
{
	unsigned int ObstacleNum = 0;

	float Pos[3] = { Location.x, Location.z - (Height * 0.5f), -Location.y };

	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].tileCache)
		{
			dtObstacleRef ObsRef = 0;
			NavMeshes[i].tileCache->addObstacle(Pos, Radius, Height, area, &ObsRef);

			ObstacleNum = (unsigned int)ObsRef;
		}
	}

	return ObstacleNum;
}

void UTIL_AddTemporaryObstacles(const Vector Location, float Radius, float Height, int area, unsigned int* ObstacleRefArray)
{
	unsigned int ObstacleNum = 0;

	float Pos[3] = { Location.x, Location.z - (Height * 0.5f), -Location.y };

	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		ObstacleRefArray[i] = 0;

		if (NavMeshes[i].tileCache)
		{
			dtObstacleRef ObsRef = 0;
			NavMeshes[i].tileCache->addObstacle(Pos, Radius, Height, area, &ObsRef);

			ObstacleRefArray[i] = (unsigned int)ObsRef;
		}
	}
}

unsigned int UTIL_AddTemporaryBoxObstacle(const Vector bMin, const Vector bMax, int area)
{
	unsigned int ObstacleNum = 0;

	float bMinf[3] = { bMin.x, bMin.z, -bMin.y };
	float bMaxf[3] = { bMax.x, bMax.z, -bMax.y };

	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].tileCache)
		{
			dtObstacleRef ObsRef = 0;
			NavMeshes[i].tileCache->addBoxObstacle(bMinf, bMaxf, area, &ObsRef);

			ObstacleNum = (unsigned int)ObsRef;

		}
	}

	return ObstacleNum;
}

void DEBUG_DrawNavMesh(const Vector DrawCentre, const int NavMeshIndex)
{
	if (NavMeshIndex < 0 || NavMeshIndex >(MAX_NAV_MESHES - 1)) { return; }

	const dtTileCache* m_tileCache = NavMeshes[NavMeshIndex].tileCache;

	if (m_tileCache)
	{
		dtCompressedTileRef DrawnTiles[16];
		int NumTiles;

		Vector vMin = Vector((DrawCentre.x - 50.0f), (DrawCentre.y - 50.0f), (DrawCentre.z - 50.0f));
		Vector vMax = Vector((DrawCentre.x + 50.0f), (DrawCentre.y + 50.0f), (DrawCentre.z + 50.0f));

		float Min[3] = { vMin.x, vMin.z, -vMin.y };
		float Max[3] = { vMax.x, vMax.z, -vMax.y };

		m_tileCache->queryTiles(Min, Max, DrawnTiles, &NumTiles, 16);

		if (NumTiles == 0)
		{
			char buf[64];
			sprintf(buf, "Failed to find any tiles! %d in total\n", m_tileCache->getTileCount());
			UTIL_SayText(buf, GAME_GetListenServerEdict());
		}

		for (int i = 0; i < NumTiles; i++)
		{
			const dtCompressedTile* Tile = m_tileCache->getTileByRef(DrawnTiles[i]);

			float bmin[3], bmax[3];

			m_tileCache->calcTightTileBounds(Tile->header, bmin, bmax);

			Vector MinBox = Vector(bmin[0], -bmin[2], bmin[1]);
			Vector MaxBox = Vector(bmax[0], -bmax[2], bmax[1]);

			UTIL_DrawLine(GAME_GetListenServerEdict(), Vector(MinBox.x, MinBox.y, MinBox.z + 10.0f), Vector(MaxBox.x, MinBox.y, MinBox.z + 10.0f), 5.0f);
			UTIL_DrawLine(GAME_GetListenServerEdict(), Vector(MaxBox.x, MinBox.y, MinBox.z + 10.0f), Vector(MaxBox.x, MaxBox.y, MinBox.z + 10.0f), 5.0f);
			UTIL_DrawLine(GAME_GetListenServerEdict(), Vector(MaxBox.x, MaxBox.y, MinBox.z + 10.0f), Vector(MinBox.x, MaxBox.y, MinBox.z + 10.0f), 5.0f);
			UTIL_DrawLine(GAME_GetListenServerEdict(), Vector(MinBox.x, MaxBox.y, MinBox.z + 10.0f), Vector(MinBox.x, MinBox.y, MinBox.z + 10.0f), 5.0f);
		}
	}
	else
	{
		UTIL_SayText("No Tile Cache!\n", GAME_GetListenServerEdict());
	}
}

void UTIL_RemoveTemporaryObstacle(unsigned int ObstacleRef)
{
	if (ObstacleRef == 0) { return; }

	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].tileCache)
		{
			NavMeshes[i].tileCache->removeObstacle((dtObstacleRef)ObstacleRef);
		}
	}
}

void UTIL_RemoveTemporaryObstacles(unsigned int* ObstacleRefs)
{
	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].tileCache)
		{
			NavMeshes[i].tileCache->removeObstacle(ObstacleRefs[i]);

		}

		ObstacleRefs[i] = 0;
	}
}

void GetFullFilePath(char* buffer, const char* mapname)
{
	UTIL_BuildFileName(buffer, "addons", "evobot", "navmeshes", mapname);
	strcat(buffer, ".nav");
}

void DEBUG_DrawOffMeshConnections()
{
	if (NavMeshes[0].tileCache)
	{
		MeshProcess* m_tmproc = (MeshProcess*)NavMeshes[0].tileCache->getMeshProcess();

		if (m_tmproc)
		{
			m_tmproc->DrawAllConnections();
		}
	}
}

void ReloadNavMeshes()
{
	UnloadNavMeshes();
	LoadNavMesh(STRING(gpGlobals->mapname));
}

void UnloadNavMeshes()
{
	for (int i = 0; i < MAX_NAV_MESHES; i++)
	{
		if (NavMeshes[i].navMesh)
		{
			dtFreeNavMesh(NavMeshes[i].navMesh);
			NavMeshes[i].navMesh = nullptr;
		}

		if (NavMeshes[i].navQuery)
		{
			dtFreeNavMeshQuery(NavMeshes[i].navQuery);
			NavMeshes[i].navQuery = nullptr;
		}

		if (NavMeshes[i].tileCache)
		{
			dtFreeTileCache(NavMeshes[i].tileCache);
			NavMeshes[i].tileCache = nullptr;
		}
	}
}

void UnloadNavigationData()
{
	UnloadNavMeshes();

	memset(NavProfiles, 0, sizeof(nav_profile));
	memset(NavDoors, 0, sizeof(NavDoors));
	memset(NavWeldableObstacles, 0, sizeof(NavWeldableObstacles));
	NumDoors = 0;

	UTIL_ClearMapAIData();
	UTIL_ClearMapLocations();
}

bool LoadNavMesh(const char* mapname)
{
	char filename[256]; // Full path to BSP file

	GetFullFilePath(filename, mapname);

	FILE* savedFile = fopen(filename, "rb");

	if (!savedFile)
	{
		return false;
	}

	// Read header.
	TileCacheSetExportHeader header;
	size_t headerReadReturnCode = fread(&header, sizeof(TileCacheSetExportHeader), 1, savedFile);
	if (headerReadReturnCode != 1)
	{
		// Error or early EOF
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}
	if (header.magic != TILECACHESET_MAGIC)
	{
		char buf[64];
		sprintf(buf, "Header Magic does not match! %d\n", header.magic);
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, buf);
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}
	if (header.version != TILECACHESET_VERSION)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Header version does not match!\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	NavMeshes[REGULAR_NAV_MESH].navMesh = dtAllocNavMesh();
	if (!NavMeshes[REGULAR_NAV_MESH].navMesh)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate navmesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}
	dtStatus status = NavMeshes[REGULAR_NAV_MESH].navMesh->init(&header.regularMeshParams);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise nav mesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	NavMeshes[REGULAR_NAV_MESH].tileCache = dtAllocTileCache();
	if (!NavMeshes[REGULAR_NAV_MESH].tileCache)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}


	NavMeshes[ONOS_NAV_MESH].navMesh = dtAllocNavMesh();
	if (!NavMeshes[ONOS_NAV_MESH].navMesh)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate onos navmesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}
	status = NavMeshes[ONOS_NAV_MESH].navMesh->init(&header.onosMeshParams);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise onos nav mesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	NavMeshes[ONOS_NAV_MESH].tileCache = dtAllocTileCache();
	if (!NavMeshes[ONOS_NAV_MESH].tileCache)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate onos tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}


	NavMeshes[BUILDING_NAV_MESH].navMesh = dtAllocNavMesh();
	if (!NavMeshes[BUILDING_NAV_MESH].navMesh)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate building navmesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}
	status = NavMeshes[BUILDING_NAV_MESH].navMesh->init(&header.buildingMeshParams);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise building nav mesh\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	NavMeshes[BUILDING_NAV_MESH].tileCache = dtAllocTileCache();
	if (!NavMeshes[BUILDING_NAV_MESH].tileCache)
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not allocate building tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}


	int CurrFilePos = ftell(savedFile);

	LinearAllocator* m_talloc = new LinearAllocator(32000);
	FastLZCompressor* m_tcomp = new FastLZCompressor;
	MeshProcess* m_tmproc = new MeshProcess;

	m_tmproc->NumOffMeshConns = header.NumOffMeshCons;

	fseek(savedFile, header.OffMeshConAreasOffset, SEEK_SET);
	size_t ReadResult = fread(m_tmproc->OffMeshAreas, header.OffMeshConAreasLength, 1, savedFile);

	fseek(savedFile, header.OffMeshConDirsOffset, SEEK_SET);
	ReadResult = fread(m_tmproc->OffMeshDirs, header.OffMeshConDirsLength, 1, savedFile);

	fseek(savedFile, header.OffMeshConFlagsOffset, SEEK_SET);
	ReadResult = fread(m_tmproc->OffMeshFlags, header.OffMeshConFlagsLength, 1, savedFile);

	fseek(savedFile, header.OffMeshConRadsOffset, SEEK_SET);
	ReadResult = fread(m_tmproc->OffMeshRads, header.OffMeshConRadsLength, 1, savedFile);

	fseek(savedFile, header.OffMeshConUserIDsOffset, SEEK_SET);
	ReadResult = fread(m_tmproc->OffMeshIDs, header.OffMeshConUserIDsLength, 1, savedFile);

	fseek(savedFile, header.OffMeshConVertsOffset, SEEK_SET);
	ReadResult = fread(m_tmproc->OffMeshVerts, header.OffMeshConVertsLength, 1, savedFile);

	// TODO: Need to pass all off mesh connection verts, areas, flags etc as arrays to m_tmproc. Needs to be exported from recast as such

	status = NavMeshes[REGULAR_NAV_MESH].tileCache->init(&header.regularCacheParams, m_talloc, m_tcomp, m_tmproc);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	status = NavMeshes[ONOS_NAV_MESH].tileCache->init(&header.onosCacheParams, m_talloc, m_tcomp, m_tmproc);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	status = NavMeshes[BUILDING_NAV_MESH].tileCache->init(&header.buildingCacheParams, m_talloc, m_tcomp, m_tmproc);
	if (dtStatusFailed(status))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise tile cache\n");
		fclose(savedFile);
		UnloadNavigationData();
		return false;
	}

	fseek(savedFile, CurrFilePos, SEEK_SET);

	// Read tiles.
	for (int i = 0; i < header.numRegularTiles; ++i)
	{
		TileCacheTileHeader tileHeader;
		size_t tileHeaderReadReturnCode = fread(&tileHeader, sizeof(tileHeader), 1, savedFile);
		if (tileHeaderReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile header read returned code\n");
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}
		if (!tileHeader.tileRef || !tileHeader.dataSize)
			break;

		unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
		if (!data) break;
		memset(data, 0, tileHeader.dataSize);
		size_t tileDataReadReturnCode = fread(data, tileHeader.dataSize, 1, savedFile);
		if (tileDataReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile data read returned code\n");
			// Error or early EOF
			dtFree(data);
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}

		dtCompressedTileRef tile = 0;
		dtStatus addTileStatus = NavMeshes[REGULAR_NAV_MESH].tileCache->addTile(data, tileHeader.dataSize, DT_COMPRESSEDTILE_FREE_DATA, &tile);
		if (dtStatusFailed(addTileStatus))
		{
			dtFree(data);
		}

		if (tile)
			NavMeshes[REGULAR_NAV_MESH].tileCache->buildNavMeshTile(tile, NavMeshes[REGULAR_NAV_MESH].navMesh);
	}

	for (int i = 0; i < header.numOnosTiles; ++i)
	{
		TileCacheTileHeader tileHeader;
		size_t tileHeaderReadReturnCode = fread(&tileHeader, sizeof(tileHeader), 1, savedFile);
		if (tileHeaderReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile header read returned code\n");
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}
		if (!tileHeader.tileRef || !tileHeader.dataSize)
			break;

		unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
		if (!data) break;
		memset(data, 0, tileHeader.dataSize);
		size_t tileDataReadReturnCode = fread(data, tileHeader.dataSize, 1, savedFile);
		if (tileDataReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile data read returned code\n");
			// Error or early EOF
			dtFree(data);
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}

		dtCompressedTileRef tile = 0;
		dtStatus addTileStatus = NavMeshes[ONOS_NAV_MESH].tileCache->addTile(data, tileHeader.dataSize, DT_COMPRESSEDTILE_FREE_DATA, &tile);
		if (dtStatusFailed(addTileStatus))
		{
			dtFree(data);
		}

		if (tile)
			NavMeshes[ONOS_NAV_MESH].tileCache->buildNavMeshTile(tile, NavMeshes[ONOS_NAV_MESH].navMesh);
	}

	for (int i = 0; i < header.numBuildingTiles; ++i)
	{
		TileCacheTileHeader tileHeader;
		size_t tileHeaderReadReturnCode = fread(&tileHeader, sizeof(tileHeader), 1, savedFile);
		if (tileHeaderReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile header read returned code\n");
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}
		if (!tileHeader.tileRef || !tileHeader.dataSize)
			break;

		unsigned char* data = (unsigned char*)dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM);
		if (!data) break;
		memset(data, 0, tileHeader.dataSize);
		size_t tileDataReadReturnCode = fread(data, tileHeader.dataSize, 1, savedFile);
		if (tileDataReadReturnCode != 1)
		{
			ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Tile data read returned code\n");
			// Error or early EOF
			dtFree(data);
			fclose(savedFile);
			UnloadNavigationData();
			return false;
		}

		dtCompressedTileRef tile = 0;
		dtStatus addTileStatus = NavMeshes[BUILDING_NAV_MESH].tileCache->addTile(data, tileHeader.dataSize, DT_COMPRESSEDTILE_FREE_DATA, &tile);
		if (dtStatusFailed(addTileStatus))
		{
			dtFree(data);
		}

		if (tile)
			NavMeshes[BUILDING_NAV_MESH].tileCache->buildNavMeshTile(tile, NavMeshes[BUILDING_NAV_MESH].navMesh);
	}

	fclose(savedFile);

	NavMeshes[REGULAR_NAV_MESH].navQuery = dtAllocNavMeshQuery();

	dtStatus initStatus = NavMeshes[REGULAR_NAV_MESH].navQuery->init(NavMeshes[REGULAR_NAV_MESH].navMesh, 65535);

	if (dtStatusFailed(initStatus))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise nav query\n");
		UnloadNavigationData();
		return false;
	}

	NavMeshes[ONOS_NAV_MESH].navQuery = dtAllocNavMeshQuery();

	initStatus = NavMeshes[ONOS_NAV_MESH].navQuery->init(NavMeshes[ONOS_NAV_MESH].navMesh, 65535);

	if (dtStatusFailed(initStatus))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise onos nav query\n");
		UnloadNavigationData();
		return false;
	}

	NavMeshes[BUILDING_NAV_MESH].navQuery = dtAllocNavMeshQuery();

	initStatus = NavMeshes[BUILDING_NAV_MESH].navQuery->init(NavMeshes[BUILDING_NAV_MESH].navMesh, 65535);

	if (dtStatusFailed(initStatus))
	{
		ClientPrint(GAME_GetListenServerEdict(), HUD_PRINTNOTIFY, "Could not initialise building nav query\n");
		UnloadNavigationData();
		return false;
	}

	return true;
}

bool loadNavigationData(const char* mapname)
{

	UnloadNavigationData();

	if (!LoadNavMesh(mapname))
	{
		return false;
	}
	
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_WALLCLIMB);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_MSTRUCTURE);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_PHASEGATE, 0.1f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_LADDER, 1.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 10.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 10.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 2.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_ASTRUCTURE, 20.0f);
	NavProfiles[MARINE_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[SKULK_REGULAR_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_LADDER);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_WALLCLIMB, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 1.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[SKULK_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[FADE_REGULAR_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_PHASEGATE, 0.1f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_LADDER, 2.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 1.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 1.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 2.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_WALLCLIMB, 1.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[FADE_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[GORGE_REGULAR_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_WALLCLIMB);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_LADDER, 1.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 10.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 10.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 1.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[GORGE_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[ONOS_REGULAR_NAV_PROFILE].NavMeshIndex = ONOS_NAV_MESH;
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_WALLCLIMB);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_NOONOS);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 3.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_LADDER, 1.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 10.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 10.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 3.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 10.0f);
	NavProfiles[ONOS_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].NavMeshIndex = BUILDING_NAV_MESH;
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_BLOCKED);
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_MSTRUCTURE);
	NavProfiles[BUILDING_REGULAR_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[GORGE_BUILD_NAV_PROFILE].NavMeshIndex = BUILDING_NAV_MESH;
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_BLOCKED);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_MSTRUCTURE);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_WALLCLIMB);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[GORGE_BUILD_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_LADDER);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 50.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_WALLCLIMB, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 1.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[SKULK_AMBUSH_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[GORGE_HIDE_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_WALLCLIMB);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 5.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.5f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_LADDER, 1.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 10.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 10.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 1.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[GORGE_HIDE_NAV_PROFILE].bFlyingProfile = false;

	NavProfiles[LERK_FLYING_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_ASTRUCTURE);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.addExcludeFlags(SAMPLE_POLYFLAGS_PHASEGATE);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_GROUND, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_JUMP, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_FALL, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_WALLCLIMB, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHFALL, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_HIGHJUMP, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_CROUCH, 1.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_BLOCKED, 2.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].Filters.setAreaCost(SAMPLE_POLYAREA_MSTRUCTURE, 20.0f);
	NavProfiles[LERK_FLYING_NAV_PROFILE].bFlyingProfile = true;

	NavProfiles[ALL_NAV_PROFILE].NavMeshIndex = REGULAR_NAV_MESH;
	NavProfiles[ALL_NAV_PROFILE].Filters.setIncludeFlags(0xFFFF);
	NavProfiles[ALL_NAV_PROFILE].Filters.setExcludeFlags(0);
	NavProfiles[ALL_NAV_PROFILE].bFlyingProfile = false;

	UTIL_PopulateDoors();
	UTIL_PopulateWeldableObstacles();

	return true;
}

bool NavmeshLoaded()
{
	return NavMeshes[0].navMesh != nullptr;
}

Vector UTIL_GetRandomPointOnNavmesh(const bot_t* pBot)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	if (NavProfileIndex < 0) { return ZERO_VECTOR; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery)
	{
		return ZERO_VECTOR;
	}

	Vector Result;

	dtPolyRef refPoly;

	float result[3];
	memset(result, 0, sizeof(result));

	dtStatus status = m_navQuery->findRandomPoint(m_navFilter, frand, &refPoly, result);

	if (dtStatusFailed(status))
	{
		return ZERO_VECTOR;
	}

	Result.x = result[0];
	Result.y = -result[2];
	Result.z = result[1];

	return Result;
}

Vector UTIL_GetRandomPointOnNavmeshInRadiusOfAreaType(SamplePolyFlags Flag, const Vector origin, const float MaxRadius)
{
	const dtNavMeshQuery* m_NavQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);

	if (!m_NavQuery) { return ZERO_VECTOR; }

	dtQueryFilter filter;
	filter.setExcludeFlags(0);
	filter.setIncludeFlags(Flag);

	Vector Result = ZERO_VECTOR;

	float pCheckLoc[3] = { origin.x, origin.z, -origin.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus foundPolyResult = m_NavQuery->findNearestPoly(pCheckLoc, pExtents, &filter, &FoundPoly, NavNearest);

	if (dtStatusFailed(foundPolyResult))
	{
		return ZERO_VECTOR;
	}

	dtPolyRef RandomPoly;
	float RandomPoint[3];

	dtStatus foundRandomPointResult = m_NavQuery->findRandomPointAroundCircle(FoundPoly, NavNearest, MaxRadius, &filter, frand, &RandomPoly, RandomPoint);


	if (dtStatusFailed(foundRandomPointResult))
	{
		return ZERO_VECTOR;
	}

	Result.x = RandomPoint[0];
	Result.y = -RandomPoint[2];
	Result.z = RandomPoint[1];

	return Result;
}

Vector UTIL_GetRandomPointOnNavmeshInRadius(const int NavProfileIndex, const Vector origin, const float MaxRadius)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return ZERO_VECTOR; }

	Vector Result = ZERO_VECTOR;

	float pCheckLoc[3] = { origin.x, origin.z, -origin.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus foundPolyResult = m_navQuery->findNearestPoly(pCheckLoc, pExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusFailed(foundPolyResult))
	{
		return ZERO_VECTOR;
	}

	dtPolyRef RandomPoly;
	float RandomPoint[3];

	dtStatus foundRandomPointResult = m_navQuery->findRandomPointAroundCircle(FoundPoly, NavNearest, MaxRadius, m_navFilter, frand, &RandomPoly, RandomPoint);


	if (dtStatusFailed(foundRandomPointResult))
	{
		return ZERO_VECTOR;
	}

	Result.x = RandomPoint[0];
	Result.y = -RandomPoint[2];
	Result.z = RandomPoint[1];

	return Result;
}

Vector UTIL_GetRandomPointOnNavmeshInRadiusIgnoreReachability(const int NavProfileIndex, const Vector origin, const float MaxRadius)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return ZERO_VECTOR; }

	Vector Result = ZERO_VECTOR;

	float pCheckLoc[3] = { origin.x, origin.z, -origin.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus foundPolyResult = m_navQuery->findNearestPoly(pCheckLoc, pExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusFailed(foundPolyResult))
	{
		return ZERO_VECTOR;
	}

	dtPolyRef RandomPoly;
	float RandomPoint[3];

	dtStatus foundRandomPointResult = m_navQuery->findRandomPointAroundCircleIgnoreReachability(FoundPoly, NavNearest, MaxRadius, m_navFilter, frand, &RandomPoly, RandomPoint);


	if (dtStatusFailed(foundRandomPointResult))
	{
		return ZERO_VECTOR;
	}

	Result.x = RandomPoint[0];
	Result.y = -RandomPoint[2];
	Result.z = RandomPoint[1];

	return Result;
}

Vector UTIL_GetRandomPointOnNavmeshInDonut(const int NavProfile, const Vector origin, const float MinRadius, const float MaxRadius)
{
	int maxIterations = 0;
	float MinRadiusSq = sqrf(MinRadius);

	while (maxIterations < 100)
	{
		Vector StartPoint = UTIL_GetRandomPointOnNavmeshInRadius(NavProfile, origin, MaxRadius);

		if (vDist2DSq(StartPoint, origin) > MinRadiusSq)
		{
			return StartPoint;
		}

		maxIterations++;
	}

	return ZERO_VECTOR;
}

Vector UTIL_GetRandomPointOnNavmeshInDonutIgnoreReachability(const int NavProfile, const Vector origin, const float MinRadius, const float MaxRadius)
{
	int maxIterations = 0;
	float MinRadiusSq = sqrf(MinRadius);

	while (maxIterations < 100)
	{
		Vector StartPoint = UTIL_GetRandomPointOnNavmeshInRadiusIgnoreReachability(NavProfile, origin, MaxRadius);

		if (vDist2DSq(StartPoint, origin) > MinRadiusSq)
		{
			return StartPoint;
		}

		maxIterations++;
	}

	return ZERO_VECTOR;
}

static float frand()
{
	return (float)rand() / (float)RAND_MAX;
}

dtStatus FindPhaseGatePathToPoint(const int NavProfileIndex, Vector FromLocation, Vector ToLocation, bot_path_node* path, int* pathSize, float MaxAcceptableDistance)
{
	*pathSize = 0;

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery || !FromLocation || !ToLocation)
	{
		return DT_FAILURE;
	}

	bot_path_node PathToPhaseStart[MAX_PATH_SIZE];
	memset(PathToPhaseStart, 0, sizeof(PathToPhaseStart));
	int PhaseStartPathSize = 0;

	bot_path_node PathToFinalDestination[MAX_PATH_SIZE];
	memset(PathToFinalDestination, 0, sizeof(PathToFinalDestination));
	int PhaseEndPathSize = 0;

	edict_t* StartPhaseGate = nullptr;
	edict_t* EndPhaseGate = nullptr;

	StartPhaseGate = UTIL_GetNearestStructureIndexOfType(FromLocation, STRUCTURE_MARINE_PHASEGATE, UTIL_MetresToGoldSrcUnits(100.0f), true, false);
	EndPhaseGate = UTIL_GetNearestStructureIndexOfType(ToLocation, STRUCTURE_MARINE_PHASEGATE, UTIL_MetresToGoldSrcUnits(100.0f), true, false);

	if (!StartPhaseGate || !EndPhaseGate || (StartPhaseGate == EndPhaseGate)) { return DT_FAILURE; }

	float TotalDist = vDist2DSq(FromLocation, StartPhaseGate->v.origin) + vDist2DSq(EndPhaseGate->v.origin, ToLocation);

	if (TotalDist > vDist2DSq(FromLocation, ToLocation)) { return DT_FAILURE; }

	dtStatus RouteToFirstPhaseGate = FindPathClosestToPoint(NavProfileIndex, FromLocation, StartPhaseGate->v.origin, PathToPhaseStart, &PhaseStartPathSize, max_player_use_reach);

	if (dtStatusFailed(RouteToFirstPhaseGate))
	{
		return DT_FAILURE;
	}

	dtStatus RouteToFinalPoint = FindPathClosestToPoint(NavProfileIndex, EndPhaseGate->v.origin, ToLocation, PathToFinalDestination, &PhaseEndPathSize, MaxAcceptableDistance);

	if (dtStatusFailed(RouteToFinalPoint))
	{
		return DT_FAILURE;
	}

	// Now we join together the path to the starting phase gate and the path from the phase destination to the end, and add the phase itself in the middle

	int CurrPathIndex = 0;

	for (int i = 0; i < PhaseStartPathSize; i++)
	{
		memcpy(&path[CurrPathIndex++], &PathToPhaseStart[i], sizeof(bot_path_node));
	}

	// Add a node to inform the bot they have to use the phase gate
	path[CurrPathIndex].Location = EndPhaseGate->v.origin + Vector(0.0f, 0.0f, 10.0f);
	path[CurrPathIndex].area = SAMPLE_POLYAREA_PHASEGATE;
	path[CurrPathIndex].flag = SAMPLE_POLYFLAGS_PHASEGATE;
	path[CurrPathIndex].poly = UTIL_GetNearestPolyRefForEntity(EndPhaseGate);
	path[CurrPathIndex].requiredZ = EndPhaseGate->v.origin.z;

	CurrPathIndex++;

	// Append the path from the destination phase to the end
	for (int i = 1; i < PhaseEndPathSize; i++)
	{
		memcpy(&path[CurrPathIndex++], &PathToFinalDestination[i], sizeof(bot_path_node));
	}

	*pathSize = CurrPathIndex;

	return DT_SUCCESS;
}

// Special path finding that takes flight movement into account
dtStatus FindFlightPathToPoint(const int NavProfileIndex, Vector FromLocation, Vector ToLocation, bot_path_node* path, int* pathSize, float MaxAcceptableDistance)
{
	if (NavProfileIndex < 0) { return DT_FAILURE; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery || !m_navMesh || !m_navFilter || !FromLocation || !ToLocation)
	{
		return DT_FAILURE;
	}

	float pStartPos[3] = { FromLocation.x, FromLocation.z, -FromLocation.y };
	float pEndPos[3] = { ToLocation.x, ToLocation.z, -ToLocation.y };

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATH_POLY];
	dtPolyRef StraightPolyPath[MAX_PATH_SIZE];
	int nPathCount = 0;
	float StraightPath[MAX_PATH_SIZE * 3];
	unsigned char straightPathFlags[MAX_PATH_SIZE];
	memset(straightPathFlags, 0, sizeof(straightPathFlags));
	int nVertCount = 0;

	// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, pExtents, m_navFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly start failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, pExtents, m_navFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly end failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, m_navFilter, PolyPath, &nPathCount, MAX_PATH_POLY);

	if (PolyPath[nPathCount - 1] != EndPoly)
	{
		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[nPathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(MaxAcceptableDistance))
		{
			return DT_FAILURE;
		}
		else
		{
			dtVcopy(EndNearest, epos);
		}
	}

	status = m_navQuery->findStraightPath(StartNearest, EndNearest, PolyPath, nPathCount, StraightPath, straightPathFlags, StraightPolyPath, &nVertCount, MAX_PATH_SIZE, DT_STRAIGHTPATH_AREA_CROSSINGS);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return (status & DT_STATUS_DETAIL_MASK); // couldn't create a path
	}

	if (nVertCount == 0)
	{
		return DT_FAILURE; // couldn't find a path
	}

	int pathLengthInBytes = MAX_PATH_SIZE * sizeof(bot_path_node);
	memset(path, 0, pathLengthInBytes);

	unsigned char CurrArea;
	unsigned char ThisArea;

	m_navMesh->getPolyArea(StraightPolyPath[0], &CurrArea);

	// At this point we have our path.  Copy it to the path store
	int nIndex = 0;
	TraceResult hit;
	Vector TraceStart;

	int NumPathPoints = nVertCount;
	int CurrentPathPoint = 0;

	for (int nVert = 0; nVert < nVertCount; nVert++)
	{
		Vector NextPathPoint = ZERO_VECTOR;
		Vector PrevPoint = (CurrentPathPoint > 0) ? path[(CurrentPathPoint - 1)].Location : ZERO_VECTOR;

		// The path point output by Detour uses the OpenGL, right-handed coordinate system. Convert to Goldsrc coordinates
		NextPathPoint.x = StraightPath[nIndex++];
		NextPathPoint.z = StraightPath[nIndex++];
		NextPathPoint.y = -StraightPath[nIndex++];

		m_navMesh->getPolyArea(StraightPolyPath[nVert], &ThisArea);

		if (ThisArea == SAMPLE_POLYAREA_GROUND || ThisArea == SAMPLE_POLYAREA_CROUCH)
		{
			NextPathPoint = UTIL_AdjustPointAwayFromNavWall(NextPathPoint, 16.0f);
		}

		TraceStart = NextPathPoint;

		UTIL_TraceLine(TraceStart, (TraceStart - Vector(0.0f, 0.0f, 100.0f)), ignore_monsters, ignore_glass, nullptr, &hit);

		if (hit.flFraction < 1.0f)
		{
			NextPathPoint = hit.vecEndPos + Vector(0.0f, 0.0f, 20.0f);
		}

		float NewRequiredZ = NextPathPoint.z;

		if (CurrArea == SAMPLE_POLYAREA_WALLCLIMB || CurrArea == SAMPLE_POLYAREA_LADDER)
		{
			NewRequiredZ = UTIL_FindZHeightForWallClimb(PrevPoint, NextPathPoint, head_hull);
			
			Vector ClimbStartPoint = Vector(PrevPoint.x, PrevPoint.y, NewRequiredZ);

			path[CurrentPathPoint].requiredZ = ClimbStartPoint.z;
			path[CurrentPathPoint].Location = ClimbStartPoint;
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;

			Vector ClimbEndPoint = Vector(NextPathPoint.x, NextPathPoint.y, NewRequiredZ);

			path[CurrentPathPoint].requiredZ = ClimbEndPoint.z;
			path[CurrentPathPoint].Location = ClimbEndPoint;
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;	
		}

		if (CurrArea == SAMPLE_POLYAREA_JUMP || CurrArea == SAMPLE_POLYAREA_HIGHJUMP)
		{
			float MaxHeight = fmaxf(PrevPoint.z, NextPathPoint.z);
			MaxHeight += 60.0f;

			path[CurrentPathPoint].requiredZ = MaxHeight;
			path[CurrentPathPoint].Location = Vector(PrevPoint.x, PrevPoint.y, MaxHeight);
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;

			path[CurrentPathPoint].requiredZ = MaxHeight;
			path[CurrentPathPoint].Location = Vector(NextPathPoint.x, NextPathPoint.y, MaxHeight);
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;
		}

		if (CurrArea == SAMPLE_POLYAREA_FALL || CurrArea == SAMPLE_POLYAREA_HIGHFALL)
		{
			Vector MidPoint = PrevPoint + ((NextPathPoint - PrevPoint) * 0.5f);

			path[CurrentPathPoint].requiredZ = PrevPoint.z;
			path[CurrentPathPoint].Location = Vector(MidPoint.x, MidPoint.y, PrevPoint.z);
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;

			path[CurrentPathPoint].requiredZ = NextPathPoint.z;
			path[CurrentPathPoint].Location = Vector(MidPoint.x, MidPoint.y, NextPathPoint.z);
			path[CurrentPathPoint].flag = straightPathFlags[nVert];
			path[CurrentPathPoint].area = CurrArea;
			path[CurrentPathPoint].poly = StraightPolyPath[nVert];

			CurrentPathPoint++;
			NumPathPoints++;
		}

		path[CurrentPathPoint].requiredZ = NextPathPoint.z;
		path[CurrentPathPoint].Location = NextPathPoint;
		path[CurrentPathPoint].flag = straightPathFlags[nVert];
		path[CurrentPathPoint].area = CurrArea;
		path[CurrentPathPoint].poly = StraightPolyPath[nVert];

		CurrArea = ThisArea;

		CurrentPathPoint++;
	}

	*pathSize = NumPathPoints;

	return DT_SUCCESS;
}

Vector UTIL_FindHighestSuccessfulTracePoint(const Vector TraceFrom, const Vector TargetPoint, const Vector NextPoint, const float IterationStep, const float MinIdealHeight, const float MaxHeight)
{

	Vector OriginTrace = TraceFrom;
	float AddedHeight = 0.0f;

	bool bFoundInitialPoint = false;
	Vector CurrentHighest = ZERO_VECTOR;
	
	int NumIterations = (int)ceilf(MaxHeight / IterationStep);

	Vector CurrentTarget = TargetPoint;

	for (int i = 0; i <= NumIterations; i++)
	{
		if (!UTIL_QuickTrace(nullptr, TargetPoint, CurrentTarget)) { return CurrentHighest; }

		if (!UTIL_QuickHullTrace(nullptr, OriginTrace, CurrentTarget, head_hull))
		{
			if (bFoundInitialPoint) { break; }
		}
		else
		{
			bFoundInitialPoint = true;
			if (AddedHeight >= MinIdealHeight)
			{
				return CurrentTarget;
			}
			else
			{
				if (NextPoint != ZERO_VECTOR && UTIL_QuickHullTrace(nullptr, CurrentTarget, NextPoint, head_hull))
				{
					CurrentHighest = CurrentTarget;
				}
				
			}
			
		}	

		CurrentTarget.z += IterationStep;
		AddedHeight += IterationStep;
	}

	return CurrentHighest;
}

dtStatus FindPathClosestToPoint(const int NavProfileIndex, const Vector FromLocation, const Vector ToLocation, bot_path_node* path, int* pathSize, float MaxAcceptableDistance)
{
	if (NavProfileIndex < 0) { return DT_FAILURE; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery || !m_navMesh || !m_navFilter || !FromLocation || !ToLocation)
	{
		return DT_FAILURE;
	}

	float pStartPos[3] = { FromLocation.x, FromLocation.z, -FromLocation.y };
	float pEndPos[3] = { ToLocation.x, ToLocation.z, -ToLocation.y };

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATH_POLY];
	dtPolyRef StraightPolyPath[MAX_PATH_SIZE];
	int nPathCount = 0;
	float StraightPath[MAX_PATH_SIZE * 3];
	unsigned char straightPathFlags[MAX_PATH_SIZE];
	memset(straightPathFlags, 0, sizeof(straightPathFlags));
	int nVertCount = 0;

	// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, pExtents, m_navFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly start failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, pExtents, m_navFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly end failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, m_navFilter, PolyPath, &nPathCount, MAX_PATH_POLY);

	if (PolyPath[nPathCount - 1] != EndPoly)
	{
		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[nPathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(MaxAcceptableDistance))
		{
			return DT_FAILURE;
		}
		else
		{
			dtVcopy(EndNearest, epos);
		}
	}

	status = m_navQuery->findStraightPath(StartNearest, EndNearest, PolyPath, nPathCount, StraightPath, straightPathFlags, StraightPolyPath, &nVertCount, MAX_PATH_SIZE, DT_STRAIGHTPATH_AREA_CROSSINGS);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return (status & DT_STATUS_DETAIL_MASK); // couldn't create a path
	}

	if (nVertCount == 0)
	{
		return DT_FAILURE; // couldn't find a path
	}

	int pathLengthInBytes = MAX_PATH_SIZE * sizeof(bot_path_node);
	memset(path, 0, pathLengthInBytes);

	unsigned char CurrArea;
	unsigned char ThisArea;

	m_navMesh->getPolyArea(StraightPolyPath[0], &CurrArea);

	// At this point we have our path.  Copy it to the path store
	int nIndex = 0;
	TraceResult hit;
	Vector TraceStart;

	Vector NodeFromLocation = ZERO_VECTOR;

	for (int nVert = 0; nVert < nVertCount; nVert++)
	{
		path[(nVert)].FromLocation = NodeFromLocation;

		path[(nVert)].Location.x = StraightPath[nIndex++];
		path[(nVert)].Location.z = StraightPath[nIndex++];
		path[(nVert)].Location.y = -StraightPath[nIndex++];

		 m_navMesh->getPolyArea(StraightPolyPath[nVert], &ThisArea);

		if (ThisArea == SAMPLE_POLYAREA_GROUND || ThisArea == SAMPLE_POLYAREA_CROUCH)
		{
			path[(nVert)].Location = UTIL_AdjustPointAwayFromNavWall(path[(nVert)].Location, 16.0f);
		}

		TraceStart.x = path[(nVert)].Location.x;
		TraceStart.y = path[(nVert)].Location.y;
		TraceStart.z = path[(nVert)].Location.z;

		UTIL_TraceLine(TraceStart, (TraceStart - Vector(0.0f, 0.0f, 100.0f)), ignore_monsters, ignore_glass, nullptr, &hit);

		if (hit.flFraction < 1.0f)
		{
			path[(nVert)].Location = hit.vecEndPos;
			
			if (CurrArea != SAMPLE_POLYAREA_JUMP)
			{
				path[(nVert)].Location.z += 20.0f;
			}
		}

		path[(nVert)].requiredZ = path[(nVert)].Location.z;

		if (CurrArea == SAMPLE_POLYAREA_WALLCLIMB || CurrArea == SAMPLE_POLYAREA_LADDER)
		{
			float NewRequiredZ = UTIL_FindZHeightForWallClimb(path[(nVert - 1)].Location, path[(nVert)].Location, head_hull);
			path[(nVert)].requiredZ = fmaxf(NewRequiredZ, path[(nVert)].Location.z);

			if (CurrArea == SAMPLE_POLYAREA_LADDER)
			{
				path[(nVert)].requiredZ += 5.0f;
			}

		}
		else
		{
			path[(nVert)].requiredZ = path[(nVert)].Location.z;
		}

		path[(nVert)].flag = straightPathFlags[nVert];
		path[(nVert)].area = CurrArea;
		path[(nVert)].poly = StraightPolyPath[nVert];

		CurrArea = ThisArea;

		NodeFromLocation = path[(nVert)].Location;
	}

	*pathSize = nVertCount;

	return DT_SUCCESS;
}

dtStatus FindDetailedPathClosestToPoint(const int NavProfileIndex, const Vector FromLocation, const Vector ToLocation, bot_path_node* path, int* pathSize, float MaxAcceptableDistance)
{
	if (NavProfileIndex < 0) { return DT_FAILURE; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery || !m_navMesh || !m_navFilter || !FromLocation || !ToLocation)
	{
		return DT_FAILURE;
	}

	float pStartPos[3] = { FromLocation.x, FromLocation.z, -FromLocation.y };
	float pEndPos[3] = { ToLocation.x, ToLocation.z, -ToLocation.y };

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATH_POLY];
	dtPolyRef StraightPolyPath[MAX_PATH_SIZE];
	int nPathCount = 0;
	float StraightPath[MAX_PATH_SIZE * 3];
	unsigned char straightPathFlags[MAX_PATH_SIZE];
	memset(straightPathFlags, 0, sizeof(straightPathFlags));
	int nVertCount = 0;

	// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, pExtents, m_navFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly start failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, pExtents, m_navFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly end failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, m_navFilter, PolyPath, &nPathCount, MAX_PATH_POLY);

	if (PolyPath[nPathCount - 1] != EndPoly)
	{
		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[nPathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(MaxAcceptableDistance))
		{
			return DT_FAILURE;
		}
		else
		{
			dtVcopy(EndNearest, epos);
		}
	}

	status = m_navQuery->findStraightPath(StartNearest, EndNearest, PolyPath, nPathCount, StraightPath, straightPathFlags, StraightPolyPath, &nVertCount, MAX_PATH_SIZE, DT_STRAIGHTPATH_ALL_CROSSINGS);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return (status & DT_STATUS_DETAIL_MASK); // couldn't create a path
	}

	if (nVertCount == 0)
	{
		return DT_FAILURE; // couldn't find a path
	}

	int pathLengthInBytes = MAX_PATH_SIZE * sizeof(bot_path_node);
	memset(path, 0, pathLengthInBytes);

	unsigned char CurrArea;
	unsigned char ThisArea;

	m_navMesh->getPolyArea(StraightPolyPath[0], &CurrArea);

	// At this point we have our path.  Copy it to the path store
	int nIndex = 0;
	TraceResult hit;
	Vector TraceStart;

	for (int nVert = 0; nVert < nVertCount; nVert++)
	{
		path[(nVert)].Location.x = StraightPath[nIndex++];
		path[(nVert)].Location.z = StraightPath[nIndex++];
		path[(nVert)].Location.y = -StraightPath[nIndex++];

		m_navMesh->getPolyArea(StraightPolyPath[nVert], &ThisArea);

		if (ThisArea == SAMPLE_POLYAREA_GROUND || ThisArea == SAMPLE_POLYAREA_CROUCH)
		{
			path[(nVert)].Location = UTIL_AdjustPointAwayFromNavWall(path[(nVert)].Location, 16.0f);
		}

		TraceStart.x = path[(nVert)].Location.x;
		TraceStart.y = path[(nVert)].Location.y;
		TraceStart.z = path[(nVert)].Location.z;

		UTIL_TraceLine(TraceStart, (TraceStart - Vector(0.0f, 0.0f, 100.0f)), ignore_monsters, ignore_glass, nullptr, &hit);

		if (hit.flFraction < 1.0f)
		{
			bool isCrouchedArea = (CurrArea == SAMPLE_POLYAREA_CROUCH);

			path[(nVert)].Location = hit.vecEndPos + Vector(0.0f, 0.0f, 18.0f);
		}

		path[(nVert)].requiredZ = path[(nVert)].Location.z;

		if (CurrArea == SAMPLE_POLYAREA_WALLCLIMB || CurrArea == SAMPLE_POLYAREA_LADDER)
		{
			float NewRequiredZ = UTIL_FindZHeightForWallClimb(path[(nVert - 1)].Location, path[(nVert)].Location, head_hull);
			path[(nVert)].requiredZ = fmaxf(NewRequiredZ, path[(nVert)].Location.z);

			if (CurrArea == SAMPLE_POLYAREA_LADDER)
			{
				path[(nVert)].requiredZ += 5.0f;
			}

		}
		else
		{
			path[(nVert)].requiredZ = path[(nVert)].Location.z;
		}

		path[(nVert)].flag = straightPathFlags[nVert];
		path[(nVert)].area = CurrArea;
		path[(nVert)].poly = StraightPolyPath[nVert];

		CurrArea = ThisArea;
	}

	*pathSize = nVertCount;

	return DT_SUCCESS;
}

dtStatus FindPathClosestToPoint(bot_t* pBot, const BotMoveStyle MoveStyle, const Vector FromLocation, const Vector ToLocation, bot_path_node* path, int* pathSize, float MaxAcceptableDistance)
{
	if (!pBot) { return DT_FAILURE; }

	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MoveStyle);

	if (NavProfileIndex < 0) { return DT_FAILURE; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);


	if (!m_navQuery || !m_navMesh || !m_navFilter || !FromLocation || !ToLocation)
	{
		return DT_FAILURE;
	}

	if (IsPlayerMarine(pBot->pEdict) && UTIL_GetNumBuiltStructuresOfType(STRUCTURE_MARINE_PHASEGATE) > 1)
	{
		dtStatus PhaseStatus = FindPhaseGatePathToPoint(NavProfileIndex, pBot->pEdict->v.origin, ToLocation, path, pathSize, MaxAcceptableDistance);

		if (dtStatusSucceed(PhaseStatus))
		{
			pBot->BotNavInfo.CurrentPathPoint = 1;
			return DT_SUCCESS;
		}
	}

	float pStartPos[3] = { FromLocation.x, FromLocation.z, -FromLocation.y };
	float pEndPos[3] = { ToLocation.x, ToLocation.z, -ToLocation.y };

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATH_POLY];
	dtPolyRef StraightPolyPath[MAX_PATH_SIZE];
	int nPathCount = 0;
	float StraightPath[MAX_PATH_SIZE * 3];
	unsigned char straightPathFlags[MAX_PATH_SIZE];
	memset(straightPathFlags, 0, sizeof(straightPathFlags));
	int nVertCount = 0;

	// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, pExtents, m_navFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly start failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, pExtents, m_navFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		//BotSay(pBot, "findNearestPoly end failed!");
		return (status & DT_STATUS_DETAIL_MASK); // couldn't find a polygon
	}

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, m_navFilter, PolyPath, &nPathCount, MAX_PATH_POLY);

	if (PolyPath[nPathCount - 1] != EndPoly)
	{
		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[nPathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(MaxAcceptableDistance))
		{
			return DT_FAILURE;
		}
		else
		{
			dtVcopy(EndNearest, epos);
		}
	}

	status = m_navQuery->findStraightPath(StartNearest, EndNearest, PolyPath, nPathCount, StraightPath, straightPathFlags, StraightPolyPath, &nVertCount, MAX_PATH_SIZE, DT_STRAIGHTPATH_AREA_CROSSINGS);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return (status & DT_STATUS_DETAIL_MASK); // couldn't create a path
	}

	if (nVertCount == 0)
	{
		return DT_FAILURE; // couldn't find a path
	}

	int pathLengthInBytes = MAX_PATH_SIZE * sizeof(bot_path_node);
	memset(path, 0, pathLengthInBytes);

	unsigned char CurrArea;

	m_navMesh->getPolyArea(StraightPolyPath[0], &CurrArea);

	// At this point we have our path.  Copy it to the path store
	int nIndex = 0;
	TraceResult hit;
	Vector TraceStart;

	Vector NodeFromLocation = Vector(StartNearest[0], -StartNearest[2], StartNearest[1]);

	for (int nVert = 0; nVert < nVertCount; nVert++)
	{
		path[(nVert)].FromLocation = NodeFromLocation;

		// The nav mesh doesn't always align perfectly with the floor, so align each nav point with the floor after generation
		path[(nVert)].Location.x = StraightPath[nIndex++];
		path[(nVert)].Location.z = StraightPath[nIndex++];
		path[(nVert)].Location.y = -StraightPath[nIndex++];

		path[(nVert)].Location = UTIL_AdjustPointAwayFromNavWall(path[(nVert)].Location, 16.0f);

		TraceStart.x = path[(nVert)].Location.x;
		TraceStart.y = path[(nVert)].Location.y;
		TraceStart.z = path[(nVert)].Location.z + 18.0f;

		if (CurrArea != SAMPLE_POLYAREA_JUMP || path[(nVert)].FromLocation.z > path[(nVert)].Location.z)
		{

			UTIL_TraceHull(TraceStart, (TraceStart - Vector(0.0f, 0.0f, 100.0f)), ignore_monsters, head_hull, nullptr, &hit);

			if (hit.flFraction < 1.0f && !hit.fStartSolid)
			{
				bool isCrouchedArea = (CurrArea == SAMPLE_POLYAREA_CROUCH);

				path[(nVert)].Location = hit.vecEndPos + Vector(0.0f, 0.0f, 2.0f);
			}
		}
		else
		{
			UTIL_TraceLine(TraceStart, (TraceStart - Vector(0.0f, 0.0f, 50.0f)), ignore_monsters, nullptr, &hit);

			if (hit.flFraction < 1.0f && !hit.fStartSolid)
			{
				path[(nVert)].Location = hit.vecEndPos;
			}
		}

		// End alignment to floor

		// For ladders and wall climbing, calculate the climb height needed to complete the move.
		// This what allows bots to climb over railings without having to explicitly place nav points on the railing itself
		path[(nVert)].requiredZ = path[(nVert)].Location.z;

		if (CurrArea == SAMPLE_POLYAREA_WALLCLIMB || CurrArea == SAMPLE_POLYAREA_LADDER)
		{
			int HullNum = GetPlayerHullIndex(pBot->pEdict, false);
			float NewRequiredZ = UTIL_FindZHeightForWallClimb(path[(nVert - 1)].Location, path[(nVert)].Location, HullNum);
			path[(nVert)].requiredZ = fmaxf(NewRequiredZ, path[(nVert)].Location.z);

			if (CurrArea == SAMPLE_POLYAREA_LADDER)
			{
				path[(nVert)].requiredZ += 5.0f;
			}

		}
		else
		{
			path[(nVert)].requiredZ = path[(nVert)].Location.z;
		}

		path[(nVert)].flag = straightPathFlags[nVert];
		path[(nVert)].area = CurrArea;
		path[(nVert)].poly = StraightPolyPath[nVert];

		m_navMesh->getPolyArea(StraightPolyPath[nVert], &CurrArea);

		NodeFromLocation = path[(nVert)].Location;
	}

	*pathSize = nVertCount;

	return DT_SUCCESS;
}

bool UTIL_PointIsReachable(const int NavProfileIndex, const Vector FromLocation, const Vector ToLocation, const float MaxAcceptableDistance)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery || !FromLocation || !ToLocation)
	{
		return false;
	}

	float pStartPos[3] = { FromLocation.x, FromLocation.z, -FromLocation.y };
	float pEndPos[3] = { ToLocation.x, ToLocation.z, -ToLocation.y };

	dtStatus status;
	dtPolyRef StartPoly;
	float StartNearest[3];
	dtPolyRef EndPoly;
	float EndNearest[3];
	dtPolyRef PolyPath[MAX_PATH_POLY];
	int nPathCount = 0;

	float searchExtents[3] = { MaxAcceptableDistance, 50.0f, MaxAcceptableDistance };

	// find the start polygon
	status = m_navQuery->findNearestPoly(pStartPos, searchExtents, m_navFilter, &StartPoly, StartNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return false; // couldn't find a polygon
	}

	// find the end polygon
	status = m_navQuery->findNearestPoly(pEndPos, searchExtents, m_navFilter, &EndPoly, EndNearest);
	if ((status & DT_FAILURE) || (status & DT_STATUS_DETAIL_MASK))
	{
		return false; // couldn't find a polygon
	}

	status = m_navQuery->findPath(StartPoly, EndPoly, StartNearest, EndNearest, m_navFilter, PolyPath, &nPathCount, MAX_PATH_POLY);

	if (nPathCount == 0)
	{
		return false; // couldn't find a path
	}

	if (PolyPath[nPathCount - 1] != EndPoly)
	{
		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[nPathCount - 1], EndNearest, epos, 0);

		return (dtVdistSqr(EndNearest, epos) <= sqrf(MaxAcceptableDistance));

	}

	return true;
}

bool HasBotReachedPathPoint(const bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0)
	{
		return true;
	}

	Vector CurrentPos = (pBot->BotNavInfo.IsOnGround) ? pBot->pEdict->v.origin : pBot->CurrentFloorPosition;

	edict_t* pEdict = pBot->pEdict;

	int CurrentNavArea = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;
	Vector CurrentMoveDest = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;
	Vector PrevMoveDest = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location;

	bool bIsAtFinalPathPoint = (pBot->BotNavInfo.CurrentPathPoint == (pBot->BotNavInfo.PathSize - 1));

	Vector ClosestPointToPath = vClosestPointOnLine2D(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location, pEdict->v.origin);

	bool bDestIsDirectlyReachable = UTIL_PointIsDirectlyReachable(CurrentPos, CurrentMoveDest);
	bool bAtOrPastDestination = vEquals2D(ClosestPointToPath, CurrentMoveDest, 1.0f) && bDestIsDirectlyReachable;

	dtPolyRef BotPoly = pBot->BotNavInfo.CurrentPoly;
	dtPolyRef DestinationPoly = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].poly;

	float playerRadius = GetPlayerRadius(pEdict);

	switch (CurrentNavArea)
	{
	case SAMPLE_POLYAREA_GROUND:
		if (!bIsAtFinalPathPoint)
		{
			return (bAtOrPastDestination || (vDist2D(pEdict->v.origin, CurrentMoveDest) <= 8.0f && (fabs(pBot->CurrentFloorPosition.z - CurrentMoveDest.z) < 50.0f)));
		}
		else
		{
			return ((vDist2D(pEdict->v.origin, CurrentMoveDest) < playerRadius && bDestIsDirectlyReachable) || bAtOrPastDestination);
		}
	case SAMPLE_POLYAREA_CROUCH:
		return (vDist2D(pEdict->v.origin, CurrentMoveDest) < playerRadius && bDestIsDirectlyReachable);
	case SAMPLE_POLYAREA_BLOCKED:
		return bAtOrPastDestination;
	case SAMPLE_POLYAREA_FALL:
	case SAMPLE_POLYAREA_HIGHFALL:
	case SAMPLE_POLYAREA_JUMP:
	case SAMPLE_POLYAREA_HIGHJUMP:
		if (!bIsAtFinalPathPoint)
		{
			Vector thisMoveDir = UTIL_GetVectorNormal2D(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location - pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location);
			Vector nextMoveDir = UTIL_GetVectorNormal2D(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].Location - pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location);

			float DirectionDot = UTIL_GetDotProduct(thisMoveDir, nextMoveDir);

			if (DirectionDot >= -0.5f)
			{
				return bAtOrPastDestination && UTIL_PointIsDirectlyReachable(pBot, pBot->CurrentFloorPosition, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].Location) && UTIL_QuickHullTrace(pBot->pEdict, pBot->pEdict->v.origin, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].Location + Vector(0.0f, 0.0f, 10.0f));
			}
			else
			{
				return bAtOrPastDestination && pBot->BotNavInfo.IsOnGround && fabs(pBot->CurrentFloorPosition.z - CurrentMoveDest.z) < 50.0f;
				//return (vDist2D(pEdict->v.origin, CurrentMoveDest) <= playerRadius && (fabs(pBot->CurrentFloorPosition.z - CurrentMoveDest.z) < 50.0f) && pBot->BotNavInfo.IsOnGround);
			}
		}
		else
		{
			return (vDist2D(pEdict->v.origin, CurrentMoveDest) <= playerRadius && (pEdict->v.origin.z - CurrentMoveDest.z) < 50.0f && pBot->BotNavInfo.IsOnGround);
		}
	case SAMPLE_POLYAREA_WALLCLIMB:
		return (bAtOrPastDestination && pBot->CollisionHullTopLocation.z > CurrentMoveDest.z);
	case SAMPLE_POLYAREA_LADDER:
		if (CurrentMoveDest.z > PrevMoveDest.z)
		{
			return ((BotPoly == DestinationPoly) && UTIL_QuickTrace(pEdict, pEdict->v.origin, CurrentMoveDest));
		}
		else
		{
			return (fabs(pBot->CollisionHullBottomLocation.z - CurrentMoveDest.z) < 50.0f);
		}
	default:
		return (bAtOrPastDestination && UTIL_QuickTrace(pEdict, pEdict->v.origin, CurrentMoveDest));
	}

	return false;
}

void CheckAndHandleDoorObstruction(bot_t* pBot, const Vector MoveFrom, const Vector MoveTo)
{

	edict_t* BlockingDoor = UTIL_GetDoorBlockingPathPoint(pBot->pEdict->v.origin, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location, SAMPLE_POLYAREA_GROUND, nullptr);

	if (!FNullEnt(BlockingDoor))
	{
		if (BlockingDoor->v.velocity != ZERO_VECTOR || BlockingDoor->v.avelocity != ZERO_VECTOR) { return; }

		const nav_door* Door = UTIL_GetNavDoorByEdict(BlockingDoor);

		if (Door)
		{
			if (Door->ActivationType == DOOR_USE)
			{
				if (IsPlayerInUseRange(pBot->pEdict, Door->DoorEdict))
				{
					if (pBot->pEdict->v.oldbuttons & IN_DUCK)
					{
						pBot->pEdict->v.button |= IN_DUCK;
					}

					BotUseObject(pBot, Door->DoorEdict, false);
				}

				return;
			}

			if (Door->ActivationType == DOOR_SHOOT)
			{
				BotAttackTarget(pBot, Door->DoorEdict);
				return;
			}

			

			if (Door->ActivationType == DOOR_BUTTON || Door->ActivationType == DOOR_TRIGGER)
			{
				edict_t* Trigger = UTIL_GetNearestDoorTrigger(pBot->pEdict->v.origin, Door, nullptr);

				if (!FNullEnt(Trigger))
				{
					if (Door->ActivationType == DOOR_BUTTON)
					{
						Vector UseLocation = UTIL_GetButtonFloorLocation(pBot->pEdict->v.origin, Trigger);

						TASK_SetUseTask(pBot, &pBot->MoveTask, Trigger, UseLocation, true);
					}
					else
					{
						TASK_SetTouchTask(pBot, &pBot->MoveTask, Trigger, true);
						
					}

					return;
				}
			}
		}
	}

	if (pBot->BotNavInfo.CurrentPathPoint < pBot->BotNavInfo.PathSize - 1)
	{
		BlockingDoor = UTIL_GetDoorBlockingPathPoint(&pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1], nullptr);

		if (!FNullEnt(BlockingDoor))
		{
			if (BlockingDoor->v.velocity != ZERO_VECTOR || BlockingDoor->v.avelocity != ZERO_VECTOR) { return; }

			const nav_door* Door = UTIL_GetNavDoorByEdict(BlockingDoor);

			if (Door)
			{
				if (Door->ActivationType == DOOR_USE)
				{
					if (IsPlayerInUseRange(pBot->pEdict, Door->DoorEdict))
					{
						if (pBot->pEdict->v.oldbuttons & IN_DUCK)
						{
							pBot->pEdict->v.button |= IN_DUCK;
						}

						BotUseObject(pBot, Door->DoorEdict, false);
					}

					return;
				}

				if (Door->ActivationType == DOOR_SHOOT)
				{
					BotAttackTarget(pBot, Door->DoorEdict);
					return;
				}

				if (Door->ActivationType == DOOR_BUTTON || Door->ActivationType == DOOR_TRIGGER)
				{
					edict_t* Trigger = UTIL_GetNearestDoorTrigger(pBot->pEdict->v.origin, Door, nullptr);

					if (!FNullEnt(Trigger))
					{
						if (Door->ActivationType == DOOR_BUTTON)
						{
							Vector UseLocation = UTIL_GetButtonFloorLocation(pBot->pEdict->v.origin, Trigger);
							TASK_SetUseTask(pBot, &pBot->MoveTask, Trigger, UseLocation, true);
						}
						else
						{
							TASK_SetTouchTask(pBot, &pBot->MoveTask, Trigger, true);
						}
						
						return;
					}
				}
			}
		}
	}

	if (pBot->BotNavInfo.CurrentPathPoint < pBot->BotNavInfo.PathSize - 2)
	{
		BlockingDoor = UTIL_GetDoorBlockingPathPoint(&pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 2], nullptr);

		if (!FNullEnt(BlockingDoor))
		{
			if (BlockingDoor->v.velocity != ZERO_VECTOR || BlockingDoor->v.avelocity != ZERO_VECTOR) { return; }

			const nav_door* Door = UTIL_GetNavDoorByEdict(BlockingDoor);

			if (Door)
			{
				if (Door->ActivationType == DOOR_USE)
				{
					if (IsPlayerInUseRange(pBot->pEdict, Door->DoorEdict))
					{
						if (pBot->pEdict->v.oldbuttons & IN_DUCK)
						{
							pBot->pEdict->v.button |= IN_DUCK;
						}

						BotUseObject(pBot, Door->DoorEdict, false);
					}

					return;
				}

				if (Door->ActivationType == DOOR_SHOOT)
				{
					BotAttackTarget(pBot, Door->DoorEdict);
					return;
				}

				if (Door->ActivationType == DOOR_BUTTON || Door->ActivationType == DOOR_TRIGGER)
				{
					edict_t* Trigger = UTIL_GetNearestDoorTrigger(pBot->pEdict->v.origin, Door, nullptr);

					if (!FNullEnt(Trigger))
					{
						if (Door->ActivationType == DOOR_BUTTON)
						{
							Vector UseLocation = UTIL_GetButtonFloorLocation(pBot->pEdict->v.origin, Trigger);

							TASK_SetUseTask(pBot, &pBot->MoveTask, Trigger, UseLocation, true);
						}
						else
						{
							TASK_SetTouchTask(pBot, &pBot->MoveTask, Trigger, true);
						}

						return;
					}
				}

			}
		}
	}
	
}

edict_t* UTIL_GetDoorBlockingPathPoint(bot_path_node* PathNode, edict_t* SearchDoor)
{
	if (!PathNode) { return nullptr; }

	Vector FromLoc = PathNode->FromLocation;
	Vector ToLoc = PathNode->Location;

	TraceResult doorHit;

	if (PathNode->area == SAMPLE_POLYAREA_LADDER || PathNode->area == SAMPLE_POLYAREA_WALLCLIMB)
	{
		Vector TargetLoc = Vector(FromLoc.x, FromLoc.y, PathNode->requiredZ);

		UTIL_TraceLine(FromLoc, TargetLoc, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
			
		}

		Vector TargetLoc2 = Vector(ToLoc.x, ToLoc.y, PathNode->requiredZ);

		UTIL_TraceLine(TargetLoc, TargetLoc2, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName2 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}

	}
	else if (PathNode->area == SAMPLE_POLYAREA_FALL || PathNode->area == SAMPLE_POLYAREA_HIGHFALL)
	{
		Vector TargetLoc = Vector(ToLoc.x, ToLoc.y, FromLoc.z);

		UTIL_TraceLine(FromLoc, TargetLoc, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName3 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}

		UTIL_TraceLine(TargetLoc, ToLoc + Vector(0.0f, 0.0f, 10.0f), ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName4 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}
	}

	UTIL_TraceLine(FromLoc, ToLoc + Vector(0.0f, 0.0f, 10.0f), ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

	if (!FNullEnt(doorHit.pHit))
	{
		const char* HitName5 = STRING(doorHit.pHit->v.classname);
	}

	if (!FNullEnt(SearchDoor))
	{
		if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
	}
	else
	{
		if (!FNullEnt(doorHit.pHit))
		{
			if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
				|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
				|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
			{
				return doorHit.pHit;
			}
		}
	}


	return nullptr;
}

edict_t* UTIL_GetDoorBlockingPathPoint(const Vector FromLocation, const Vector ToLocation, const unsigned char Area, edict_t* SearchDoor)
{

	Vector FromLoc = FromLocation;
	Vector ToLoc = ToLocation;

	TraceResult doorHit;

	if (Area == SAMPLE_POLYAREA_LADDER || Area == SAMPLE_POLYAREA_WALLCLIMB)
	{
		Vector TargetLoc = Vector(FromLoc.x, FromLoc.y, ToLocation.z);

		UTIL_TraceLine(FromLoc, TargetLoc, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}

		}

		Vector TargetLoc2 = Vector(ToLoc.x, ToLoc.y, ToLocation.z);

		UTIL_TraceLine(TargetLoc, TargetLoc2, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName2 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}

	}
	else if (Area == SAMPLE_POLYAREA_FALL || Area == SAMPLE_POLYAREA_HIGHFALL)
	{
		Vector TargetLoc = Vector(ToLoc.x, ToLoc.y, FromLoc.z);

		UTIL_TraceLine(FromLoc, TargetLoc, ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName3 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}

		UTIL_TraceLine(TargetLoc, ToLoc + Vector(0.0f, 0.0f, 10.0f), ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

		if (!FNullEnt(doorHit.pHit))
		{
			const char* HitName4 = STRING(doorHit.pHit->v.classname);
		}

		if (!FNullEnt(SearchDoor))
		{
			if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
		}
		else
		{
			if (!FNullEnt(doorHit.pHit))
			{
				if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
					|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
				{
					return doorHit.pHit;
				}
			}
		}
	}

	UTIL_TraceLine(FromLoc, ToLoc + Vector(0.0f, 0.0f, 10.0f), ignore_monsters, dont_ignore_glass, nullptr, &doorHit);

	if (!FNullEnt(doorHit.pHit))
	{
		const char* HitName5 = STRING(doorHit.pHit->v.classname);
	}

	if (!FNullEnt(SearchDoor))
	{
		if (doorHit.pHit == SearchDoor) { return doorHit.pHit; }
	}
	else
	{
		if (!FNullEnt(doorHit.pHit))
		{
			if (strcmp(STRING(doorHit.pHit->v.classname), "func_door") == 0
				|| strcmp(STRING(doorHit.pHit->v.classname), "func_seethroughdoor") == 0
				|| strcmp(STRING(doorHit.pHit->v.classname), "func_door_rotating") == 0)
			{
				return doorHit.pHit;
			}
		}
	}


	return nullptr;
}

bool UTIL_IsPathBlockedByDoor(const Vector StartLoc, const Vector EndLoc, edict_t* SearchDoor)
{
	Vector ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(EndLoc, MARINE_REGULAR_NAV_PROFILE);

	if (!ValidNavmeshPoint)
	{
		return ZERO_VECTOR;
	}

	bot_path_node Path[MAX_PATH_SIZE];
	memset(Path, 0, sizeof(Path));
	int PathSize = 0;

	// Now we find a path backwards from the valid nav mesh point to our location, trying to get as close as we can to it

	dtStatus PathFindingStatus = FindPathClosestToPoint(MARINE_REGULAR_NAV_PROFILE, StartLoc, ValidNavmeshPoint, Path, &PathSize, 50.0f);

	if (dtStatusSucceed(PathFindingStatus))
	{
		for (int i = 1; i < PathSize; i++)
		{
			if (UTIL_GetDoorBlockingPathPoint(&Path[i], SearchDoor) != nullptr)
			{
				return true;
			}
		}

		return false;
	}

	return true;
}

edict_t* UTIL_GetNearestDoorTrigger(const Vector Location, const nav_door* Door, edict_t* IgnoreTrigger)
{
	if (!Door) { return nullptr; }

	if (Door->NumTriggers == 0) { return nullptr; }

	if (Door->NumTriggers == 1) { return Door->TriggerEdicts[0]; }

	edict_t* NearestTrigger = nullptr;
	float NearestDist = 0.0f;

	Vector DoorLocation = UTIL_GetCentreOfEntity(Door->DoorEdict);

	for (int i = 0; i < Door->NumTriggers; i++)
	{
		if (!FNullEnt(Door->TriggerEdicts[i]) && Door->TriggerEdicts[i] != IgnoreTrigger)
		{
			Vector ButtonLocation = UTIL_GetButtonFloorLocation(Location, Door->TriggerEdicts[i]);

			if (!UTIL_IsPathBlockedByDoor(Location, ButtonLocation, Door->DoorEdict))
			{
				float ThisDist = vDist3DSq(DoorLocation, ButtonLocation);

				if (FNullEnt(NearestTrigger) || ThisDist < NearestDist)
				{
					NearestTrigger = Door->TriggerEdicts[i];
					NearestDist = ThisDist;
				}
				
			}
		}
	}

	return NearestTrigger;
}

void CheckAndHandleBreakableObstruction(bot_t* pBot, const Vector MoveFrom, const Vector MoveTo)
{
	Vector MoveTarget = MoveTo;

	if (MoveTarget.z > pBot->pEdict->v.origin.z)
	{
		MoveTarget.z += 32.0f;
	}
	else
	{
		MoveTarget.z -= 32.0f;
	}

	Vector TraceDir = UTIL_GetVectorNormal(MoveTarget - pBot->pEdict->v.origin);

	Vector TraceEnd = pBot->pEdict->v.origin + (TraceDir * 50.0f);

	bool bBrokenGlass = false;

	TraceResult breakableHit;
	UTIL_TraceLine(pBot->pEdict->v.origin, TraceEnd, ignore_monsters, dont_ignore_glass, pBot->pEdict->v.pContainingEntity, &breakableHit);

	if (breakableHit.flFraction < 1.0f)
	{
		if (strcmp(STRING(breakableHit.pHit->v.classname), "func_breakable") == 0)
		{
			pBot->desiredMovementDir = ZERO_VECTOR;

			bool bIsPlayerMarine = IsPlayerMarine(pBot->pEdict);
			NSWeapon BreakWeapon = (bIsPlayerMarine) ? WEAPON_MARINE_KNIFE : GetBotAlienPrimaryWeapon(pBot);

			pBot->DesiredCombatWeapon = BreakWeapon;

			BotLookAt(pBot, breakableHit.pHit);

			if (GetBotCurrentWeapon(pBot) == BreakWeapon)
			{
				pBot->pEdict->v.button |= IN_ATTACK;
			}

			bBrokenGlass = true;
		}
	}

	if (!bBrokenGlass)
	{
		TraceEnd = pBot->pEdict->v.origin + (pBot->desiredMovementDir * 50.0f);

		UTIL_TraceLine(pBot->pEdict->v.origin - (pBot->desiredMovementDir * 10.0f), TraceEnd, dont_ignore_monsters, dont_ignore_glass, pBot->pEdict->v.pContainingEntity, &breakableHit);

		if (breakableHit.flFraction < 1.0f)
		{
			if (strcmp(STRING(breakableHit.pHit->v.classname), "func_breakable") == 0)
			{
				pBot->desiredMovementDir = ZERO_VECTOR;

				bool bIsPlayerMarine = IsPlayerMarine(pBot->pEdict);
				NSWeapon BreakWeapon = (bIsPlayerMarine) ? WEAPON_MARINE_KNIFE : GetBotAlienPrimaryWeapon(pBot);

				pBot->DesiredCombatWeapon = BreakWeapon;

				BotLookAt(pBot, breakableHit.pHit);

				if (GetBotCurrentWeapon(pBot) == BreakWeapon)
				{
					pBot->pEdict->v.button |= IN_ATTACK;
				}
			}
		}
	}
}

void NewMove(bot_t* pBot)
{

	if (pBot->BotNavInfo.PathSize == 0)
	{
		return;
	}

	SamplePolyAreas CurrentNavArea = (SamplePolyAreas)pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;

	unsigned char NextArea = SAMPLE_POLYAREA_GROUND;

	if (pBot->BotNavInfo.CurrentPathPoint < (pBot->BotNavInfo.PathSize - 1))
	{
		NextArea = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].area;
	}

	Vector MoveFrom = ZERO_VECTOR;

	if (pBot->BotNavInfo.CurrentPathPoint > 0)
	{
		MoveFrom = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location;
	}
	else
	{
		MoveFrom = pBot->pEdict->v.origin;
	}

	Vector MoveTo = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;



	switch (CurrentNavArea)
	{
	case SAMPLE_POLYAREA_GROUND:
	case SAMPLE_POLYAREA_CROUCH:
		GroundMove(pBot, MoveFrom, MoveTo);
		break;
	case SAMPLE_POLYAREA_FALL:
	case SAMPLE_POLYAREA_HIGHFALL:
		FallMove(pBot, MoveFrom, MoveTo);
		break;
	case SAMPLE_POLYAREA_JUMP:
	case SAMPLE_POLYAREA_HIGHJUMP:
		JumpMove(pBot, MoveFrom, MoveTo);
		break;
	case SAMPLE_POLYAREA_BLOCKED:
		BlockedMove(pBot, MoveFrom, MoveTo);
		break;
	case SAMPLE_POLYAREA_WALLCLIMB:
	{
		if (IsPlayerSkulk(pBot->pEdict))
		{
			WallClimbMove(pBot, MoveFrom, MoveTo, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ);
		}
		else
		{
			BlinkClimbMove(pBot, MoveFrom, MoveTo, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ);
		}
	}
	break;
	case SAMPLE_POLYAREA_LADDER:
		LadderMove(pBot, MoveFrom, MoveTo, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ, NextArea);
		break;
	case SAMPLE_POLYAREA_PHASEGATE:
		PhaseGateMove(pBot, MoveFrom, MoveTo);
		break;
	default:
		GroundMove(pBot, MoveFrom, MoveTo);
		break;
	}

	if (vEquals(pBot->LookTargetLocation, ZERO_VECTOR))
	{
		Vector FurthestView = UTIL_GetFurthestVisiblePointOnPath(pBot);

		if (!FurthestView || vDist2DSq(FurthestView, pBot->CurrentEyePosition) < sqrf(200.0f))
		{
			FurthestView = MoveTo;

			Vector LookNormal = UTIL_GetVectorNormal2D(FurthestView - pBot->CurrentEyePosition);

			FurthestView = FurthestView + (LookNormal * 1000.0f);
		}

		BotLookAt(pBot, FurthestView);
	}

	// While moving, check to make sure we're not obstructed by a func_breakable, e.g. vent or window.
	CheckAndHandleBreakableObstruction(pBot, MoveFrom, MoveTo);

	if (gpGlobals->time - pBot->LastUseTime >= 3.0f)
	{
		CheckAndHandleDoorObstruction(pBot, MoveFrom, MoveTo);
	}
	

}

void GroundMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint)
{
	edict_t* pEdict = pBot->pEdict;

	if (pBot->BotNavInfo.IsOnGround)
	{
		pBot->BotNavInfo.LastNavMeshPosition = pBot->CurrentFloorPosition;
		pBot->BotNavInfo.LastPathFollowPosition = pBot->CurrentFloorPosition;
	}

	Vector CurrentPos = (pBot->BotNavInfo.IsOnGround) ? pBot->pEdict->v.origin : pBot->CurrentFloorPosition;

	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - CurrentPos);
	// Same goes for the right vector, might not be the same as the bot's right
	Vector vRight = UTIL_GetVectorNormal(UTIL_GetCrossProduct(vForward, UP_VECTOR));

	bool bAdjustingForCollision = false;

	float PlayerRadius = GetPlayerRadius(pEdict) + 2.0f;

	Vector stTrcLft = CurrentPos - (vRight * PlayerRadius);
	Vector stTrcRt = CurrentPos + (vRight * PlayerRadius);
	Vector endTrcLft = stTrcLft + (vForward * 24.0f);
	Vector endTrcRt = stTrcRt + (vForward * 24.0f);

	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, pBot->BotNavInfo.MoveStyle);

	bool bumpLeft = !UTIL_PointIsDirectlyReachable(NavProfileIndex, stTrcLft, endTrcLft);
	bool bumpRight = !UTIL_PointIsDirectlyReachable(NavProfileIndex, stTrcRt, endTrcRt);

	pBot->desiredMovementDir = vForward;

	if (bumpRight && !bumpLeft)
	{
		pBot->desiredMovementDir = pBot->desiredMovementDir - vRight;
	}
	else if (bumpLeft && !bumpRight)
	{
		pBot->desiredMovementDir = pBot->desiredMovementDir + vRight;
	}
	else if (bumpLeft && bumpRight)
	{
		stTrcLft.z = pBot->pEdict->v.origin.z;
		stTrcRt.z = pBot->pEdict->v.origin.z;
		endTrcLft.z = pBot->pEdict->v.origin.z;
		endTrcRt.z = pBot->pEdict->v.origin.z;

		if (!UTIL_QuickTrace(pBot->pEdict, stTrcLft, endTrcLft))
		{
			pBot->desiredMovementDir = pBot->desiredMovementDir + vRight;
		}
		else
		{
			pBot->desiredMovementDir = pBot->desiredMovementDir - vRight;
		}
	}
	else
	{
		float DistFromLine = vDistanceFromLine2D(StartPoint, EndPoint, CurrentPos);

		if (DistFromLine > 18.0f)
		{
			float modifier = (float)vPointOnLine(StartPoint, EndPoint, CurrentPos);
			pBot->desiredMovementDir = pBot->desiredMovementDir + (vRight * modifier);
		}

		float LeapDist = (IsPlayerSkulk(pEdict)) ? UTIL_MetresToGoldSrcUnits(5.0f) : UTIL_MetresToGoldSrcUnits(2.0f);

		if (IsPlayerFade(pBot->pEdict) && pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_CROUCH)
		{
			LeapDist = UTIL_MetresToGoldSrcUnits(1.0f);
		}

		if (CanBotLeap(pBot) && vDist2DSq(pBot->pEdict->v.origin, EndPoint) > sqrf(LeapDist) && UTIL_PointIsDirectlyReachable(NavProfileIndex, pBot->pEdict->v.origin, EndPoint))
		{
			float CombatWeaponEnergyCost = GetEnergyCostForWeapon(pBot->DesiredCombatWeapon);
			float RequiredEnergy = (CombatWeaponEnergyCost + GetLeapCost(pBot)) - (GetPlayerEnergyRegenPerSecond(pEdict) * 0.5f); // We allow for around .5s of regen time as well

			if (GetPlayerEnergy(pEdict) >= RequiredEnergy)
			{
				Vector CurrVelocity = UTIL_GetVectorNormal2D(pBot->pEdict->v.velocity);

				float MoveDot = UTIL_GetDotProduct2D(CurrVelocity, vForward);

				if (MoveDot >= 0.95f)
				{
					BotLeap(pBot, EndPoint);
				}
			}
		}
	}

	pBot->desiredMovementDir = UTIL_GetVectorNormal2D(pBot->desiredMovementDir);

	bool bCanDuck = (IsPlayerMarine(pBot->pEdict) || IsPlayerFade(pBot->pEdict) || IsPlayerOnos(pBot->pEdict));

	if (!bCanDuck) { return; }

	// If this is a crouch type movement, then crouch
	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_CROUCH)
	{
		pEdict->v.button |= IN_DUCK;
		return;
	}

	// Start ducking early if the next path point after this one is a crouch move
	if ((pBot->BotNavInfo.CurrentPathPoint < (pBot->BotNavInfo.PathSize - 1)) && pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].area == SAMPLE_POLYAREA_CROUCH && vDist2DSq(pEdict->v.origin, EndPoint) < sqrf(50.0f))
	{
		pEdict->v.button |= IN_DUCK;
		return;
	}

	Vector HeadLocation = GetPlayerTopOfCollisionHull(pEdict, false);

	// Also crouch if we have something in our way at head height
	if (!UTIL_QuickTrace(pBot->pEdict, HeadLocation, (HeadLocation + (pBot->desiredMovementDir * 50.0f))))
	{
		pBot->pEdict->v.button |= IN_DUCK;
	}
}

void FallMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint)
{
	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - pBot->pEdict->v.origin);

	if (vEquals(vForward, ZERO_VECTOR))
	{
		vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);
	}

	pBot->desiredMovementDir = vForward;

	if (UTIL_PointIsDirectlyReachable(pBot, EndPoint)) { return; }

	bool bCanDuck = (IsPlayerMarine(pBot->pEdict) || IsPlayerFade(pBot->pEdict) || IsPlayerOnos(pBot->pEdict));

	if (!bCanDuck) { return; }

	Vector HeadLocation = GetPlayerTopOfCollisionHull(pBot->pEdict, false);

	if (!UTIL_QuickTrace(pBot->pEdict, HeadLocation, (HeadLocation + (pBot->desiredMovementDir * 50.0f))))
	{
		pBot->pEdict->v.button |= IN_DUCK;
	}
}

void BlockedMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint)
{

	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);

	pBot->desiredMovementDir = vForward;

	BotJump(pBot);
}

void JumpMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint)
{
	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - pBot->pEdict->v.origin);

	if (vEquals(vForward, ZERO_VECTOR))
	{
		vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);
	}

	pBot->desiredMovementDir = vForward;

	BotJump(pBot);

	bool bCanDuck = (IsPlayerMarine(pBot->pEdict) || IsPlayerFade(pBot->pEdict) || IsPlayerOnos(pBot->pEdict));

	if (!bCanDuck) { return; }

	Vector HeadLocation = GetPlayerTopOfCollisionHull(pBot->pEdict, false);

	if (!UTIL_QuickTrace(pBot->pEdict, HeadLocation, (HeadLocation + (pBot->desiredMovementDir * 50.0f))))
	{
		pBot->pEdict->v.button |= IN_DUCK;
	}
}

void LadderMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint, float RequiredClimbHeight, unsigned char NextArea)
{
	edict_t* pEdict = pBot->pEdict;

	const Vector vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);

	bool bIsGoingUpLadder = (EndPoint.z > StartPoint.z);

	// Onos is buggy as hell getting on and off ladders, ducking seems to help...
	if (IsPlayerOnos(pBot->pEdict))
	{
		pEdict->v.button |= IN_DUCK;
	}


	if (IsPlayerOnLadder(pEdict))
	{
		// We're on the ladder and actively climbing
		const Vector LadderRightNormal = UTIL_GetVectorNormal(UTIL_GetCrossProduct(pBot->CurrentLadderNormal, UP_VECTOR));

		Vector ClimbRightNormal = LadderRightNormal;

		if (bIsGoingUpLadder)
		{
			ClimbRightNormal = -LadderRightNormal;
		}

		if (bIsGoingUpLadder)
		{

			Vector HullTraceTo = EndPoint;
			HullTraceTo.z = pBot->CollisionHullBottomLocation.z;


			// We have reached our desired climb height and want to get off the ladder
			if ((pBot->pEdict->v.origin.z >= RequiredClimbHeight) && UTIL_QuickHullTrace(pEdict, pEdict->v.origin, Vector(EndPoint.x, EndPoint.y, pEdict->v.origin.z), head_hull))
			{
				// Move directly towards the desired get-off point, looking slightly up still
				pBot->desiredMovementDir = vForward;

				Vector LookLocation = EndPoint;
				LookLocation.z = pBot->CurrentEyePosition.z + 64.0f;

				BotMoveLookAt(pBot, LookLocation);

				// If the get-off point is opposite the ladder, then jump to get to it
				if (UTIL_GetDotProduct(pBot->CurrentLadderNormal, vForward) > 0.75f)
				{
					BotJump(pBot);
				}

				if (!IsPlayerGorge(pEdict) && !IsPlayerLerk(pEdict) && !IsPlayerSkulk(pEdict))
				{
					Vector HeadTraceLocation = GetPlayerTopOfCollisionHull(pEdict, false);

					bool bHittingHead = !UTIL_QuickTrace(pBot->pEdict, HeadTraceLocation, HeadTraceLocation + Vector(0.0f, 0.0f, 10.0f));
					bool bClimbIntoVent = (NextArea == SAMPLE_POLYAREA_CROUCH);

					if (bHittingHead || bClimbIntoVent)
					{
						pEdict->v.button |= IN_DUCK;
					}
				}

				return;
			}
			else
			{
				// This is for cases where the ladder physically doesn't reach the desired get-off point and the bot kind of has to "jump" up off the ladder.
				if (pBot->CollisionHullTopLocation.z >= UTIL_GetNearestLadderTopPoint(pEdict).z)
				{
					pBot->desiredMovementDir = vForward;
					// We look up really far to get maximum launch
					BotMoveLookAt(pBot, EndPoint + Vector(0.0f, 0.0f, 100.0f));
					return;
				}

				// Still climbing the ladder. Look up, and move left/right on the ladder to avoid any blockages

				Vector StartLeftTrace = pBot->CollisionHullTopLocation - (ClimbRightNormal * GetPlayerRadius(pEdict));
				Vector StartRightTrace = pBot->CollisionHullTopLocation + (ClimbRightNormal * GetPlayerRadius(pEdict));

				bool bBlockedLeft = !UTIL_QuickTrace(pEdict, StartLeftTrace, StartLeftTrace + Vector(0.0f, 0.0f, 32.0f));
				bool bBlockedRight = !UTIL_QuickTrace(pEdict, StartRightTrace, StartRightTrace + Vector(0.0f, 0.0f, 32.0f));

				// Look up at the top of the ladder

				// If we are blocked going up the ladder, face the ladder and slide left/right to avoid blockage
				if (bBlockedLeft && !bBlockedRight)
				{
					Vector LookLocation = pBot->pEdict->v.origin - (pBot->CurrentLadderNormal * 50.0f);
					LookLocation.z = RequiredClimbHeight + 100.0f;
					BotMoveLookAt(pBot, LookLocation);

					pBot->desiredMovementDir = ClimbRightNormal;
					return;
				}

				if (bBlockedRight && !bBlockedLeft)
				{
					Vector LookLocation = pBot->pEdict->v.origin - (pBot->CurrentLadderNormal * 50.0f);
					LookLocation.z = RequiredClimbHeight + 100.0f;
					BotMoveLookAt(pBot, LookLocation);

					pBot->desiredMovementDir = -ClimbRightNormal;
					return;
				}

				// Crouch if we're hitting our head on a ceiling
				

				if (!IsPlayerGorge(pEdict) && !IsPlayerLerk(pEdict) && !IsPlayerSkulk(pEdict))
				{
					Vector HeadTraceLocation = GetPlayerTopOfCollisionHull(pEdict, false);

					bool bHittingHead = !UTIL_QuickTrace(pBot->pEdict, HeadTraceLocation, HeadTraceLocation + Vector(0.0f, 0.0f, 10.0f));

					if (bHittingHead)
					{
						pEdict->v.button |= IN_DUCK;
					}
				}

				// We're not blocked by anything

				// If the get-off point is to the side, look to the side and climb. Otherwise, face the ladder

				Vector LookLocation = EndPoint;

				float dot = UTIL_GetDotProduct2D(vForward, LadderRightNormal);

				// Get-off point is to the side of the ladder rather than right at the top
				if (fabsf(dot) > 0.5f)
				{
					if (dot > 0.0f)
					{
						LookLocation = pBot->pEdict->v.origin + (LadderRightNormal * 50.0f);
					}
					else
					{
						LookLocation = pBot->pEdict->v.origin - (LadderRightNormal * 50.0f);
					}

				}
				else
				{
					// Get-off point is at the top of the ladder, so face the ladder
					LookLocation = pBot->pEdict->v.origin - (pBot->CurrentLadderNormal * 50.0f);
				}

				LookLocation.z = RequiredClimbHeight + 100.0f;
				BotMoveLookAt(pBot, LookLocation);

				if (RequiredClimbHeight > pBot->pEdict->v.origin.z)
				{
					pBot->desiredMovementDir = -pBot->CurrentLadderNormal;
				}
				else
				{
					pBot->desiredMovementDir = pBot->CurrentLadderNormal;
				}
			}


		}
		else
		{

			// We're going down the ladder

			Vector StartLeftTrace = pBot->CollisionHullBottomLocation - (LadderRightNormal * (GetPlayerRadius(pEdict) + 2.0f));
			Vector StartRightTrace = pBot->CollisionHullBottomLocation + (LadderRightNormal * (GetPlayerRadius(pEdict) + 2.0f));

			bool bBlockedLeft = !UTIL_QuickTrace(pEdict, StartLeftTrace, StartLeftTrace - Vector(0.0f, 0.0f, 32.0f));
			bool bBlockedRight = !UTIL_QuickTrace(pEdict, StartRightTrace, StartRightTrace - Vector(0.0f, 0.0f, 32.0f));

			if (bBlockedLeft)
			{
				pBot->desiredMovementDir = LadderRightNormal;
				return;
			}

			if (bBlockedRight)
			{
				pBot->desiredMovementDir = -LadderRightNormal;
				return;
			}

			if (EndPoint.z > pBot->pEdict->v.origin.z)
			{
				pBot->desiredMovementDir = -pBot->CurrentLadderNormal;
			}
			else
			{
				pBot->desiredMovementDir = pBot->CurrentLadderNormal;
			}

			// We're going down the ladder, look ahead on the path or at the bottom of the ladder if we can't

			Vector FurthestView = UTIL_GetFurthestVisiblePointOnPath(pBot);

			if (!FurthestView)
			{
				FurthestView = EndPoint + (pBot->CurrentLadderNormal * 100.0f);
			}

			BotMoveLookAt(pBot, FurthestView);
		}

		return;
	}

	// We're not yet on the ladder

	// If we're going down the ladder and are approaching it, just keep moving towards it
	if (pBot->BotNavInfo.IsOnGround && !bIsGoingUpLadder)
	{
		Vector ApproachDir = UTIL_GetVectorNormal2D(EndPoint - pBot->pEdict->v.origin);

		float Dot = UTIL_GetDotProduct2D(ApproachDir, vForward);

		if (Dot > 45.0f)
		{
			pBot->desiredMovementDir = vForward;
			pBot->BotNavInfo.bShouldWalk = true;
		}
		else
		{
			Vector nearestLadderPoint = UTIL_GetNearestLadderCentrePoint(pEdict);
			pBot->desiredMovementDir = UTIL_GetVectorNormal2D(nearestLadderPoint - StartPoint);
		}

		return;
	}


	if (bIsGoingUpLadder && (pBot->CollisionHullTopLocation.z > EndPoint.z))
	{
		pBot->desiredMovementDir = vForward;

		if (!UTIL_QuickHullTrace(pEdict, pEdict->v.origin, Vector(EndPoint.x, EndPoint.y, pEdict->v.origin.z)))
		{
			// Gorges can't duck, so they have to jump to get over any barrier
			if (!IsPlayerGorge(pEdict))
			{
				pEdict->v.button |= IN_DUCK;
			}
			else
			{
				BotJump(pBot);
			}
		}

		return;
	}

	Vector nearestLadderTop = UTIL_GetNearestLadderTopPoint(pEdict);

	if (pBot->pEdict->v.origin.z < nearestLadderTop.z)
	{

		Vector nearestLadderPoint = UTIL_GetNearestLadderCentrePoint(pEdict);
		nearestLadderPoint.z = pEdict->v.origin.z;
		pBot->desiredMovementDir = UTIL_GetVectorNormal2D(nearestLadderPoint - pEdict->v.origin);
	}
}

void PhaseGateMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint)
{
	edict_t* NearestPhaseGate = UTIL_GetNearestStructureIndexOfType(pBot->pEdict->v.origin, STRUCTURE_MARINE_PHASEGATE, UTIL_MetresToGoldSrcUnits(2.0f), true, false);

	if (FNullEnt(NearestPhaseGate)) { return; }

	if (IsPlayerInUseRange(pBot->pEdict, NearestPhaseGate))
	{
		BotMoveLookAt(pBot, NearestPhaseGate->v.origin);
		pBot->desiredMovementDir = ZERO_VECTOR;
		BotUseObject(pBot, NearestPhaseGate, false);

		if (vDist2DSq(pBot->pEdict->v.origin, NearestPhaseGate->v.origin) < sqrf(16.0f))
		{
			pBot->desiredMovementDir = UTIL_GetForwardVector2D(NearestPhaseGate->v.angles);
		}

		return;
	}
	else
	{
		pBot->desiredMovementDir = UTIL_GetVectorNormal2D(NearestPhaseGate->v.origin - pBot->pEdict->v.origin);
	}
}

bool IsBotOffPath(const bot_t* pBot)
{
	// Can't be off the path if we don't have one...
	if (pBot->BotNavInfo.PathSize == 0) { return false; }
	

	// If we're trying to use a phase gate, then we're fine as long as there is a phase gate within reach at the start and end teleport points
	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_PHASEGATE)
	{
		if (!UTIL_StructureOfTypeExistsInLocation(STRUCTURE_MARINE_PHASEGATE, pBot->pEdict->v.origin, UTIL_MetresToGoldSrcUnits(2.0f)))
		{
			return true;
		}

		// This checks to ensure the target phase gate hasn't been destroyed since the bot initially calculated its path. If so, then this will force it to calculate a new path
		if (!UTIL_StructureOfTypeExistsInLocation(STRUCTURE_MARINE_PHASEGATE, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location, UTIL_MetresToGoldSrcUnits(2.0f)))
		{
			return true;
		}

		return false;
	}

	edict_t* pEdict = pBot->pEdict;

	Vector MoveTo = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;

	Vector MoveFrom = pBot->CurrentFloorPosition;

	if (pBot->BotNavInfo.CurrentPathPoint > 0)
	{
		MoveFrom = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location;
	}
	else
	{
		if (!UTIL_PointIsDirectlyReachable(MoveFrom, MoveTo))
		{
			return true;
		}
	}

	

	float PlayerRadiusSq = sqrf(GetPlayerRadius(pBot->pEdict));
	float PlayerHeight = GetPlayerHeight(pBot->pEdict, false);

	Vector vForward = UTIL_GetVectorNormal2D(MoveTo - MoveFrom);

	Vector PointOnPath = vClosestPointOnLine2D(MoveFrom, MoveTo, pBot->CurrentFloorPosition);

	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_WALLCLIMB)
	{
		return (vEquals(PointOnPath, MoveTo, 2.0f) && !IsPlayerClimbingWall(pBot->pEdict) && pBot->CollisionHullTopLocation.z < MoveTo.z);
	}


	// Give us a chance to land before deciding we're off the path
	if (!pBot->BotNavInfo.IsOnGround) { return false; }


	// TODO: This sucks
	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_GROUND || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_CROUCH)
	{

		bool bAtMoveStart = vEquals(PointOnPath, MoveFrom, 2.0f);


		// If we're on the from or to move points, but the height is significantly different, we must be under or over the path somehow
		if (bAtMoveStart && fabs(pBot->CurrentFloorPosition.z - MoveFrom.z) > PlayerHeight)
		{
			return true;
		}

		bool bAtMoveEnd = vEquals(PointOnPath, MoveTo, 2.0f);

		if (bAtMoveEnd && fabs(pBot->CurrentFloorPosition.z - MoveTo.z) > PlayerHeight)
		{
			return true;
		}

		float MaxDist = (bAtMoveStart || bAtMoveEnd) ? 50.0f : 200.0f;

		if (vDistanceFromLine2D(MoveFrom, MoveTo, pBot->CurrentFloorPosition) > sqrf(MaxDist))
		{ 
			return true;
		}

		return false;
	}

	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_JUMP || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_HIGHJUMP)
	{
		Vector ExactJumpTarget = UTIL_GetGroundLocation(MoveTo);

		if (pBot->BotNavInfo.IsOnGround && (MoveTo.z - pBot->CurrentFloorPosition.z) > max_player_jump_height)
		{
			return true;
		}

		return false;
	}

	if (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_FALL || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_HIGHFALL)
	{
		if (vEquals(PointOnPath, MoveTo, 2.0f) && fabs(pBot->CurrentFloorPosition.z - MoveTo.z) > PlayerHeight)
		{
			return true;
		}

		return false;
	}

	return false;
}

void BlinkClimbMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint, float RequiredClimbHeight)
{
	edict_t* pEdict = pBot->pEdict;

	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);
	Vector CheckLine = StartPoint + (vForward * 1000.0f);
	Vector MoveDir = UTIL_GetVectorNormal2D(EndPoint - pBot->pEdict->v.origin);

	Vector PointOnMove = vClosestPointOnLine2D(StartPoint, EndPoint, pEdict->v.origin);
	float DistFromLineSq = vDist2DSq(PointOnMove, pEdict->v.origin);

	if (vEquals(PointOnMove, StartPoint, 2.0f) && DistFromLineSq > sqrf(8.0f))
	{
		pBot->desiredMovementDir = UTIL_GetVectorNormal2D(StartPoint - pBot->pEdict->v.origin);
		return;
	}

	pBot->desiredMovementDir = MoveDir;

	// Always duck. It doesn't have any downsides and means we don't have to separately handle vent climbing
	pBot->pEdict->v.button |= IN_DUCK;

	pBot->DesiredMoveWeapon = WEAPON_FADE_BLINK;

	// Wait until we have blink equipped before proceeding
	if (GetBotCurrentWeapon(pBot) != WEAPON_FADE_BLINK) { return; }

	// Only blink if we're below the target climb height
	if (pEdict->v.origin.z < RequiredClimbHeight)
	{
		Vector CurrVelocity = UTIL_GetVectorNormal2D(pBot->pEdict->v.velocity);

		float Dot = UTIL_GetDotProduct2D(MoveDir, CurrVelocity);

		Vector FaceDir = UTIL_GetForwardVector2D(pEdict->v.angles);

		float FaceDot = UTIL_GetDotProduct2D(FaceDir, MoveDir);

		// Don't start blinking unless we're already in the air, or we're moving in the correct direction. Stops fade shooting off sideways when approaching a climb point from the side
		if (FaceDot > 0.9f)
		{
			float ZDiff = fabs(pEdict->v.origin.z - RequiredClimbHeight);

			// We don't want to blast off like a rocket, so only apply enough blink until our upwards velocity is enough to carry us to the desired height
			float DesiredZVelocity = sqrtf(2.0f * GOLDSRC_GRAVITY * (ZDiff + 10.0f));

			if (pBot->pEdict->v.velocity.z < DesiredZVelocity || pBot->pEdict->v.velocity.z < 300.0f)
			{
				BotMoveLookAt(pBot, EndPoint + Vector(0.0f, 0.0f, 100.0f));
				pBot->pEdict->v.button |= IN_ATTACK2;
			}
			else
			{
				Vector LookAtTarget = EndPoint;
				LookAtTarget.z = pBot->CurrentEyePosition.z;
				BotMoveLookAt(pBot, LookAtTarget);
			}
		}
	}
}

void WallClimbMove(bot_t* pBot, const Vector StartPoint, const Vector EndPoint, float RequiredClimbHeight)
{
	edict_t* pEdict = pBot->pEdict;

	Vector vForward = UTIL_GetVectorNormal2D(EndPoint - StartPoint);
	Vector vRight = UTIL_GetVectorNormal(UTIL_GetCrossProduct(vForward, UP_VECTOR));

	pBot->desiredMovementDir = vForward;

	Vector CheckLine = StartPoint + (vForward * 1000.0f);

	float DistFromLine = vDistanceFromLine2D(StartPoint, CheckLine, pEdict->v.origin);

	// Draw an imaginary 2D line between from and to movement, and make sure we're aligned. If we've drifted off to one side, readjust.
	if (DistFromLine > 18.0f)
	{
		float modifier = (float)vPointOnLine(StartPoint, CheckLine, pEdict->v.origin);

		pBot->desiredMovementDir = UTIL_GetVectorNormal2D(pBot->desiredMovementDir + (vRight * modifier));
	}

	// Jump if we're on the floor, to give ourselves a boost and remove that momentary pause while "wall-sticking" mode activates if skulk
	if ((pEdict->v.flags & FL_ONGROUND) && !IsPlayerClimbingWall(pEdict))
	{
		Vector CurrentVelocity = UTIL_GetVectorNormal2D(pBot->pEdict->v.velocity);

		float VelocityDot = UTIL_GetDotProduct2D(vForward, CurrentVelocity);

		if (VelocityDot > 0.7f)
		{
			//BotJump(pBot);
		}
	}

	// Stop holding crouch if we're a skulk so we can actually climb
	if (IsPlayerSkulk(pBot->pEdict))
	{
		pBot->pEdict->v.button &= ~IN_DUCK;
	}

	float ZDiff = fabs(pEdict->v.origin.z - RequiredClimbHeight);
	Vector AdjustedTargetLocation = EndPoint + (UTIL_GetVectorNormal2D(EndPoint - StartPoint) * 1000.0f);
	Vector DirectAheadView = pBot->CurrentEyePosition + (UTIL_GetVectorNormal2D(AdjustedTargetLocation - pEdict->v.origin) * 10.0f);

	Vector LookLocation = ZERO_VECTOR;

	if (ZDiff < 1.0f)
	{
		LookLocation = DirectAheadView;
	}
	else
	{
		// Don't look up/down quite so much as we reach the desired height so we slow down a bit, reduces the chance of over-shooting and climbing right over a vent
		if (pEdict->v.origin.z > RequiredClimbHeight)
		{
			if (ZDiff > 32.0f)
			{
				LookLocation = DirectAheadView - Vector(0.0f, 0.0f, 100.0f);
			}
			else
			{
				LookLocation = DirectAheadView - Vector(0.0f, 0.0f, 20.0f);
			}
		}
		else
		{
			if (ZDiff > 32.0f)
			{
				LookLocation = DirectAheadView + Vector(0.0f, 0.0f, 100.0f);
			}
			else
			{
				LookLocation = DirectAheadView + Vector(0.0f, 0.0f, 20.0f);
			}
		}
	}

	BotMoveLookAt(pBot, LookLocation);

}

void MoveDirectlyTo(bot_t* pBot, const Vector Destination)
{
	Vector CurrentPos = (pBot->BotNavInfo.IsOnGround) ? pBot->pEdict->v.origin : pBot->CurrentFloorPosition;

	const Vector vForward = UTIL_GetVectorNormal2D(Destination - CurrentPos);
	// Same goes for the right vector, might not be the same as the bot's right
	const Vector vRight = UTIL_GetVectorNormal2D(UTIL_GetCrossProduct(vForward, UP_VECTOR));

	const float PlayerRadius = GetPlayerRadius(pBot->pEdict);

	Vector stTrcLft = CurrentPos - (vRight * PlayerRadius);
	Vector stTrcRt = CurrentPos + (vRight * PlayerRadius);
	Vector endTrcLft = stTrcLft + (vForward * 24.0f);
	Vector endTrcRt = stTrcRt + (vForward * 24.0f);

	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, pBot->BotNavInfo.MoveStyle);

	const bool bumpLeft = !UTIL_PointIsDirectlyReachable(NavProfileIndex, stTrcLft, endTrcLft);
	const bool bumpRight = !UTIL_PointIsDirectlyReachable(NavProfileIndex, stTrcRt, endTrcRt);

	pBot->desiredMovementDir = vForward;

	if (bumpRight && !bumpLeft)
	{
		pBot->desiredMovementDir = pBot->desiredMovementDir - vRight;
	}
	else if (bumpLeft && !bumpRight)
	{
		pBot->desiredMovementDir = pBot->desiredMovementDir + vRight;
	}
	else if (bumpLeft && bumpRight)
	{
		stTrcLft.z = pBot->pEdict->v.origin.z;
		stTrcRt.z = pBot->pEdict->v.origin.z;
		endTrcLft.z = pBot->pEdict->v.origin.z;
		endTrcRt.z = pBot->pEdict->v.origin.z;

		if (!UTIL_QuickTrace(pBot->pEdict, stTrcLft, endTrcLft))
		{
			pBot->desiredMovementDir = pBot->desiredMovementDir + vRight;
		}
		else
		{
			pBot->desiredMovementDir = pBot->desiredMovementDir - vRight;
		}
	}

	float DistFromDestination = vDist2DSq(pBot->pEdict->v.origin, Destination);

	if (CanBotLeap(pBot) && DistFromDestination > sqrf(UTIL_MetresToGoldSrcUnits(10.0f)))
	{
		Vector CurrVelocity = UTIL_GetVectorNormal2D(pBot->pEdict->v.velocity);

		float MoveDot = UTIL_GetDotProduct2D(CurrVelocity, vForward);

		if (MoveDot >= 0.98f)
		{
			BotLeap(pBot, Destination);
		}
	}


	if (!pBot->LookTargetLocation)
	{
		Vector LookTarget = Destination;

		if (DistFromDestination < sqrf(200.0f))
		{
			Vector LookNormal = UTIL_GetVectorNormal2D(LookTarget - pBot->CurrentEyePosition);

			LookTarget = LookTarget + (LookNormal * 1000.0f);
		}

		BotLookAt(pBot, LookTarget);
	}

	HandlePlayerAvoidance(pBot, Destination);
	BotMovementInputs(pBot);
}


bool UTIL_PointIsDirectlyReachable(const bot_t* pBot, const Vector targetPoint)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return false; }

	edict_t* pEdict = pBot->pEdict;

	Vector CurrentPos = (pBot->BotNavInfo.IsOnGround) ? pBot->pEdict->v.origin : pBot->CurrentFloorPosition;

	float pStartPos[3] = { CurrentPos.x, CurrentPos.z, -CurrentPos.y };
	float pEndPos[3] = { targetPoint.x, targetPoint.z, -targetPoint.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3];
	float EndNearest[3];

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;


	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, pReachableExtents, m_navFilter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return false;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, pReachableExtents, m_navFilter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return false;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return true; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_navFilter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	if (hitDist < 1.0f) { return false; }

	if (EndPoly == PolyPath[pathCount - 1]) { return true; }

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);


	return (Height == 0.0f || Height == EndNearest[1]);

}

bool UTIL_PointIsDirectlyReachable(const bot_t* pBot, const Vector start, const Vector target)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return false; }

	if (!start || !target) { return false; }

	float pStartPos[3] = { start.x, start.z, -start.y };
	float pEndPos[3] = { target.x, target.z, -target.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3];
	float EndNearest[3];

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;

	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, pReachableExtents, m_navFilter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return false;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, pReachableExtents, m_navFilter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return false;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return true; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_navFilter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	if (hitDist < 1.0f) { return false; }

	if (EndPoly == PolyPath[pathCount - 1]) { return true; }

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);


	return (Height == 0.0f || Height == EndNearest[1]);

}

const dtNavMesh* UTIL_GetNavMeshForProfile(const int NavProfileIndex)
{
	if (NavProfileIndex < 0 || NavProfileIndex >(MAX_NAV_PROFILES - 1)) { return nullptr; }

	if (NavProfiles[NavProfileIndex].NavMeshIndex > MAX_NAV_MESHES - 1) { return nullptr; }

	return NavMeshes[NavProfiles[NavProfileIndex].NavMeshIndex].navMesh;
}

const dtNavMeshQuery* UTIL_GetNavMeshQueryForProfile(const int NavProfileIndex)
{
	if (NavProfileIndex < 0 || NavProfileIndex >(MAX_NAV_PROFILES - 1)) { return nullptr; }

	if (NavProfiles[NavProfileIndex].NavMeshIndex > MAX_NAV_MESHES - 1) { return nullptr; }

	return NavMeshes[NavProfiles[NavProfileIndex].NavMeshIndex].navQuery;
}

const dtQueryFilter* UTIL_GetNavMeshFilterForProfile(const int NavProfileIndex)
{
	if (NavProfileIndex < 0 || NavProfileIndex >(MAX_NAV_PROFILES - 1)) { return nullptr; }

	if (NavProfiles[NavProfileIndex].NavMeshIndex > MAX_NAV_MESHES - 1) { return nullptr; }

	return &NavProfiles[NavProfileIndex].Filters;
}

const dtTileCache* UTIL_GetTileCacheForProfile(const int NavProfileIndex)
{
	if (NavProfileIndex < 0 || NavProfileIndex >(MAX_NAV_PROFILES - 1)) { return nullptr; }

	if (NavProfiles[NavProfileIndex].NavMeshIndex > MAX_NAV_MESHES - 1) { return nullptr; }

	return NavMeshes[NavProfiles[NavProfileIndex].NavMeshIndex].tileCache;
}

bool UTIL_PointIsDirectlyReachable(const int NavProfileIndex, const Vector start, const Vector target)
{
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navMesh) { return false; }

	float pStartPos[3] = { start.x, start.z, -start.y };
	float pEndPos[3] = { target.x, target.z, -target.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3] = { 0.0f, 0.0f, 0.0f };
	float EndNearest[3] = { 0.0f, 0.0f, 0.0f };

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;


	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, pReachableExtents, m_navFilter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return false;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, pReachableExtents, m_navFilter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return false;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return true; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_navFilter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	if (hitDist < 1.0f)
	{
		if (pathCount == 0) { return false; }

		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[pathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(max_player_use_reach))
		{
			return false;
		}
		else
		{
			return true;
		}
	}

	if (EndPoly == PolyPath[pathCount - 1]) { return true; }

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);


	return (Height == 0.0f || Height == EndNearest[1]);
}

bool UTIL_TraceNav(const int NavProfileIndex, const Vector start, const Vector target, const float MaxAcceptableDistance)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_Filter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return false; }

	float pStartPos[3] = { start.x, start.z, -start.y };
	float pEndPos[3] = { target.x, target.z, -target.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3] = { 0.0f, 0.0f, 0.0f };
	float EndNearest[3] = { 0.0f, 0.0f, 0.0f };

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;

	float MaxReachableExtents[3] = { MaxAcceptableDistance, 50.0f, MaxAcceptableDistance };

	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, MaxReachableExtents, m_Filter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return false;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, MaxReachableExtents, m_Filter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return false;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return true; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_Filter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	if (hitDist < 1.0f)
	{
		if (pathCount == 0) { return false; }

		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[pathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(MaxAcceptableDistance))
		{
			return false;
		}
		else
		{
			return true;
		}
	}

	if (EndPoly == PolyPath[pathCount - 1]) { return true; }

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);


	return (Height == 0.0f || Height == EndNearest[1]);
}

void UTIL_TraceNavLine(const int NavProfileIndex, const Vector Start, const Vector End, nav_hitresult* HitResult)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_Filter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery)
	{
		HitResult->flFraction = 0.0f;
		HitResult->bStartOffMesh = true;
		HitResult->TraceEndPoint = Start;
		return;
	}

	float pStartPos[3] = { Start.x, Start.z, -Start.y };
	float pEndPos[3] = { End.x, End.z, -End.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3] = { 0.0f, 0.0f, 0.0f };
	float EndNearest[3] = { 0.0f, 0.0f, 0.0f };

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;

	float MaxReachableExtents[3] = { 18.0f, 32.0f, 18.0f };

	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, MaxReachableExtents, m_Filter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		HitResult->flFraction = 0.0f;
		HitResult->bStartOffMesh = true;
		HitResult->TraceEndPoint = Start;
		return;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, MaxReachableExtents, m_Filter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		HitResult->flFraction = 0.0f;
		HitResult->bStartOffMesh = true;
		HitResult->TraceEndPoint = Start;
		return;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly)
	{

		HitResult->flFraction = 1.0f;
		HitResult->bStartOffMesh = false;
		HitResult->TraceEndPoint = Vector(EndNearest[0], -EndNearest[2], EndNearest[1]);
		return;
	}

	

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_Filter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	HitResult->flFraction = hitDist;
	HitResult->bStartOffMesh = false;

	Vector HitLocation = ZERO_VECTOR;

	if (hitDist >= 1.0f)
	{
		HitLocation = Vector(EndNearest[0], -EndNearest[2], EndNearest[1]);
	}
	else
	{
		Vector Dir = UTIL_GetVectorNormal(End - Start);
		Vector Point = Start + (Dir * HitResult->flFraction);

		HitLocation = UTIL_ProjectPointToNavmesh(Point, Vector(100.0f, 100.0f, 100.0f), NavProfileIndex);
	}

	HitResult->TraceEndPoint = HitLocation;
}

bool UTIL_PointIsDirectlyReachable(const Vector start, const Vector target)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_Filter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return false; }

	float pStartPos[3] = { start.x, start.z, -start.y };
	float pEndPos[3] = { target.x, target.z, -target.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3] = { 0.0f, 0.0f, 0.0f };
	float EndNearest[3] = { 0.0f, 0.0f, 0.0f };

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;


	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, pReachableExtents, m_Filter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return false;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, pReachableExtents, m_Filter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return false;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return true; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_Filter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	if (hitDist < 1.0f)
	{
		if (pathCount == 0) { return false; }

		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[pathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(max_player_use_reach))
		{
			return false;
		}
		else
		{
			return true;
		}
	}

	if (EndPoly == PolyPath[pathCount - 1]) { return true; }

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);


	return (Height == 0.0f || Height == EndNearest[1]);
}

float UTIL_PointIsDirectlyReachable_DEBUG(const Vector start, const Vector target)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_Filter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return 0.0f; }

	float pStartPos[3] = { start.x, start.z, -start.y };
	float pEndPos[3] = { target.x, target.z, -target.y };

	dtPolyRef StartPoly;
	dtPolyRef EndPoly;
	float StartNearest[3];
	float EndNearest[3];

	float hitDist;
	float HitNormal[3];

	dtPolyRef PolyPath[MAX_PATH_POLY];
	int pathCount = 0;


	dtStatus FoundStartPoly = m_navQuery->findNearestPoly(pStartPos, pReachableExtents, m_Filter, &StartPoly, StartNearest);

	if (!dtStatusSucceed(FoundStartPoly))
	{
		return 1.1f;
	}

	dtStatus FoundEndPoly = m_navQuery->findNearestPoly(pEndPos, pReachableExtents, m_Filter, &EndPoly, EndNearest);

	if (!dtStatusSucceed(FoundEndPoly))
	{
		return 1.2f;
	}

	// All polys are convex, therefore definitely reachable if start and end points are within the same poly
	if (StartPoly == EndPoly) { return 2.1f; }

	m_navQuery->raycast(StartPoly, StartNearest, EndNearest, m_Filter, &hitDist, HitNormal, PolyPath, &pathCount, MAX_PATH_SIZE);

	float ClosestPoint[3] = { 0.0f, 0.0f, 0.0f };
	float Height = 0.0f;
	m_navQuery->closestPointOnPolyBoundary(PolyPath[pathCount - 1], EndNearest, ClosestPoint);
	m_navQuery->getPolyHeight(PolyPath[pathCount - 1], ClosestPoint, &Height);

	float Dist = dtVdistSqr(EndNearest, ClosestPoint);

	if (hitDist < 1.0f)
	{
		if (pathCount == 0) { return 1.3f; }

		float epos[3];
		dtVcopy(epos, EndNearest);

		m_navQuery->closestPointOnPoly(PolyPath[pathCount - 1], EndNearest, epos, 0);

		if (dtVdistSqr(EndNearest, epos) > sqrf(max_player_use_reach))
		{
			return 1.5f;
		}
		else
		{
			return 2.4f;
		}


		return 1.3f;
	}

	if (EndPoly != PolyPath[pathCount - 1])
	{
		if (Height == 0.0f || Height == EndNearest[1])
		{
			return 2.3f;
		}
		return 1.4f;
	}

	return 2.2f;
}

dtPolyRef UTIL_GetNearestPolyRefForLocation(const int NavProfileIndex, const Vector Location)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return 0; }

	float ConvertedFloorCoords[3] = { Location.x, Location.z, -Location.y };

	float pPolySearchExtents[3] = { 50.0f, 50.0f, 50.0f };

	dtPolyRef result;
	float nearestPoint[3] = { 0.0f, 0.0f, 0.0f };

	m_navQuery->findNearestPoly(ConvertedFloorCoords, pPolySearchExtents, m_navFilter, &result, nearestPoint);

	return result;
}

dtPolyRef UTIL_GetNearestPolyRefForLocation(const Vector Location)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return 0; }

	float ConvertedFloorCoords[3] = { Location.x, Location.z, -Location.y };

	float pPolySearchExtents[3] = { 50.0f, 50.0f, 50.0f };

	dtPolyRef result;
	float nearestPoint[3] = { 0.0f, 0.0f, 0.0f };

	m_navQuery->findNearestPoly(ConvertedFloorCoords, pPolySearchExtents, m_navFilter, &result, nearestPoint);

	return result;
}

dtPolyRef UTIL_GetNearestPolyRefForEntity(const edict_t* Edict)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return 0; }

	Vector Floor = UTIL_GetFloorUnderEntity(Edict);

	float ConvertedFloorCoords[3] = { Floor.x, Floor.z, -Floor.y };

	float pPolySearchExtents[3] = { 50.0f, 50.0f, 50.0f };

	dtPolyRef result;
	float nearestPoint[3] = { 0.0f, 0.0f, 0.0f };

	m_navQuery->findNearestPoly(ConvertedFloorCoords, pPolySearchExtents, m_navFilter, &result, nearestPoint);

	return result;
}

unsigned char UTIL_GetNavAreaAtLocation(const int NavProfile, const Vector Location)
{
	if (NavProfile < 0 || NavProfile > MAX_NAV_PROFILES - 1) { return SAMPLE_POLYAREA_BLOCKED; }

	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfile);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfile);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfile);

	if (!m_navQuery) { return (unsigned char)SAMPLE_POLYAREA_BLOCKED; }

	Vector TraceHit = UTIL_GetTraceHitLocation(Location + Vector(0.0f, 0.0f, 10.0f), Location - Vector(0.0f, 0.0f, 500.0f));

	Vector PointToProject = (TraceHit != ZERO_VECTOR) ? TraceHit : Location;

	float pCheckLoc[3] = { PointToProject.x, PointToProject.z, -PointToProject.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, pReachableExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		unsigned char area = 0;
		m_navMesh->getPolyArea(FoundPoly, &area);
		return area;
	}
	else
	{
		return (unsigned char)SAMPLE_POLYAREA_BLOCKED;
	}
}

unsigned char UTIL_GetNavAreaAtLocation(const Vector Location)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return 0; }

	Vector TraceHit = UTIL_GetTraceHitLocation(Location + Vector(0.0f, 0.0f, 10.0f), Location - Vector(0.0f, 0.0f, 500.0f));

	Vector PointToProject = (TraceHit != ZERO_VECTOR) ? TraceHit : Location;

	float pCheckLoc[3] = { PointToProject.x, PointToProject.z, -PointToProject.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, pReachableExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		unsigned char area = 0;
		m_navMesh->getPolyArea(FoundPoly, &area);
		return area;
	}
	else
	{
		return 0;
	}
}

const char* UTIL_NavmeshAreaToChar(const unsigned char Area)
{
	switch (Area)
	{
	case SAMPLE_POLYAREA_BLOCKED:
		return "Blocked";
	case SAMPLE_POLYAREA_CROUCH:
		return "Crouch";
	case SAMPLE_POLYAREA_DOOR:
		return "Door";
	case SAMPLE_POLYAREA_FALL:
		return "Fall";
	case SAMPLE_POLYAREA_GROUND:
		return "Ground";
	case SAMPLE_POLYAREA_HIGHFALL:
		return "High Fall";
	case SAMPLE_POLYAREA_HIGHJUMP:
		return "High Jump";
	case SAMPLE_POLYAREA_JUMP:
		return "Jump";
	case SAMPLE_POLYAREA_LADDER:
		return "Ladder";
	case SAMPLE_POLYAREA_WALLCLIMB:
		return "Wall Climb";
	case SAMPLE_POLYAREA_WATER:
		return "Water";
	default:
		return "INVALID";

	}

	return "INVALID";
}

int UTIL_GetMoveProfileForBot(const bot_t* pBot, BotMoveStyle MoveStyle)
{
	switch (pBot->bot_ns_class)
	{
	case CLASS_MARINE:
		return UTIL_GetMoveProfileForMarine(MoveStyle);
	case CLASS_SKULK:
		return UTIL_GetMoveProfileForSkulk(MoveStyle);
	case CLASS_GORGE:
		return UTIL_GetMoveProfileForGorge(MoveStyle);
	case CLASS_LERK:
		return UTIL_GetMoveProfileForLerk(MoveStyle);
	case CLASS_FADE:
		return UTIL_GetMoveProfileForFade(MoveStyle);
	case CLASS_ONOS:
		return UTIL_GetMoveProfileForOnos(MoveStyle);
	default:
		return MARINE_REGULAR_NAV_PROFILE;
	}
}

int UTIL_GetMoveProfileForMarine(const BotMoveStyle MoveStyle)
{
	return MARINE_REGULAR_NAV_PROFILE;
}

int UTIL_GetMoveProfileForSkulk(const BotMoveStyle MoveStyle)
{
	switch (MoveStyle)
	{
	case MOVESTYLE_AMBUSH:
	case MOVESTYLE_HIDE:
		return SKULK_AMBUSH_NAV_PROFILE;
	default:
		return SKULK_REGULAR_NAV_PROFILE;
	}
}

int UTIL_GetMoveProfileForGorge(const BotMoveStyle MoveStyle)
{
	switch (MoveStyle)
	{
	case MOVESTYLE_HIDE:
		return GORGE_HIDE_NAV_PROFILE;
	default:
		return GORGE_REGULAR_NAV_PROFILE;
	}
}

int UTIL_GetMoveProfileForLerk(const BotMoveStyle MoveStyle)
{
	switch (MoveStyle)
	{
	case MOVESTYLE_NORMAL:
		return LERK_FLYING_NAV_PROFILE;
	default:
		return GORGE_REGULAR_NAV_PROFILE;
	}

	return GORGE_REGULAR_NAV_PROFILE;
}

int UTIL_GetMoveProfileForFade(const BotMoveStyle MoveStyle)
{
	return FADE_REGULAR_NAV_PROFILE;
}

int UTIL_GetMoveProfileForOnos(const BotMoveStyle MoveStyle)
{
	return ONOS_REGULAR_NAV_PROFILE;
}

void UTIL_UpdateBotMovementStatus(bot_t* pBot)
{
	if (pBot->pEdict->v.movetype != pBot->BotNavInfo.CurrentMoveType)
	{
		if (pBot->BotNavInfo.CurrentMoveType == MOVETYPE_FLY)
		{
			OnBotEndLadder(pBot);
		}


		if (pBot->pEdict->v.movetype == MOVETYPE_FLY)
		{
			OnBotStartLadder(pBot);
		}

		pBot->BotNavInfo.CurrentMoveType = pBot->pEdict->v.movetype;
	}

	pBot->BotNavInfo.CurrentPoly = UTIL_GetNearestPolyRefForEntity(pBot->pEdict);

	pBot->CollisionHullBottomLocation = GetPlayerBottomOfCollisionHull(pBot->pEdict);
	pBot->CollisionHullTopLocation = GetPlayerTopOfCollisionHull(pBot->pEdict);
}


bool AbortCurrentMove(bot_t* pBot, const Vector NewDestination)
{
	if (pBot->BotNavInfo.PathSize == 0 || pBot->BotNavInfo.CurrentPathPoint == 0 || pBot->BotNavInfo.CurrentPathPoint == pBot->BotNavInfo.PathSize - 1) { return true; }

	if (IsBotPermaStuck(pBot))
	{
		BotSuicide(pBot);
		return false;
	}

	Vector MoveFrom = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location;
	Vector MoveTo = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;
	unsigned char area = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;

	Vector ClosestPointOnLine = vClosestPointOnLine2D(MoveFrom, MoveTo, pBot->pEdict->v.origin);

	bool bAtOrPastMovement = (vEquals2D(ClosestPointOnLine, MoveFrom, 1.0f) || vEquals2D(ClosestPointOnLine, MoveTo, 1.0f));

	if ((pBot->pEdict->v.flags & FL_ONGROUND) && bAtOrPastMovement)
	{
		return true;
	}

	Vector DestinationPointOnLine = vClosestPointOnLine(MoveFrom, MoveTo, NewDestination);

	bool bReverseCourse = (vDist3DSq(DestinationPointOnLine, MoveFrom) < vDist3DSq(DestinationPointOnLine, MoveTo));

	if (area == SAMPLE_POLYAREA_GROUND || area == SAMPLE_POLYAREA_CROUCH)
	{
		if (UTIL_PointIsDirectlyReachable(pBot->pEdict->v.origin, MoveFrom) || UTIL_PointIsDirectlyReachable(pBot->pEdict->v.origin, MoveTo))
		{
			return true;
		}

		if (bReverseCourse)
		{
			GroundMove(pBot, MoveTo, MoveFrom);
		}
		else
		{
			GroundMove(pBot, MoveFrom, MoveTo);
		}
	}

	if (area == SAMPLE_POLYAREA_WALLCLIMB)
	{
		if (bReverseCourse)
		{
			FallMove(pBot, MoveTo, MoveFrom);
		}
		else
		{
			WallClimbMove(pBot, MoveFrom, MoveTo, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ);
		}
	}

	if (area == SAMPLE_POLYAREA_LADDER)
	{
		if (bReverseCourse)
		{
			LadderMove(pBot, MoveTo, MoveFrom, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ, (unsigned char)SAMPLE_POLYAREA_CROUCH);

			// We're going DOWN the ladder
			if (MoveTo.z > MoveFrom.z)
			{
				if (pBot->pEdict->v.origin.z - MoveFrom.z < 150.0f)
				{
					BotJump(pBot);
				}
			}
		}
		else
		{
			LadderMove(pBot, MoveFrom, MoveTo, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].requiredZ, (unsigned char)SAMPLE_POLYAREA_CROUCH);

			// We're going DOWN the ladder
			if (MoveFrom.z > MoveTo.z)
			{
				if (pBot->pEdict->v.origin.z - MoveTo.z < 150.0f)
				{
					BotJump(pBot);
				}
			}
		}
	}

	if (area == SAMPLE_POLYAREA_PHASEGATE)
	{
		return true;
	}

	if (area == SAMPLE_POLYAREA_JUMP || area == SAMPLE_POLYAREA_HIGHJUMP || area == SAMPLE_POLYAREA_BLOCKED)
	{
		if (bReverseCourse)
		{
			JumpMove(pBot, MoveTo, MoveFrom);
		}
		else
		{
			JumpMove(pBot, MoveFrom, MoveTo);
		}
	}

	if (area == SAMPLE_POLYAREA_FALL || area == SAMPLE_POLYAREA_HIGHFALL)
	{
		FallMove(pBot, MoveFrom, MoveTo);
	}

	BotMovementInputs(pBot);

	return false;
}

bool IsBotPermaStuck(bot_t* pBot)
{
	if (CONFIG_GetMaxStuckTime() <= 0.1f) { return false; }

	if (pBot->LastPosition == ZERO_VECTOR || vDist3DSq(pBot->pEdict->v.origin, pBot->LastPosition) > sqrf(32.0f))
	{
		pBot->TimeSinceLastMovement = 0.0f;
		pBot->LastPosition = pBot->pEdict->v.origin;
		return false;
	}

	pBot->TimeSinceLastMovement += GAME_GetBotDeltaTime();

	return (pBot->TimeSinceLastMovement >= 30.0f);
}

bool MoveTo(bot_t* pBot, const Vector Destination, const BotMoveStyle MoveStyle, const float MaxAcceptableDist)
{
	// Invalid destination, or we're already there
	if (!Destination || BotIsAtLocation(pBot, Destination))
	{
		ClearBotMovement(pBot);
		
		return true;
	}

	UTIL_UpdateBotMovementStatus(pBot);

	nav_status* BotNavInfo = &pBot->BotNavInfo;

	// If we are currently in the process of getting back on the navmesh, don't interrupt
	if (BotNavInfo->UnstuckMoveLocation != ZERO_VECTOR)
	{
		if (IsBotPermaStuck(pBot))
		{
			BotSuicide(pBot);
			return false;
		}

		Vector MoveTarget = BotNavInfo->UnstuckMoveStartLocation + (UTIL_GetVectorNormal2D(BotNavInfo->UnstuckMoveLocation - BotNavInfo->UnstuckMoveStartLocation) * 100.0f);

		MoveDirectlyTo(pBot, MoveTarget);

		Vector ClosestPoint = vClosestPointOnLine2D(BotNavInfo->UnstuckMoveStartLocation, BotNavInfo->UnstuckMoveLocation, pBot->pEdict->v.origin);

		bool bAtOrPastMoveLocation = vEquals2D(ClosestPoint, BotNavInfo->UnstuckMoveLocation, 0.1f);

		if (bAtOrPastMoveLocation)
		{
			ClearBotStuckMovement(pBot);
			return true;
		}
		else
		{
			// This should only be a short movement, if we don't get there in a few seconds then give up
			if ((gpGlobals->time - BotNavInfo->UnstuckMoveLocationStartTime) > 5.0f)
			{
				ClearBotStuckMovement(pBot);
				return true;
			}

			BotJump(pBot);

			if (IsPlayerSkulk(pBot->pEdict))
			{
				pBot->pEdict->v.button &= ~IN_DUCK;
			}
			else
			{
				pBot->pEdict->v.button |= IN_DUCK;
			}

			

			return true;
		}
	}

	int MoveProfile = UTIL_GetMoveProfileForBot(pBot, pBot->BotNavInfo.MoveStyle);
	bool bIsFlyingProfile = NavProfiles[MoveProfile].bFlyingProfile;

	bool bMoveStyleChanged = (MoveStyle != pBot->BotNavInfo.MoveStyle);
	bool bNavProfileChanged = (MoveProfile != pBot->BotNavInfo.LastMoveProfile);
	bool bCanRecalculatePath = (gpGlobals->time - pBot->BotNavInfo.LastPathCalcTime > MIN_PATH_RECALC_TIME);
	bool bDestinationChanged = (!vEquals(Destination, BotNavInfo->TargetDestination, GetPlayerRadius(pBot->pEdict)));

	// Only recalculate the path if there isn't a path, or something has changed and enough time has elapsed since the last path calculation
	bool bShouldCalculatePath = bCanRecalculatePath && (BotNavInfo->PathSize == 0 || (bMoveStyleChanged || bNavProfileChanged || bDestinationChanged || BotNavInfo->bPendingRecalculation));

	if (bShouldCalculatePath)
	{
		// First abort our current move so we don't try to recalculate half-way up a wall or ladder
		if (!bIsFlyingProfile && !AbortCurrentMove(pBot, Destination))
		{
			return true;
		}

		if (IsPlayerOnLadder(pBot->pEdict))
		{
			BotJump(pBot);
			return true;
		}

		pBot->BotNavInfo.LastPathCalcTime = gpGlobals->time;
		BotNavInfo->bPendingRecalculation = false;

		pBot->BotNavInfo.MoveStyle = MoveStyle;
		pBot->BotNavInfo.LastMoveProfile = MoveProfile;

		

		BotNavInfo->TargetDestination = Destination;

		Vector ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(Destination, Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach), MoveProfile);

		// Destination is not on the nav mesh, so we can't get close enough
		if (!ValidNavmeshPoint)
		{
			sprintf(pBot->PathStatus, "Could not project destination to navmesh");
			return false;
		}

		dtStatus PathFindingStatus = DT_FAILURE;
		
		if (bIsFlyingProfile)
		{
			PathFindingStatus = FindFlightPathToPoint(MoveProfile, pBot->CurrentFloorPosition, ValidNavmeshPoint, BotNavInfo->CurrentPath, &BotNavInfo->PathSize, MaxAcceptableDist);
		}
		else
		{
			PathFindingStatus = FindPathClosestToPoint(pBot, pBot->BotNavInfo.MoveStyle, pBot->CurrentFloorPosition, ValidNavmeshPoint, BotNavInfo->CurrentPath, &BotNavInfo->PathSize, MaxAcceptableDist);
		}
		
		

		if (dtStatusSucceed(PathFindingStatus))
		{
			BotNavInfo->ActualMoveDestination = BotNavInfo->CurrentPath[BotNavInfo->PathSize - 1].Location;
			ClearBotStuckMovement(pBot);
			pBot->BotNavInfo.TotalStuckTime = 0.0f;

			BotNavInfo->CurrentPathPoint = 0;
			sprintf(pBot->PathStatus, "Path finding successful");

		}
		else
		{
			Vector PointBackOnPath = FindClosestPointBackOnPath(pBot);

			if (PointBackOnPath != ZERO_VECTOR)
			{
				ClearBotStuckMovement(pBot);
				ClearBotPath(pBot);

				BotNavInfo->UnstuckMoveLocation = PointBackOnPath;
				BotNavInfo->UnstuckMoveStartLocation = pBot->pEdict->v.origin;
				BotNavInfo->UnstuckMoveLocationStartTime = gpGlobals->time;

				sprintf(pBot->PathStatus, "Backwards Path Find Successful");
			}
			else
			{
				if (BotNavInfo->LastNavMeshPosition != ZERO_VECTOR && vDist2DSq(BotNavInfo->LastNavMeshPosition, pBot->pEdict->v.origin) > GetPlayerRadius(pBot->pEdict))
				{
					ClearBotStuckMovement(pBot);
					ClearBotPath(pBot);

					BotNavInfo->UnstuckMoveLocation = BotNavInfo->LastNavMeshPosition;
					BotNavInfo->UnstuckMoveStartLocation = pBot->pEdict->v.origin;
					BotNavInfo->UnstuckMoveLocationStartTime = gpGlobals->time;
				}
				else
				{
					BotSuicide(pBot);
				}

				return false;
			}
		}
	}

	if (BotNavInfo->PathSize > 0)
	{
		if (IsBotPermaStuck(pBot))
		{
			BotSuicide(pBot);
			return false;
		}

		if (bIsFlyingProfile)
		{
			BotFollowFlightPath(pBot);
		}
		else
		{
			BotFollowPath(pBot);
		}		

		HandlePlayerAvoidance(pBot, BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].Location);
		BotMovementInputs(pBot);

		return true;
	}

	return false;
}

bool AreKeyPointsReachableForBot(bot_t* pBot)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	Vector CommChairLocation = UTIL_GetCommChairLocation();

	if (CommChairLocation != ZERO_VECTOR)
	{
		if (UTIL_PointIsReachable(NavProfileIndex, pBot->CurrentFloorPosition, CommChairLocation, max_player_use_reach))
		{
			return true;
		}
	}

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (UTIL_PointIsReachable(NavProfileIndex, pBot->CurrentFloorPosition, Hives[i].FloorLocation, max_player_use_reach))
		{
			return true;
		}
	}

	return false;

}

void DEBUG_DrawPath(const bot_path_node* Path, const int PathSize, const float DrawTimeInSeconds)
{
	float DrawTime = fmaxf(DrawTimeInSeconds, 0.1f);

	if (PathSize <= 0) { return; }


	int numPoints = PathSize;

	Vector FromDraw = Path[0].Location;

	UTIL_DrawLine(GAME_GetListenServerEdict(), Path[0].Location, Path[0].Location + Vector(0.0f, 0.0f, 50.0f), DrawTime, 255, 0, 0);

	unsigned char area = Path[0].area;
	for (int i = 1; i < PathSize; i++)
	{
		area = Path[i].area;
		Vector ToDraw = Vector(Path[i].Location.x, Path[i].Location.y, Path[i].requiredZ);

		if (area == SAMPLE_POLYAREA_GROUND)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime);
		}
		else if (area == SAMPLE_POLYAREA_CROUCH)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 255, 0, 0);
		}
		else if (area == SAMPLE_POLYAREA_JUMP || area == SAMPLE_POLYAREA_HIGHJUMP || area == SAMPLE_POLYAREA_BLOCKED)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 255, 255, 0);
		}
		else if (area == SAMPLE_POLYAREA_WALLCLIMB)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 0, 128, 0);
		}
		else if (area == SAMPLE_POLYAREA_LADDER)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 0, 0, 255);
		}
		else if (area == SAMPLE_POLYAREA_FALL || area == SAMPLE_POLYAREA_HIGHFALL)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 255, 0, 255);
		}
		else if (area == SAMPLE_POLYAREA_PHASEGATE)
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime, 64, 0, 0);
		}
		else
		{
			UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, ToDraw, DrawTime);
		}

		FromDraw = ToDraw;

	}

	UTIL_DrawLine(GAME_GetListenServerEdict(), Path[PathSize - 1].Location, Path[PathSize - 1].Location + Vector(0.0f, 0.0f, 50.0f), DrawTime, 0, 0, 255);
}

Vector DEBUG_FindClosestPointBackOnPath(edict_t* Player)
{

	Vector ValidNavmeshPoint = UTIL_GetNearestPointOfInterestToLocation(Player->v.origin, false);


	ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(ValidNavmeshPoint, MARINE_REGULAR_NAV_PROFILE);

	if (!ValidNavmeshPoint)
	{
		return ZERO_VECTOR;
	}

	bot_path_node BackwardsPath[MAX_PATH_SIZE];
	memset(BackwardsPath, 0, sizeof(BackwardsPath));
	int BackwardsPathSize = 0;

	// Now we find a path backwards from the valid nav mesh point to our location, trying to get as close as we can to it

	dtStatus BackwardFindingStatus = FindPathClosestToPoint(MARINE_REGULAR_NAV_PROFILE, ValidNavmeshPoint, UTIL_GetEntityGroundLocation(Player), BackwardsPath, &BackwardsPathSize, 500.0f);

	if (dtStatusSucceed(BackwardFindingStatus))
	{
		int PathIndex = BackwardsPathSize - 1;

		Vector NewMoveLocation = BackwardsPath[PathIndex].Location;

		// The end point of this backwards path might not actually be reachable (e.g. behind a wall). Try and find any point along this path we can see ourselves
		while (!UTIL_QuickTrace(Player, Player->v.origin, NewMoveLocation + Vector(0.0f, 0.0f, 5.0f)) && PathIndex > 0)
		{
			PathIndex--;
			NewMoveLocation = BackwardsPath[PathIndex].Location;
		}

		return NewMoveLocation;
	}

	return ZERO_VECTOR;
}

Vector FindClosestPointBackOnPath(bot_t* pBot)
{
	int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	Vector ValidNavmeshPoint = UTIL_GetNearestPointOfInterestToLocation(pBot->pEdict->v.origin, false);


	ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(ValidNavmeshPoint, NavProfileIndex);

	if (!ValidNavmeshPoint)
	{
		return ZERO_VECTOR;
	}

	bot_path_node BackwardsPath[MAX_PATH_SIZE];
	memset(BackwardsPath, 0, sizeof(BackwardsPath));
	int BackwardsPathSize = 0;

	// Now we find a path backwards from the valid nav mesh point to our location, trying to get as close as we can to it

	dtStatus BackwardFindingStatus = FindPathClosestToPoint(NavProfileIndex, ValidNavmeshPoint, pBot->CurrentFloorPosition, BackwardsPath, &BackwardsPathSize, 500.0f);

	if (dtStatusSucceed(BackwardFindingStatus))
	{
		int PathIndex = BackwardsPathSize - 1;

		Vector NewMoveLocation = BackwardsPath[PathIndex].Location;

		// The end point of this backwards path might not actually be reachable (e.g. behind a wall). Try and find any point along this path we can see ourselves
		while (!UTIL_QuickTrace(pBot->pEdict, pBot->pEdict->v.origin, NewMoveLocation) && PathIndex > 0)
		{
			PathIndex--;
			NewMoveLocation = BackwardsPath[PathIndex].Location;
		}

		return NewMoveLocation;
	}

	return ZERO_VECTOR;
}

Vector FindClosestNavigablePointToDestination(const int NavProfileIndex, const Vector FromLocation, const Vector ToLocation, float MaxAcceptableDistance)
{
	bot_path_node Path[MAX_PATH_SIZE];
	memset(Path, 0, sizeof(Path));
	int PathSize = 0;

	// Now we find a path backwards from the valid nav mesh point to our location, trying to get as close as we can to it

	dtStatus PathFindingResult = FindPathClosestToPoint(NavProfileIndex, FromLocation, ToLocation, Path, &PathSize, MaxAcceptableDistance);

	if (dtStatusSucceed(PathFindingResult))
	{
		return Path[PathSize - 1].Location;
	}

	return ZERO_VECTOR;
}

void DEBUG_TestFlightPathFind(edict_t* pEdict, const Vector Destination)
{
	if (!NavmeshLoaded())
	{
		if (!loadNavigationData(STRING(gpGlobals->mapname)))
		{
			return;
		}
	}

	Vector ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(Destination, Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach), SKULK_REGULAR_NAV_PROFILE);

	// We can't actually get close enough to this point to consider it "reachable"
	if (!ValidNavmeshPoint)
	{
		UTIL_SayText("Couldn't project point to mesh!\n", pEdict);
		return;
	}

	bot_path_node FlightPath[MAX_PATH_SIZE];
	memset(FlightPath, 0, sizeof(FlightPath));
	int FlightPathSize = 0;

	dtStatus FlightFindingStatus = FindFlightPathToPoint(SKULK_REGULAR_NAV_PROFILE, UTIL_GetEntityGroundLocation(pEdict), ValidNavmeshPoint, FlightPath, &FlightPathSize, 500.0f);

	if (dtStatusSucceed(FlightFindingStatus))
	{
		DEBUG_DrawPath(FlightPath, FlightPathSize, 10.0f);
	}
}

void DEBUG_TestBackwardsPathFind(edict_t* pEdict, const Vector Destination)
{
	if (!NavmeshLoaded())
	{
		if (!loadNavigationData(STRING(gpGlobals->mapname)))
		{
			return;
		}
	}

	Vector ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(Destination, Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach), MARINE_REGULAR_NAV_PROFILE);

	// We can't actually get close enough to this point to consider it "reachable"
	if (!ValidNavmeshPoint)
	{
		UTIL_SayText("Couldn't project point to mesh!\n", pEdict);
		return;
	}

	bot_path_node BackwardsPath[MAX_PATH_SIZE];
	memset(BackwardsPath, 0, sizeof(BackwardsPath));
	int BackwardsPathSize = 0;

	dtStatus BackwardFindingStatus = FindPathClosestToPoint(MARINE_REGULAR_NAV_PROFILE, ValidNavmeshPoint, UTIL_GetEntityGroundLocation(pEdict), BackwardsPath, &BackwardsPathSize, 500.0f);

	if (dtStatusSucceed(BackwardFindingStatus))
	{
		int PathIndex = BackwardsPathSize - 1;

		Vector NewMoveLocation = BackwardsPath[PathIndex].Location;

		while (!UTIL_QuickTrace(pEdict, pEdict->v.origin, NewMoveLocation) && PathIndex > 0)
		{
			PathIndex--;
			NewMoveLocation = BackwardsPath[PathIndex].Location;
		}

		Vector MoveDir = UTIL_GetVectorNormal2D(pEdict->v.origin - NewMoveLocation);

		if (isnan(MoveDir.x) || MoveDir == ZERO_VECTOR)
		{
			MoveDir = UTIL_GetVectorNormal2D(BackwardsPath[BackwardsPathSize - 1].Location - BackwardsPath[PathIndex].Location);
		}

		NewMoveLocation = (pEdict->v.origin - (MoveDir * 100.0f));

		DEBUG_DrawPath(BackwardsPath, BackwardsPathSize, 10.0f);
		UTIL_DrawLine(pEdict, pEdict->v.origin, NewMoveLocation, 10.0f, 255, 0, 255);

		char buf[64];
		sprintf(buf, "Found backwards path (%f, %f, %f)!\n", NewMoveLocation.x, NewMoveLocation.y, NewMoveLocation.z);
		UTIL_SayText(buf, pEdict);
	}
	else
	{
		UTIL_SayText("Couldn't find backwards path!\n", pEdict);
	}
}

int GetNextDirectFlightPath(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0) { return -1; }

	nav_status* BotNavInfo = &pBot->BotNavInfo;

	for (int i = BotNavInfo->PathSize - 2; i > pBot->BotNavInfo.CurrentPathPoint; i--)
	{
		Vector ThisPoint = BotNavInfo->CurrentPath[i].Location;

		

		if (UTIL_QuickTrace(pBot->pEdict, pBot->pEdict->v.origin, ThisPoint))
		{
			if (BotNavInfo->CurrentPath[i].area != SAMPLE_POLYAREA_CROUCH && BotNavInfo->CurrentPath[i + 1].area != SAMPLE_POLYAREA_CROUCH)
			{

				Vector NewPoint = UTIL_FindHighestSuccessfulTracePoint(pBot->pEdict->v.origin, ThisPoint, BotNavInfo->CurrentPath[i + 1].Location, 10.0f, 50.0f, 200.0f);

				if (NewPoint != ZERO_VECTOR)
				{
					BotNavInfo->CurrentPath[i].Location = NewPoint;
					return i;
				}
			}
			else
			{
				if (UTIL_QuickHullTrace(pBot->pEdict, pBot->pEdict->v.origin, ThisPoint))
				{
					return i;
				}
			}
		}
	}

	return pBot->BotNavInfo.CurrentPathPoint;
}

void BotFollowFlightPath(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0)
	{
		return;
	}

	nav_status* BotNavInfo = &pBot->BotNavInfo;
	edict_t* pEdict = pBot->pEdict;

	Vector CurrentMoveDest = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;
	Vector NextPoint = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].Location;
	unsigned char CurrentMoveArea = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;


	//if ((CurrentMoveArea == SAMPLE_POLYAREA_GROUND || CurrentMoveArea == SAMPLE_POLYAREA_CROUCH) && !UTIL_QuickTrace(pBot->pEdict, pBot->pEdict->v.origin, CurrentMoveDest))
	//{
	//	BotRecalcPath(pBot, BotNavInfo->ActualMoveDestination);
	//	return;
	//}

	Vector ClosestPointToPath = vClosestPointOnLine(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location, CurrentMoveDest, pEdict->v.origin);

	bool bAtOrPastDestination = vEquals(ClosestPointToPath, CurrentMoveDest, 32.0f);

	Vector MoveFrom = ZERO_VECTOR;

	if (BotNavInfo->CurrentPathPoint > 0)
	{
		MoveFrom = BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint - 1].Location;
	}
	else
	{
		MoveFrom = pBot->pEdict->v.origin;
	}

	// If we've reached our current path point
	if (bAtOrPastDestination || UTIL_QuickHullTrace(pEdict, pEdict->v.origin, NextPoint, head_hull))
	{
		// End of the whole path, stop all movement
		if (BotNavInfo->CurrentPathPoint == (pBot->BotNavInfo.PathSize - 1))
		{
			ClearBotPath(pBot);
			ClearBotStuck(pBot);
			return;
		}
		else
		{
			// Pick the next point in the path
			BotNavInfo->CurrentPathPoint++;

			if (BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].area != SAMPLE_POLYAREA_CROUCH && BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint + 1].area != SAMPLE_POLYAREA_CROUCH)
			{
				BotNavInfo->CurrentPathPoint = GetNextDirectFlightPath(pBot);
			}

			pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location = pBot->pEdict->v.origin;

			CurrentMoveDest = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;
			NextPoint = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].Location;
			ClosestPointToPath = vClosestPointOnLine(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location, CurrentMoveDest, pEdict->v.origin);

			MoveFrom = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location;

			ClearBotStuck(pBot);
		}
	}

	Vector MoveDir = UTIL_GetVectorNormal(NextPoint - pBot->pEdict->v.origin);

	Vector ObstacleCheck = pBot->pEdict->v.origin + (MoveDir * 32.0f);

	if (vDist3DSq(pBot->pEdict->v.origin, ClosestPointToPath) > sqrf(100.0f) || !UTIL_QuickTrace(pBot->pEdict, pBot->pEdict->v.origin, ClosestPointToPath) || !UTIL_QuickTrace(pBot->pEdict, pBot->pEdict->v.origin, ObstacleCheck))
	{
		BotRecalcPath(pBot, BotNavInfo->ActualMoveDestination);
		return;
	}


	if (IsBotStuck(pBot, CurrentMoveDest))
	{
		if (BotNavInfo->TotalStuckTime > 3.0f)
		{
			ClearBotPath(pBot);
			return;
		}
	}

	float Velocity = vSize2DSq(pBot->pEdict->v.velocity);

	bool bMustHugGround = (pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area == SAMPLE_POLYAREA_CROUCH || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].area == SAMPLE_POLYAREA_CROUCH);

	if (!bMustHugGround || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint - 1].Location.z <= pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location.z)
	{
		if (Velocity < sqrf(500.f))
		{
			if (!(pBot->pEdict->v.oldbuttons & IN_JUMP))
			{
				pBot->pEdict->v.button |= IN_JUMP;
			}
			else
			{
				if (gpGlobals->time - BotNavInfo->LastFlapTime < 0.2f)
				{
					pBot->pEdict->v.button |= IN_JUMP;
				}
				else
				{
					BotNavInfo->LastFlapTime = gpGlobals->time;
				}
			}
		}
		else
		{
			pBot->pEdict->v.button |= IN_JUMP;
		}
	}



	pBot->desiredMovementDir = UTIL_GetForwardVector2D(pBot->pEdict->v.v_angle);

	Vector LookLocation = CurrentMoveDest;


	if (ClosestPointToPath.z - pBot->pEdict->v.origin.z > 8.0f)
	{
		LookLocation.z += 50.0f;
	}
	else
	{
		// Crouch areas need the lerk to stick close to the ground to avoid missing the crouch entry point
		
		bool bIsFinalPoint = pBot->BotNavInfo.CurrentPathPoint == pBot->BotNavInfo.PathSize - 1;

		if (bMustHugGround || bIsFinalPoint)
		{
			if (pBot->pEdict->v.origin.z - CurrentMoveDest.z > 8.0f)
			{
				LookLocation.z -= 50.0f;
			}
		}
		else
		{
			if (UTIL_QuickHullTrace(pBot->pEdict, pBot->pEdict->v.origin, CurrentMoveDest + Vector(0.0f, 0.0f, 50.0f)))
			{
				LookLocation.z += 50.0f;
			}
		}
	}


	
	BotMoveLookAt(pBot, LookLocation);

	CheckAndHandleBreakableObstruction(pBot, MoveFrom, CurrentMoveDest);

	if (gpGlobals->time - pBot->LastUseTime >= 3.0f)
	{
		CheckAndHandleDoorObstruction(pBot, MoveFrom, CurrentMoveDest);
	}
}

void BotFollowPath(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0)
	{
		return;
	}

	nav_status* BotNavInfo = &pBot->BotNavInfo;
	edict_t* pEdict = pBot->pEdict;

	SamplePolyAreas CurrentMoveArea = (SamplePolyAreas)BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].area;

	// If we've reached our current path point
	if (HasBotReachedPathPoint(pBot))
	{
		ClearBotStuck(pBot);

		// End of the whole path, stop all movement
		if (pBot->BotNavInfo.CurrentPathPoint == (pBot->BotNavInfo.PathSize - 1))
		{
			ClearBotPath(pBot);
			return;
		}
		else
		{
			// Pick the next point in the path
			pBot->BotNavInfo.CurrentPathPoint++;
		}
	}

	if (IsBotOffPath(pBot))
	{
		ClearBotPath(pBot);
		return;
	}

	Vector TargetMoveLocation = BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].Location;

	bool bIsUsingPhaseGate = (BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].area == SAMPLE_POLYAREA_PHASEGATE);

	bool bIsJumping = (BotNavInfo->CurrentPath[BotNavInfo->CurrentPathPoint].area == SAMPLE_POLYAREA_JUMP);

	if (bIsJumping)
	{
		bool thing = true;
	}

	if (!bIsUsingPhaseGate && IsBotStuck(pBot, TargetMoveLocation))
	{
		if (BotNavInfo->TotalStuckTime > 3.0f)
		{
			ClearBotPath(pBot);
			return;
		}

		// If onos, ducking is usually a good way to get unstuck...
		if (IsPlayerOnos(pBot->pEdict))
		{
			pEdict->v.button |= IN_DUCK;
		}

		if (!IsPlayerClimbingWall(pBot->pEdict) && !IsPlayerOnLadder(pBot->pEdict))
		{
			PerformUnstuckMove(pBot, TargetMoveLocation);
			return;
		}
	}

	NewMove(pBot);

}

void PerformUnstuckMove(bot_t* pBot, const Vector MoveDestination)
{

	Vector FwdDir = UTIL_GetVectorNormal2D(MoveDestination - pBot->pEdict->v.origin);
	pBot->desiredMovementDir = FwdDir;

	Vector HeadLocation = GetPlayerTopOfCollisionHull(pBot->pEdict, false);

	bool bMustCrouch = false;

	if (!IsPlayerSkulk(pBot->pEdict) && !IsPlayerGorge(pBot->pEdict) && !UTIL_QuickTrace(pBot->pEdict, HeadLocation, (HeadLocation + (FwdDir * 50.0f))))
	{
		pBot->pEdict->v.button |= IN_DUCK;
		bMustCrouch = true;
	}

	Vector MoveRightVector = UTIL_GetVectorNormal2D(UTIL_GetCrossProduct(FwdDir, UP_VECTOR));

	Vector BotRightSide = (pBot->pEdict->v.origin + (MoveRightVector * GetPlayerRadius(pBot->pEdict)));
	Vector BotLeftSide = (pBot->pEdict->v.origin - (MoveRightVector * GetPlayerRadius(pBot->pEdict)));

	bool bBlockedLeftSide = !UTIL_QuickTrace(pBot->pEdict, BotRightSide, BotRightSide + (FwdDir * 50.0f));
	bool bBlockedRightSide = !UTIL_QuickTrace(pBot->pEdict, BotLeftSide, BotLeftSide + (FwdDir * 50.0f));

	if (!bMustCrouch)
	{
		BotJump(pBot);
	}


	if (bBlockedRightSide && !bBlockedLeftSide)
	{
		pBot->desiredMovementDir = MoveRightVector;
		return;
	}
	else if (!bBlockedRightSide && bBlockedLeftSide)
	{
		pBot->desiredMovementDir = -MoveRightVector;
		return;
	}
	else
	{
		bBlockedLeftSide = !UTIL_QuickTrace(pBot->pEdict, BotRightSide, BotRightSide - (MoveRightVector * 50.0f));
		bBlockedRightSide = !UTIL_QuickTrace(pBot->pEdict, BotLeftSide, BotLeftSide + (MoveRightVector * 50.0f));

		if (bBlockedRightSide)
		{
			pBot->desiredMovementDir = -MoveRightVector;
		}
		else if (bBlockedLeftSide)
		{
			pBot->desiredMovementDir = MoveRightVector;
		}
		else
		{
			pBot->desiredMovementDir = FwdDir;
		}

	}

}

bool IsBotStuck(bot_t* pBot, const Vector MoveDestination)
{
	// If invalid move destination then bail out
	if (!MoveDestination) { return false; }

	// If moving to a new destination set a new distance baseline. We do not reset the stuck timer
	if (MoveDestination != pBot->BotNavInfo.StuckCheckMoveLocation)
	{
		float CurrentDistFromDestination = vDist3DSq(pBot->pEdict->v.origin, MoveDestination);
		pBot->BotNavInfo.StuckCheckMoveLocation = MoveDestination;
		pBot->BotNavInfo.LastDistanceFromDestination = CurrentDistFromDestination;
		return false;
	}

	// If first time performing a stuck check, set a baseline time and return false
	if (pBot->BotNavInfo.LastStuckCheckTime == 0.0f)
	{
		pBot->BotNavInfo.LastStuckCheckTime = gpGlobals->time;
		return false;
	}

	// Get the delta (usually frame time) between our last stuck check time. Will be appended to the total stuck time if bot is stuck
	float StuckCheckDelta = (gpGlobals->time - pBot->BotNavInfo.LastStuckCheckTime);
	pBot->BotNavInfo.LastStuckCheckTime = gpGlobals->time;

	// Get distance to destination
	float CurrentDistFromDestination = vDist3DSq(pBot->pEdict->v.origin, MoveDestination);

	// If this is the first time we're checking if we're stuck for this location, then set a baseline distance and bail
	if (pBot->BotNavInfo.LastDistanceFromDestination == 0.0f)
	{
		pBot->BotNavInfo.LastDistanceFromDestination = CurrentDistFromDestination;

		return false;
	}

	// If we've not managed to get any closer to our destination since the last check, then we're stuck.
	//Increase the total stuck time, and we're stuck if we've been unable to progress for 1 second or longer
	if (CurrentDistFromDestination >= pBot->BotNavInfo.LastDistanceFromDestination)
	{
		pBot->BotNavInfo.TotalStuckTime += StuckCheckDelta;

		if (pBot->BotNavInfo.TotalStuckTime >= 1.0f)
		{
			return true;
		}
	}
	else
	{
		// We managed to make some progress, we're not stuck. Set a new baseline for distance to destination
		pBot->BotNavInfo.TotalStuckTime = 0.0f;
		pBot->BotNavInfo.LastDistanceFromDestination = CurrentDistFromDestination;
		return false;
	}



	return false;
}

bool BotIsAtLocation(const bot_t* pBot, const Vector Destination)
{
	if (!Destination || !(pBot->pEdict->v.flags & FL_ONGROUND)) { return false; }

	return (vDist2DSq(pBot->pEdict->v.origin, Destination) < sqrf(GetPlayerRadius(pBot->pEdict)) && fabs(pBot->CurrentFloorPosition.z - Destination.z) <= GetPlayerHeight(pBot->pEdict, false));
}

Vector UTIL_ProjectPointToNavmesh(const Vector Location)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return ZERO_VECTOR; }

	Vector TraceHit = UTIL_GetTraceHitLocation(Location + Vector(0.0f, 0.0f, 10.0f), Location - Vector(0.0f, 0.0f, 500.0f));

	Vector PointToProject = (TraceHit != ZERO_VECTOR) ? TraceHit : Location;

	float pCheckLoc[3] = { PointToProject.x, PointToProject.z, -PointToProject.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];
	float Extents[3] = { 400.0f, 400.0f, 400.0f };

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, Extents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		return Vector(NavNearest[0], -NavNearest[2], NavNearest[1]);
	}
	else
	{
		return ZERO_VECTOR;
	}
}

Vector UTIL_ProjectPointToNavmesh(const Vector Location, const Vector Extents)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(ALL_NAV_PROFILE);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(ALL_NAV_PROFILE);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(ALL_NAV_PROFILE);

	if (!m_navQuery) { return ZERO_VECTOR; }

	float extents[3] = { Extents.x, Extents.z, Extents.y };

	Vector TraceHit = UTIL_GetTraceHitLocation(Location + Vector(0.0f, 0.0f, 1.0f), Location - Vector(0.0f, 0.0f, 500.0f));

	Vector PointToProject = (TraceHit != ZERO_VECTOR) ? TraceHit : Location;

	float pCheckLoc[3] = { PointToProject.x, PointToProject.z, -PointToProject.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, extents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		return Vector(NavNearest[0], -NavNearest[2], NavNearest[1]);
	}
	else
	{
		return ZERO_VECTOR;
	}
}

Vector UTIL_ProjectPointToNavmesh(const Vector Location, const int NavProfileIndex)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return ZERO_VECTOR; }

	float pCheckLoc[3] = { Location.x, Location.z, -Location.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, pExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		return Vector(NavNearest[0], -NavNearest[2], NavNearest[1]);
	}
	else
	{
		return ZERO_VECTOR;
	}
}

Vector UTIL_ProjectPointToNavmesh(const Vector Location, const Vector Extents, const int NavProfileIndex)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return ZERO_VECTOR; }

	float pCheckLoc[3] = { Location.x, Location.z, -Location.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	float fExtents[3] = { Extents.x, Extents.z, Extents.y };

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, fExtents, m_navFilter, &FoundPoly, NavNearest);

	if (dtStatusSucceed(success))
	{
		return Vector(NavNearest[0], -NavNearest[2], NavNearest[1]);
	}
	else
	{
		return ZERO_VECTOR;
	}
}

bool UTIL_PointIsOnNavmesh(const Vector Location, const int NavProfileIndex)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return false; }

	float pCheckLoc[3] = { Location.x, Location.z, -Location.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	float pCheckExtents[3] = { 5.0f, 72.0f, 5.0f };

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, pCheckExtents, m_navFilter, &FoundPoly, NavNearest);

	return dtStatusSucceed(success) && FoundPoly > 0;

}

bool UTIL_PointIsOnNavmesh(const int NavProfileIndex, const Vector Location, const Vector SearchExtents)
{
	const dtNavMeshQuery* m_navQuery = UTIL_GetNavMeshQueryForProfile(NavProfileIndex);
	const dtNavMesh* m_navMesh = UTIL_GetNavMeshForProfile(NavProfileIndex);
	const dtQueryFilter* m_navFilter = UTIL_GetNavMeshFilterForProfile(NavProfileIndex);

	if (!m_navQuery) { return false; }

	float pCheckLoc[3] = { Location.x, Location.z, -Location.y };

	dtPolyRef FoundPoly;
	float NavNearest[3];

	float pCheckExtents[3] = { SearchExtents.x, SearchExtents.z, SearchExtents.y };

	dtStatus success = m_navQuery->findNearestPoly(pCheckLoc, pCheckExtents, m_navFilter, &FoundPoly, NavNearest);

	return dtStatusSucceed(success) && FoundPoly > 0;

}

void HandlePlayerAvoidance(bot_t* pBot, const Vector MoveDestination)
{
	// Don't handle player avoidance if climbing a wall, ladder or in the air, as it will mess up the move and cause them to get stuck most likely
	if (IsPlayerOnLadder(pBot->pEdict) || IsPlayerClimbingWall(pBot->pEdict) || !pBot->BotNavInfo.IsOnGround) { return; }

	float avoidDistSq = sqrf(50.0f);
	const Vector BotLocation = pBot->pEdict->v.origin;
	const Vector MoveDir = UTIL_GetVectorNormal2D((MoveDestination - pBot->pEdict->v.origin));

	const int NavProfileIndex = UTIL_GetMoveProfileForBot(pBot, MOVESTYLE_NORMAL);

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != pBot->pEdict && IsPlayerActiveInGame(clients[i]))
		{
			// Don't do avoidance for a player if they're moving in broadly the same direction as us
			Vector OtherMoveDir = GetPlayerAttemptedMoveDirection(clients[i]);

			if (vDist3DSq(BotLocation, clients[i]->v.origin) <= avoidDistSq)
			{
				Vector BlockAngle = UTIL_GetVectorNormal2D(clients[i]->v.origin - BotLocation);
				float MoveBlockDot = UTIL_GetDotProduct2D(MoveDir, BlockAngle);

				// If other player is between us and our destination
				if (MoveBlockDot > 0.0f)
				{
					// If the other player is in the air or on top of us, back up and let them land
					if (!(clients[i]->v.flags & FL_ONGROUND) || clients[i]->v.groundentity == pBot->pEdict)
					{
						pBot->desiredMovementDir = UTIL_GetVectorNormal2D(BotLocation - clients[i]->v.origin);
						return;
					}

					// Determine if we should move left or right to clear them
					Vector MoveRightVector = UTIL_GetCrossProduct(MoveDir, UP_VECTOR);

					int modifier = vPointOnLine(pBot->pEdict->v.origin, MoveDestination, clients[i]->v.origin);

					if (modifier == 0) { modifier = 1; }

					Vector PreferredMoveDir = (MoveRightVector * modifier);

					// First see if we have enough room to move in our preferred avoidance direction
					if (UTIL_TraceNav(NavProfileIndex, BotLocation, BotLocation + (PreferredMoveDir * 32.0f), 2.0f))
					{
						pBot->desiredMovementDir = PreferredMoveDir;
						return;
					}

					// Then try the opposite direction
					if (UTIL_TraceNav(NavProfileIndex, BotLocation, BotLocation - (PreferredMoveDir * 32.0f), 2.0f))
					{
						pBot->desiredMovementDir = -PreferredMoveDir;
						return;
					}

					// Back up since we can't go either side
					if (UTIL_GetDotProduct2D(MoveDir, OtherMoveDir) < 0.0f)
					{
						pBot->desiredMovementDir = MoveDir * -1.0f;
					}
				}
			}
		}
	}
}

float UTIL_GetPathCostBetweenLocations(const int NavProfileIndex, const Vector FromLocation, const Vector ToLocation)
{
	bot_path_node path[MAX_PATH_SIZE];
	int pathSize;

	dtStatus pathFindResult = FindPathClosestToPoint(NavProfileIndex, FromLocation, ToLocation, path, &pathSize, max_player_use_reach);

	if (!dtStatusSucceed(pathFindResult)) { return 0.0f; }

	int currPathPoint = 1;
	float result = 0.0f;

	while (currPathPoint < (pathSize - 1))
	{

		result += vDist2DSq(path[currPathPoint - 1].Location, path[currPathPoint].Location) * NavProfiles[NavProfileIndex].Filters.getAreaCost(path[currPathPoint].area);
		currPathPoint++;
	}

	return sqrtf(result);
}

void ClearBotMovement(bot_t* pBot)
{
	pBot->BotNavInfo.TargetDestination = ZERO_VECTOR;
	pBot->BotNavInfo.ActualMoveDestination = ZERO_VECTOR;

	ClearBotPath(pBot);
	ClearBotStuck(pBot);
	ClearBotStuckMovement(pBot);

	pBot->LastPosition = pBot->pEdict->v.origin;
	pBot->TimeSinceLastMovement = 0.0f;
}

void ClearBotStuck(bot_t* pBot)
{
	pBot->BotNavInfo.LastDistanceFromDestination = 0.0f;
	pBot->BotNavInfo.LastStuckCheckTime = gpGlobals->time;
	pBot->BotNavInfo.TotalStuckTime = 0.0f;
	pBot->BotNavInfo.UnstuckMoveLocation = ZERO_VECTOR;
	pBot->BotNavInfo.UnstuckMoveLocationStartTime = 0.0f;
	pBot->BotNavInfo.UnstuckMoveStartLocation = ZERO_VECTOR;
	pBot->BotNavInfo.StuckCheckMoveLocation = ZERO_VECTOR;
}

bool BotRecalcPath(bot_t* pBot, const Vector Destination)
{
	ClearBotPath(pBot);

	Vector ValidNavmeshPoint = UTIL_ProjectPointToNavmesh(Destination, Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach), pBot->BotNavInfo.MoveStyle);

	// We can't actually get close enough to this point to consider it "reachable"
	if (!ValidNavmeshPoint)
	{
		sprintf(pBot->PathStatus, "Could not project destination to navmesh");
		return false;
	}

	dtStatus FoundPath = FindPathClosestToPoint(pBot, pBot->BotNavInfo.MoveStyle, pBot->CurrentFloorPosition, ValidNavmeshPoint, pBot->BotNavInfo.CurrentPath, &pBot->BotNavInfo.PathSize, max_player_use_reach);

	if (dtStatusSucceed(FoundPath))
	{
		pBot->BotNavInfo.TargetDestination = Destination;
		pBot->BotNavInfo.ActualMoveDestination = ValidNavmeshPoint;
		pBot->BotNavInfo.ActualMoveDestination = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.PathSize - 1].Location;

		if (vDist2DSq(pBot->BotNavInfo.CurrentPath[0].Location, pBot->pEdict->v.origin) > sqrf(GetPlayerRadius(pBot->pEdict)))
		{
			pBot->BotNavInfo.CurrentPathPoint = 0;
		}
		else
		{
			pBot->BotNavInfo.CurrentPathPoint = 1;
		}


		return true;
	}

	return false;
}

Vector UTIL_GetNearestLadderNormal(edict_t* pEdict)
{
	TraceResult result;
	edict_t* entity = NULL;

	entity = UTIL_FindEntityByClassname(entity, "func_ladder");

	edict_t* closestLadderRef = entity;
	float lowestDist = 999999.0f;

	while (!FNullEnt(entity))
	{
		Vector LadderMin = entity->v.absmin;
		Vector LadderMax = entity->v.absmax;

		float dist = vDistanceFromLine3D(LadderMin, LadderMax, pEdict->v.origin);

		if (dist < lowestDist)
		{
			closestLadderRef = entity;
			lowestDist = dist;
		}

		entity = UTIL_FindEntityByClassname(entity, "func_ladder");
	}

	if (closestLadderRef && !FNullEnt(closestLadderRef))
	{
		Vector CentrePoint = closestLadderRef->v.absmin + ((closestLadderRef->v.absmax - closestLadderRef->v.absmin) * 0.5f);
		CentrePoint.z = pEdict->v.origin.z;

		UTIL_TraceHull(pEdict->v.origin, CentrePoint, ignore_monsters, GetPlayerHullIndex(pEdict), pEdict->v.pContainingEntity, &result);

		if (result.flFraction < 1.0f)
		{
			return result.vecPlaneNormal;
		}
	}

	return Vector(0.0f, 0.0f, 0.0f);
}

Vector UTIL_GetNearestLadderBottomPoint(edict_t* pEdict)
{
	TraceResult result;
	edict_t* entity = NULL;

	entity = UTIL_FindEntityByClassname(entity, "func_ladder");

	edict_t* closestLadderRef = entity;
	float lowestDist = 999999.0f;

	while (entity && !FNullEnt(entity))
	{
		Vector LadderMin = entity->v.absmin;
		Vector LadderMax = entity->v.absmax;

		float dist = vDistanceFromLine3D(LadderMin, LadderMax, pEdict->v.origin);

		if (dist < lowestDist)
		{
			closestLadderRef = entity;
			lowestDist = dist;
		}

		entity = UTIL_FindEntityByClassname(entity, "func_ladder");
	}

	if (closestLadderRef && !FNullEnt(closestLadderRef))
	{
		Vector Centre = (closestLadderRef->v.absmin + ((closestLadderRef->v.absmax - closestLadderRef->v.absmin) * 0.5f));
		Centre.z = closestLadderRef->v.absmin.z;
		return Centre;

	}

	return pEdict->v.origin;
}

Vector UTIL_GetNearestLadderTopPoint(edict_t* pEdict)
{
	TraceResult result;
	edict_t* entity = NULL;

	entity = UTIL_FindEntityByClassname(entity, "func_ladder");

	edict_t* closestLadderRef = entity;
	float lowestDist = 999999.0f;

	while (entity && !FNullEnt(entity))
	{
		Vector LadderMin = entity->v.absmin;
		Vector LadderMax = entity->v.absmax;

		float dist = vDistanceFromLine3D(LadderMin, LadderMax, pEdict->v.origin);

		if (dist < lowestDist)
		{
			closestLadderRef = entity;
			lowestDist = dist;
		}

		entity = UTIL_FindEntityByClassname(entity, "func_ladder");
	}

	if (closestLadderRef && !FNullEnt(closestLadderRef))
	{
		Vector Centre = (closestLadderRef->v.absmin + ((closestLadderRef->v.absmax - closestLadderRef->v.absmin) * 0.5f));
		Centre.z = closestLadderRef->v.absmax.z;
		return Centre;

	}

	return pEdict->v.origin;
}

Vector UTIL_GetNearestLadderCentrePoint(edict_t* pEdict)
{
	TraceResult result;
	edict_t* entity = NULL;

	entity = UTIL_FindEntityByClassname(entity, "func_ladder");

	edict_t* closestLadderRef = entity;
	float lowestDist = 999999.0f;

	while (entity && !FNullEnt(entity))
	{
		Vector LadderMin = entity->v.absmin;
		Vector LadderMax = entity->v.absmax;

		float dist = vDistanceFromLine3D(LadderMin, LadderMax, pEdict->v.origin);

		if (dist < lowestDist)
		{
			closestLadderRef = entity;
			lowestDist = dist;
		}

		entity = UTIL_FindEntityByClassname(entity, "func_ladder");
	}

	if (closestLadderRef && !FNullEnt(closestLadderRef))
	{
		return (closestLadderRef->v.absmin + ((closestLadderRef->v.absmax - closestLadderRef->v.absmin) * 0.5f));

	}

	return pEdict->v.origin;
}

float UTIL_FindZHeightForWallClimb(const Vector ClimbStart, const Vector ClimbEnd, const int HullNum)
{
	TraceResult hit;

	Vector StartTrace = ClimbEnd;

	UTIL_TraceLine(ClimbEnd, ClimbEnd - Vector(0.0f, 0.0f, 50.0f), ignore_monsters, nullptr, &hit);

	if (hit.flFraction < 1.0f)
	{
		StartTrace.z = hit.vecEndPos.z + 18.0f;
	}

	Vector EndTrace = ClimbStart;
	EndTrace.z = StartTrace.z;

	Vector CurrTraceStart = StartTrace;


	UTIL_TraceHull(StartTrace, EndTrace, ignore_monsters, HullNum, nullptr, &hit);

	if (hit.flFraction >= 1.0f && !hit.fStartSolid)
	{
		return StartTrace.z;
	}
	else
	{
		int maxTests = 100;
		int testCount = 0;

		while ((hit.flFraction < 1.0f || hit.fStartSolid) && testCount < maxTests)
		{
			CurrTraceStart.z += 1.0f;
			EndTrace.z = CurrTraceStart.z;
			UTIL_TraceHull(CurrTraceStart, EndTrace, ignore_monsters, HullNum, nullptr, &hit);
			testCount++;
		}

		if (hit.flFraction >= 1.0f && !hit.fStartSolid)
		{
			return CurrTraceStart.z;
		}
		else
		{
			return StartTrace.z;
		}
	}

	return StartTrace.z;
}

void ClearBotPath(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize > 0)
	{
		pBot->BotNavInfo.PathSize = 0;
		memset(pBot->BotNavInfo.CurrentPath, 0, sizeof(pBot->BotNavInfo.CurrentPath));
	}

	//pBot->BotNavInfo.LastNavMeshPosition = ZERO_VECTOR;
}

void ClearBotStuckMovement(bot_t* pBot)
{
	pBot->BotNavInfo.UnstuckMoveLocation = ZERO_VECTOR;
	pBot->BotNavInfo.UnstuckMoveLocationStartTime = 0.0f;
	pBot->BotNavInfo.UnstuckMoveStartLocation = ZERO_VECTOR;
	//pBot->BotNavInfo.TotalStuckTime = 0.0f;
}

void DEBUG_DrawBotNextPathPoint(bot_t* pBot, float TimeInSeconds)
{
	if (pBot->BotNavInfo.PathSize > 0)
	{
		float DrawTime = fmaxf(TimeInSeconds, 0.1f);

		edict_t* pEdict = pBot->pEdict;
		int area = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;

		Vector StartLine = pBot->pEdict->v.origin;
		Vector EndLine = pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location;

		switch (area)
		{
		case SAMPLE_POLYAREA_GROUND:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 255, 255, 255);
			break;
		case SAMPLE_POLYAREA_CROUCH:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 255, 0, 0);
			break;
		case SAMPLE_POLYAREA_LADDER:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 0, 0, 255);
			break;
		case SAMPLE_POLYAREA_WALLCLIMB:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 0, 128, 0);
			break;
		case SAMPLE_POLYAREA_JUMP:
		case SAMPLE_POLYAREA_HIGHJUMP:
		case SAMPLE_POLYAREA_BLOCKED:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 255, 255, 0);
			break;
		case SAMPLE_POLYAREA_FALL:
		case SAMPLE_POLYAREA_HIGHFALL:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 0, 255, 255);
			break;
		default:
			UTIL_DrawLine(GAME_GetListenServerEdict(), StartLine, EndLine, DrawTime, 0, 0, 0);
			break;
		}
	}
}


void BotDrawPath(bot_t* pBot, float DrawTimeInSeconds, bool bShort)
{

	float DrawTime = fmaxf(DrawTimeInSeconds, 0.1f);

	if (pBot->BotNavInfo.PathSize > 0)
	{
		int numPoints = (bShort) ? 5 : pBot->BotNavInfo.PathSize;

		Vector FromDraw = pBot->BotNavInfo.CurrentPath[0].Location;

		UTIL_DrawLine(GAME_GetListenServerEdict(), pBot->BotNavInfo.CurrentPath[0].Location, pBot->BotNavInfo.CurrentPath[0].Location + Vector(0.0f, 0.0f, 50.0f), DrawTime, 255, 0, 0);

		unsigned char area = pBot->BotNavInfo.CurrentPath[0].area;
		for (int i = 1; i < pBot->BotNavInfo.PathSize; i++)
		{
			area = pBot->BotNavInfo.CurrentPath[i].area;
			if (area == SAMPLE_POLYAREA_GROUND)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime);
			}
			else if (area == SAMPLE_POLYAREA_CROUCH)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 255, 0, 0);
			}
			else if (area == SAMPLE_POLYAREA_JUMP || area == SAMPLE_POLYAREA_HIGHJUMP || area == SAMPLE_POLYAREA_BLOCKED)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 255, 255, 0);
			}
			else if (area == SAMPLE_POLYAREA_WALLCLIMB)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 0, 128, 0);
			}
			else if (area == SAMPLE_POLYAREA_LADDER)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 0, 0, 255);
			}
			else if (area == SAMPLE_POLYAREA_FALL || area == SAMPLE_POLYAREA_HIGHFALL)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 255, 0, 255);
			}
			else if (area == SAMPLE_POLYAREA_PHASEGATE)
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime, 64, 0, 0);
			}
			else
			{
				UTIL_DrawLine(GAME_GetListenServerEdict(), FromDraw, pBot->BotNavInfo.CurrentPath[i].Location, DrawTime);
			}

			FromDraw = pBot->BotNavInfo.CurrentPath[i].Location;

		}

		UTIL_DrawLine(GAME_GetListenServerEdict(), pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.PathSize - 1].Location, pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.PathSize - 1].Location + Vector(0.0f, 0.0f, 50.0f), DrawTime, 0, 0, 255);
	}

}

void BotMovementInputs(bot_t* pBot)
{
	if (pBot->desiredMovementDir == ZERO_VECTOR) { return; }

	edict_t* pEdict = pBot->pEdict;

	UTIL_NormalizeVector2D(&pBot->desiredMovementDir);

	float currentYaw = pBot->pEdict->v.v_angle.y;
	float moveDelta = UTIL_VecToAngles(pBot->desiredMovementDir).y;
	float angleDelta = currentYaw - moveDelta;

	float botSpeed = (pBot->BotNavInfo.bShouldWalk) ? (pBot->pEdict->v.maxspeed * 0.4f) : pBot->pEdict->v.maxspeed;

	if (angleDelta < -180.0f)
	{
		angleDelta += 360.0f;
	}
	else if (angleDelta > 180.0f)
	{
		angleDelta -= 360.0f;
	}

	if (angleDelta >= -22.5f && angleDelta < 22.5f)
	{
		pBot->ForwardMove = botSpeed;
		pBot->SideMove = 0.0f;
		pEdict->v.button |= IN_FORWARD;
	}
	else if (angleDelta >= 22.5f && angleDelta < 67.5f)
	{
		pBot->ForwardMove = botSpeed;
		pBot->SideMove = botSpeed;
		pEdict->v.button |= IN_FORWARD;
		pEdict->v.button |= IN_MOVERIGHT;
	}
	else if (angleDelta >= 67.5f && angleDelta < 112.5f)
	{
		pBot->ForwardMove = 0.0f;
		pBot->SideMove = botSpeed;
		pEdict->v.button |= IN_MOVERIGHT;
	}
	else if (angleDelta >= 112.5f && angleDelta < 157.5f)
	{
		pBot->ForwardMove = -botSpeed;
		pBot->SideMove = botSpeed;
		pEdict->v.button |= IN_BACK;
		pEdict->v.button |= IN_MOVERIGHT;
	}
	else if (angleDelta >= 157.5f || angleDelta <= -157.5f)
	{
		pBot->ForwardMove = -botSpeed;
		pBot->SideMove = 0.0f;
		pEdict->v.button |= IN_BACK;
	}
	else if (angleDelta >= -157.5f && angleDelta < -112.5f)
	{
		pBot->ForwardMove = -botSpeed;
		pBot->SideMove = -botSpeed;
		pEdict->v.button |= IN_BACK;
		pEdict->v.button |= IN_MOVELEFT;
	}
	else if (angleDelta >= -112.5f && angleDelta < -67.5f)
	{
		pBot->ForwardMove = 0.0f;
		pBot->SideMove = -botSpeed;
		pEdict->v.button |= IN_MOVELEFT;
	}
	else if (angleDelta >= -67.5f && angleDelta < -22.5f)
	{
		pBot->ForwardMove = botSpeed;
		pBot->SideMove = -botSpeed;
		pEdict->v.button |= IN_FORWARD;
		pEdict->v.button |= IN_MOVELEFT;
	}

	if (pBot->BotNavInfo.PathSize == 0 || pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area != SAMPLE_POLYAREA_LADDER)
	{
		if (IsPlayerOnLadder(pEdict))
		{
			BotJump(pBot);
		}
	}
}

void OnBotStartLadder(bot_t* pBot)
{
	pBot->CurrentLadderNormal = UTIL_GetNearestLadderNormal(pBot->pEdict);
}

void OnBotEndLadder(bot_t* pBot)
{
	pBot->CurrentLadderNormal = ZERO_VECTOR;
}

Vector UTIL_GetFurthestVisiblePointOnPath(const bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0) { return ZERO_VECTOR; }

	if (pBot->BotNavInfo.CurrentPathPoint == (pBot->BotNavInfo.PathSize - 1))
	{
		Vector MoveDir = UTIL_GetVectorNormal2D(pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location - pBot->pEdict->v.origin);
		return pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].Location + (MoveDir * 300.0f);
	}

	int lastVisiblePathPoint = 0;

	for (lastVisiblePathPoint = pBot->BotNavInfo.CurrentPathPoint + 1; lastVisiblePathPoint < pBot->BotNavInfo.PathSize; lastVisiblePathPoint++)
	{
		if (!UTIL_QuickTrace(pBot->pEdict, pBot->CurrentEyePosition, pBot->BotNavInfo.CurrentPath[lastVisiblePathPoint].Location))
		{
			lastVisiblePathPoint--;
			break;
		}
	}

	return pBot->BotNavInfo.CurrentPath[lastVisiblePathPoint].Location;
}

Vector UTIL_GetFurthestVisiblePointOnPath(const Vector ViewerLocation, const bot_path_node* path, const int pathSize, bool bPrecise)
{
	if (pathSize == 0) { return ZERO_VECTOR; }

	int StartPathIndex = (pathSize - 1);

	for (int i = StartPathIndex; i >= 0; i--)
	{

		if (UTIL_QuickTrace(NULL, ViewerLocation, path[i].Location))
		{
			if (!bPrecise || i == (pathSize - 1))
			{
				return path[i].Location;
			}
			else
			{
				Vector FromLoc = path[i].Location;
				Vector ToLoc = path[i + 1].Location;

				Vector Dir = UTIL_GetVectorNormal(ToLoc - FromLoc);

				float Dist = vDist3D(FromLoc, ToLoc);
				int Steps = (int)floorf(Dist / 50.0f);

				if (Steps == 0) { return FromLoc; }

				Vector FinalView = FromLoc;
				Vector ThisView = FromLoc + (Dir * 50.0f);

				for (int i = 0; i < Steps; i++)
				{
					if (UTIL_QuickTrace(NULL, ViewerLocation, ThisView))
					{
						FinalView = ThisView;
					}

					ThisView = ThisView + (Dir * 50.0f);
				}

				return FinalView;
			}
		}
		else
		{
			if (bPrecise && i < (pathSize - 1))
			{
				Vector FromLoc = path[i].Location;
				Vector ToLoc = path[i + 1].Location;

				Vector Dir = UTIL_GetVectorNormal(ToLoc - FromLoc);

				float Dist = vDist3D(FromLoc, ToLoc);
				int Steps = (int)floorf(Dist / 50.0f);

				if (Steps == 0) { continue; }

				Vector FinalView = ZERO_VECTOR;
				Vector ThisView = FromLoc + (Dir * 50.0f);

				for (int i = 0; i < Steps; i++)
				{
					if (UTIL_QuickTrace(NULL, ViewerLocation, ThisView))
					{
						FinalView = ThisView;
					}

					ThisView = ThisView + (Dir * 50.0f);
				}

				if (FinalView != ZERO_VECTOR)
				{
					return FinalView;
				}
				
			}
		}
	}

	return ZERO_VECTOR;
}

Vector UTIL_GetButtonFloorLocation(const Vector UserLocation, edict_t* ButtonEdict)
{
	Vector ClosestPoint = UTIL_GetClosestPointOnEntityToLocation(UserLocation, ButtonEdict);

	Vector ButtonAccessPoint = UTIL_ProjectPointToNavmesh(ClosestPoint, Vector(100.0f, 100.0f, 100.0f), MARINE_REGULAR_NAV_PROFILE);

	if (ButtonAccessPoint == ZERO_VECTOR)
	{
		ButtonAccessPoint = ClosestPoint;
	}

	Vector PlayerAccessLoc = ButtonAccessPoint;

	if (ButtonAccessPoint.z > ClosestPoint.z)
	{
		PlayerAccessLoc.z += 18.0f;
	}
	else
	{
		PlayerAccessLoc.z += 36.0f;
	}
	
	if (fabsf(PlayerAccessLoc.z - ClosestPoint.z) <= 60.0f)
	{
		return ButtonAccessPoint;
	}

	Vector NewProjection = ClosestPoint;

	if (ButtonAccessPoint.z > ClosestPoint.z)
	{
		NewProjection = ClosestPoint - Vector(0.0f, 0.0f, 100.0f);
	}
	else
	{
		NewProjection = ClosestPoint + Vector(0.0f, 0.0f, 100.0f);
	}

	Vector NewButtonAccessPoint = UTIL_ProjectPointToNavmesh(NewProjection, MARINE_REGULAR_NAV_PROFILE);

	if (NewButtonAccessPoint == ZERO_VECTOR)
	{
		NewButtonAccessPoint = ClosestPoint;
	}

	return NewButtonAccessPoint;
}

void UTIL_LinkTriggerToDoor(const edict_t* DoorEdict, nav_door* DoorRef)
{
	DoorRef->NumTriggers = 0;

	edict_t* currTrigger = NULL;
	while (((currTrigger = UTIL_FindEntityByClassname(currTrigger, "trigger_multiple")) != NULL) && (!FNullEnt(currTrigger)))
	{
		if (FStrEq(STRING(currTrigger->v.target), STRING(DoorEdict->v.targetname)))
		{
			DoorRef->TriggerEdicts[DoorRef->NumTriggers++] = currTrigger;
			DoorRef->ActivationType = DOOR_TRIGGER;

			if (DoorRef->NumTriggers >= 8) { return; }
		}
	}

	currTrigger = NULL;
	while (((currTrigger = UTIL_FindEntityByClassname(currTrigger, "func_button")) != NULL) && (!FNullEnt(currTrigger)))
	{
		if (FStrEq(STRING(currTrigger->v.target), STRING(DoorEdict->v.targetname)))
		{
			DoorRef->TriggerEdicts[DoorRef->NumTriggers++] = currTrigger;
			DoorRef->ActivationType = DOOR_BUTTON;

			if (DoorRef->NumTriggers >= 8) { return; }
		}
	}

	currTrigger = NULL;
	while (((currTrigger = UTIL_FindEntityByClassname(currTrigger, "trigger_once")) != NULL) && (!FNullEnt(currTrigger)))
	{
		if (FStrEq(STRING(currTrigger->v.target), STRING(DoorEdict->v.targetname)))
		{
			DoorRef->TriggerEdicts[DoorRef->NumTriggers++] = currTrigger;
			DoorRef->ActivationType = DOOR_TRIGGER;

			if (DoorRef->NumTriggers >= 8) { return; }
		}
	}

}


void UTIL_PopulateWeldableObstacles()
{
	memset(NavWeldableObstacles, 0, sizeof(NavWeldableObstacles));
	NumWeldableObstacles = 0;

	edict_t* currWeldable = NULL;
	while (((currWeldable = UTIL_FindEntityByClassname(currWeldable, "avhweldable")) != NULL) && (!FNullEnt(currWeldable)))
	{
		if (currWeldable->v.solid == SOLID_BSP)
		{
			NavWeldableObstacles[NumWeldableObstacles].WeldableEdict = currWeldable;

			float SizeX = currWeldable->v.size.x;
			float SizeY = currWeldable->v.size.y;
			float SizeZ = currWeldable->v.size.z;

			bool bUseXAxis = (SizeX >= SizeY);

			float CylinderRadius = fminf(SizeX, SizeY) * 0.5f;

			CylinderRadius = fmaxf(CylinderRadius, 16.0f);

			float Ratio = (bUseXAxis) ? (SizeX / (CylinderRadius * 2.0f)) : (SizeY / (CylinderRadius * 2.0f));

			int NumObstacles = (int)ceil(Ratio);

			if (NumObstacles > 32) { NumObstacles = 32; }

			Vector Dir = (bUseXAxis) ? RIGHT_VECTOR : FWD_VECTOR;

			Vector StartPoint = UTIL_GetCentreOfEntity(currWeldable);

			if (bUseXAxis)
			{
				StartPoint.x = currWeldable->v.absmin.x + CylinderRadius;
			}
			else
			{
				StartPoint.y = currWeldable->v.absmin.y + CylinderRadius;
			}

			StartPoint.z -= 2.0f;

			Vector CurrentPoint = StartPoint;

			NavWeldableObstacles[NumWeldableObstacles].NumObstacles = NumObstacles;

			for (int ii = 0; ii < NumObstacles; ii++)
			{
				UTIL_AddTemporaryObstacles(CurrentPoint, CylinderRadius, SizeZ, DT_TILECACHE_NULL_AREA, NavWeldableObstacles[NumWeldableObstacles].ObstacleRefs[ii]);

				if (bUseXAxis)
				{
					CurrentPoint.x += CylinderRadius * 2.0f;
				}
				else
				{
					CurrentPoint.y += CylinderRadius * 2.0f;
				}
			}

			NumWeldableObstacles++;
		}
	}
}

void UTIL_MarkDoorWeldable(const char* DoorTargetName)
{
	for (int i = 0; i < NumDoors; i++)
	{
		if (FStrEq(STRING(NavDoors[i].DoorEdict->v.targetname), DoorTargetName))
		{
			NavDoors[i].ActivationType = DOOR_WELD;

			if (NavDoors[i].NumObstacles > 0)
			{
				for (int ii = 0; ii < NavDoors[i].NumObstacles; ii++)
				{
					UTIL_RemoveTemporaryObstacles(NavDoors[i].ObstacleRefs[ii]);
				}
				NavDoors[i].NumObstacles = 0;
			}

			float SizeX = NavDoors[i].DoorEdict->v.size.x;
			float SizeY = NavDoors[i].DoorEdict->v.size.y;
			float SizeZ = NavDoors[i].DoorEdict->v.size.z;

			bool bUseXAxis = (SizeX >= SizeY);

			float CylinderRadius = fminf(SizeX, SizeY) * 0.5f;

			float Ratio = (bUseXAxis) ? (SizeX / (CylinderRadius * 2.0f)) : (SizeY / (CylinderRadius * 2.0f));

			int NumObstacles = (int)ceil(Ratio);

			if (NumObstacles > 32) { NumObstacles = 32; }

			Vector Dir = (bUseXAxis) ? RIGHT_VECTOR : FWD_VECTOR;

			Vector StartPoint = UTIL_GetCentreOfEntity(NavDoors[i].DoorEdict);

			if (bUseXAxis)
			{
				StartPoint.x = NavDoors[i].DoorEdict->v.absmin.x + CylinderRadius;
			}
			else
			{
				StartPoint.y = NavDoors[i].DoorEdict->v.absmin.y + CylinderRadius;
			}

			StartPoint.z -= 25.0f;

			Vector CurrentPoint = StartPoint;

			NavDoors[i].NumObstacles = NumObstacles;

			for (int ii = 0; ii < NumObstacles; ii++)
			{
				UTIL_AddTemporaryObstacles(CurrentPoint, CylinderRadius, SizeZ, DT_TILECACHE_NULL_AREA, NavDoors[i].ObstacleRefs[ii]);

				if (bUseXAxis)
				{
					CurrentPoint.x += CylinderRadius * 2.0f;
				}
				else
				{
					CurrentPoint.y += CylinderRadius * 2.0f;
				}
			}


		}
	}
}

void UTIL_UpdateWeldableObstacles()
{
	for (int i = 0; i < NumWeldableObstacles; i++)
	{
		if (NavWeldableObstacles[i].NumObstacles == 0) { continue; }

		edict_t* WeldableEdict = NavWeldableObstacles[i].WeldableEdict;

		if (FNullEnt(WeldableEdict) || WeldableEdict->v.deadflag != DEAD_NO || WeldableEdict->v.solid != SOLID_BSP)
		{
			for (int ii = 0; ii < NavWeldableObstacles[i].NumObstacles; ii++)
			{
				UTIL_RemoveTemporaryObstacles(NavWeldableObstacles[i].ObstacleRefs[ii]);
			}

			NavWeldableObstacles[i].NumObstacles = 0;
		}
	}
}

void UTIL_UpdateWeldableDoors()
{
	for (int i = 0; i < NumDoors; i++)
	{
		if (NavDoors[i].ActivationType == DOOR_WELD)
		{
			if (NavDoors[i].DoorEdict->v.velocity != ZERO_VECTOR || NavDoors[i].DoorEdict->v.avelocity != ZERO_VECTOR)
			{
				if (NavDoors[i].NumObstacles > 0)
				{
					for (int ii = 0; ii < NavDoors[i].NumObstacles; ii++)
					{
						UTIL_RemoveTemporaryObstacles(NavDoors[i].ObstacleRefs[ii]);
					}

					NavDoors[i].NumObstacles = 0;

				}
				continue;
			}

			Vector ThisLocation = UTIL_GetCentreOfEntity(NavDoors[i].DoorEdict);

			if (ThisLocation != NavDoors[i].CurrentPosition)
			{
				float SizeX = NavDoors[i].DoorEdict->v.size.x;
				float SizeY = NavDoors[i].DoorEdict->v.size.y;
				float SizeZ = NavDoors[i].DoorEdict->v.size.z;

				bool bUseXAxis = (SizeX >= SizeY);

				float CylinderRadius = fminf(SizeX, SizeY) * 0.5f;

				float Ratio = (bUseXAxis) ? (SizeX / (CylinderRadius * 2.0f)) : (SizeY / (CylinderRadius * 2.0f));

				int NumObstacles = (int)ceil(Ratio);

				if (NumObstacles > 32) { NumObstacles = 32; }

				Vector Dir = (bUseXAxis) ? RIGHT_VECTOR : FWD_VECTOR;

				Vector StartPoint = UTIL_GetCentreOfEntity(NavDoors[i].DoorEdict);

				if (bUseXAxis)
				{
					StartPoint.x = NavDoors[i].DoorEdict->v.absmin.x + CylinderRadius;
				}
				else
				{
					StartPoint.y = NavDoors[i].DoorEdict->v.absmin.y + CylinderRadius;
				}

				StartPoint.z -= 25.0f;

				Vector CurrentPoint = StartPoint;

				NavDoors[i].NumObstacles = NumObstacles;

				for (int ii = 0; ii < NumObstacles; ii++)
				{
					UTIL_AddTemporaryObstacles(CurrentPoint, CylinderRadius, SizeZ, DT_TILECACHE_NULL_AREA, NavDoors[i].ObstacleRefs[ii]);

					if (bUseXAxis)
					{
						CurrentPoint.x += CylinderRadius * 2.0f;
					}
					else
					{
						CurrentPoint.y += CylinderRadius * 2.0f;
					}
				}

				NavDoors[i].CurrentPosition = ThisLocation;
			}
		}
	}
}

// TODO: Need to add orientated box obstacle for door
void UTIL_PopulateDoors()
{
	memset(NavDoors, 0, sizeof(NavDoors));
	NumDoors = 0;

	edict_t* currDoor = NULL;
	while (((currDoor = UTIL_FindEntityByClassname(currDoor, "func_door")) != NULL) && (!FNullEnt(currDoor)))
	{
		NavDoors[NumDoors].DoorEdict = currDoor;
		NavDoors[NumDoors].PositionOne = UTIL_GetCentreOfEntity(currDoor);
		NavDoors[NumDoors].PositionTwo = UTIL_GetCentreOfEntity(currDoor) + (currDoor->v.movedir * (fabs(currDoor->v.movedir.x * (currDoor->v.size.x - 2)) + fabs(currDoor->v.movedir.y * (currDoor->v.size.y - 2)) + fabs(currDoor->v.movedir.z * (currDoor->v.size.z - 2)) - 0.0f));
		NavDoors[NumDoors].CurrentPosition = NavDoors[NumDoors].PositionOne;
		NavDoors[NumDoors].bStartOpen = (currDoor->v.flags & DOOR_START_OPEN);

		if (currDoor->v.spawnflags & DOOR_USE_ONLY)
		{
			NavDoors[NumDoors].ActivationType = DOOR_USE;
		}
		else
		{
			UTIL_LinkTriggerToDoor(currDoor, &NavDoors[NumDoors]);

		}

		NumDoors++;
	}

	currDoor = NULL;
	while (((currDoor = UTIL_FindEntityByClassname(currDoor, "func_seethroughdoor")) != NULL) && (!FNullEnt(currDoor)))
	{
		NavDoors[NumDoors].DoorEdict = currDoor;
		NavDoors[NumDoors].PositionOne = UTIL_GetCentreOfEntity(currDoor);
		NavDoors[NumDoors].PositionTwo = UTIL_GetCentreOfEntity(currDoor) + (currDoor->v.movedir * (fabs(currDoor->v.movedir.x * (currDoor->v.size.x - 2)) + fabs(currDoor->v.movedir.y * (currDoor->v.size.y - 2)) + fabs(currDoor->v.movedir.z * (currDoor->v.size.z - 2)) - 0.0f));
		NavDoors[NumDoors].CurrentPosition = NavDoors[NumDoors].PositionOne;

		if (currDoor->v.spawnflags & DOOR_USE_ONLY)
		{
			NavDoors[NumDoors].ActivationType = DOOR_USE;
		}
		else
		{
			UTIL_LinkTriggerToDoor(currDoor, &NavDoors[NumDoors]);
		}

		NumDoors++;
	}

	currDoor = NULL;
	while (((currDoor = UTIL_FindEntityByClassname(currDoor, "func_door_rotating")) != NULL) && (!FNullEnt(currDoor)))
	{
		NavDoors[NumDoors].DoorEdict = currDoor;
		NavDoors[NumDoors].PositionOne = UTIL_GetCentreOfEntity(currDoor);
		NavDoors[NumDoors].PositionTwo = UTIL_GetCentreOfEntity(currDoor) + (currDoor->v.movedir * (fabs(currDoor->v.movedir.x * (currDoor->v.size.x - 2)) + fabs(currDoor->v.movedir.y * (currDoor->v.size.y - 2)) + fabs(currDoor->v.movedir.z * (currDoor->v.size.z - 2)) - 0.0f));
		NavDoors[NumDoors].CurrentPosition = NavDoors[NumDoors].PositionOne;

		if (currDoor->v.spawnflags & DOOR_USE_ONLY)
		{
			NavDoors[NumDoors].ActivationType = DOOR_USE;
		}
		else
		{
			UTIL_LinkTriggerToDoor(currDoor, &NavDoors[NumDoors]);

		}

		NumDoors++;
	}

	BSP_RegisterWeldables();
}

const nav_door* UTIL_GetNavDoorByEdict(const edict_t* DoorEdict)
{
	for (int i = 0; i < NumDoors; i++)
	{
		if (NavDoors[i].DoorEdict == DoorEdict)
		{
			return &NavDoors[i];
		}
	}

	return nullptr;
}

unsigned char UTIL_GetBotCurrentPathArea(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0) { return SAMPLE_POLYAREA_GROUND; }

	return pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint].area;
}

unsigned char UTIL_GetNextBotCurrentPathArea(bot_t* pBot)
{
	if (pBot->BotNavInfo.PathSize == 0 || pBot->BotNavInfo.CurrentPathPoint >= pBot->BotNavInfo.PathSize - 1) { return SAMPLE_POLYAREA_GROUND; }

	return pBot->BotNavInfo.CurrentPath[pBot->BotNavInfo.CurrentPathPoint + 1].area;
}