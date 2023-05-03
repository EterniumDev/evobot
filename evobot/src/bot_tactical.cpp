//
// EvoBot - Neoptolemus' Natural Selection bot, based on Botman's HPB bot template
//
// bot_gorge.cpp
// 
// Contains gorge-related functions. Needs refactoring into helper function file
//

#include "bot_tactical.h"

#include <extdll.h>
#include <dllapi.h>

#include "player_util.h"
#include "general_util.h"
#include "bot_navigation.h"
#include "bot_config.h"
#include "bot_util.h"
#include "game_state.h"

#include "DetourTileCacheBuilder.h"

#include <unordered_map>


resource_node ResourceNodes[64];
int NumTotalResNodes;

hive_definition Hives[10];
int NumTotalHives;

map_location MapLocations[64];
int NumMapLocations;

float CommanderViewZHeight;

extern edict_t* clients[MAX_CLIENTS];
extern bool bGameIsActive;

extern bot_t bots[MAX_CLIENTS];

std::unordered_map<int, buildable_structure> MarineBuildableStructureMap;

std::unordered_map<int, buildable_structure> AlienBuildableStructureMap;

std::unordered_map<int, dropped_marine_item> MarineDroppedItemMap;



void PopulateEmptyHiveList()
{
	memset(Hives, 0, sizeof(Hives));
	NumTotalHives = 0;

	edict_t* currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_hive")) != NULL) && (!FNullEnt(currStructure)))
	{
		Hives[NumTotalHives].edict = currStructure;
		Hives[NumTotalHives].Location = currStructure->v.origin;
		Hives[NumTotalHives].FloorLocation = UTIL_GetFloorUnderEntity(currStructure);

		if (Hives[NumTotalHives].HiveResNodeIndex < 0)
		{
			Hives[NumTotalHives].HiveResNodeIndex = UTIL_FindNearestResNodeIndexToLocation(currStructure->v.origin);
		}

		NumTotalHives++;
	}
}

bool UTIL_StructureExistsOfType(const NSStructureType StructureType)
{
	return (UTIL_GetStructureCountOfType(StructureType) > 0);
}

edict_t* UTIL_GetNearestEquipment(const Vector Location, const float SearchDist, bool bUsePhaseDist)
{
	float SearchDistSq = sqrf(SearchDist);
	edict_t* Result = nullptr;
	float MinDist = 0.0f;

	for (auto& it : MarineDroppedItemMap)
	{
		if (!it.second.bOnNavMesh || !it.second.bIsReachableMarine) { continue; }
		
		if (it.second.ItemType != ITEM_MARINE_JETPACK && it.second.ItemType != ITEM_MARINE_HEAVYARMOUR) { continue; }

		float DistSq = (bUsePhaseDist) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(it.second.Location, Location);

		if (DistSq < SearchDistSq && (FNullEnt(Result) || DistSq < MinDist))
		{
			Result = it.second.edict;
			MinDist = DistSq;
		}

	}

	return Result;
}

void SetNumberofHives(int NewValue)
{
	NumTotalHives = NewValue;

	for (int i = 0; i < NumTotalHives; i++)
	{
		Hives[i].bIsValid = true;
	}
}

void SetHiveLocation(int HiveIndex, const Vector NewLocation)
{
	Hives[HiveIndex].Location = NewLocation;

	edict_t* ClosestHive = NULL;
	float minDist = 0.0f;

	edict_t* currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_hive")) != NULL) && (!FNullEnt(currStructure)))
	{
		float Dist = vDist3DSq(currStructure->v.origin, NewLocation);

		if (!ClosestHive || Dist < minDist)
		{
			ClosestHive = currStructure;
			minDist = Dist;
		}
	}

	Hives[HiveIndex].edict = ClosestHive;
	Hives[HiveIndex].FloorLocation = UTIL_GetFloorUnderEntity(ClosestHive);

	if (Hives[HiveIndex].HiveResNodeIndex < 0)
	{
		Hives[HiveIndex].HiveResNodeIndex = UTIL_FindNearestResNodeIndexToLocation(NewLocation);
	}

}

void SetHiveStatus(int HiveIndex, int NewStatus)
{

	switch (NewStatus)
	{
	case kHiveInfoStatusUnbuilt:
		Hives[HiveIndex].Status = HIVE_STATUS_UNBUILT;
		break;
	case kHiveInfoStatusBuildingStage1:
	case kHiveInfoStatusBuildingStage2:
	case kHiveInfoStatusBuildingStage3:
	case kHiveInfoStatusBuildingStage4:
	case kHiveInfoStatusBuildingStage5:
		Hives[HiveIndex].Status = HIVE_STATUS_BUILDING;
		break;
	case kHiveInfoStatusBuilt:
		Hives[HiveIndex].Status = HIVE_STATUS_BUILT;
		break;
	default: break;
	}

	if (Hives[HiveIndex].Status != HIVE_STATUS_UNBUILT && Hives[HiveIndex].ObstacleRef == 0)
	{
		Hives[HiveIndex].ObstacleRef = UTIL_AddTemporaryObstacle(UTIL_GetCentreOfEntity(Hives[HiveIndex].edict), 125.0f, 250.0f, DT_AREA_NULL);
	}

	if (Hives[HiveIndex].Status == HIVE_STATUS_UNBUILT && Hives[HiveIndex].ObstacleRef > 0)
	{
		UTIL_RemoveTemporaryObstacle(Hives[HiveIndex].ObstacleRef);
		Hives[HiveIndex].ObstacleRef = 0;
	}
}

void SetHiveTechStatus(int HiveIndex, int NewTechStatus)
{
	switch (NewTechStatus)
	{
	case 0:
		Hives[HiveIndex].TechStatus = HIVE_TECH_NONE;
		break;
	case 1:
		Hives[HiveIndex].TechStatus = HIVE_TECH_DEFENCE;
		break;
	case 2:
		Hives[HiveIndex].TechStatus = HIVE_TECH_SENSORY;
		break;
	case 3:
		Hives[HiveIndex].TechStatus = HIVE_TECH_MOVEMENT;
		break;
	default: Hives[HiveIndex].TechStatus = HIVE_TECH_NONE; break;
	}
}

void SetHiveUnderAttack(int HiveIndex, bool bNewUnderAttack)
{
	Hives[HiveIndex].bIsUnderAttack = bNewUnderAttack;
}

void SetHiveHealthPercent(int HiveIndex, float NewHealthPercent)
{
	Hives[HiveIndex].HealthPercent = NewHealthPercent;
}

buildable_structure* UTIL_GetBuildableStructureRefFromEdict(const edict_t* Structure)
{
	if (FNullEnt(Structure)) { return nullptr; }

	int EntIndex = ENTINDEX(Structure);

	if (EntIndex < 0) { return nullptr; }

	NSStructureType StructureType = GetStructureTypeFromEdict(Structure);

	if (StructureType == STRUCTURE_NONE) { return nullptr; }

	if (UTIL_IsMarineStructure(StructureType))
	{
		return &MarineBuildableStructureMap[EntIndex];
	}
	else
	{
		return &AlienBuildableStructureMap[EntIndex];
	}

	return nullptr;
}

edict_t* UTIL_GetClosestStructureAtLocation(const Vector& Location, bool bMarineStructures)
{
	edict_t* Result = NULL;
	float MinDist = 0.0f;

	if (bMarineStructures)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			float thisDist = vDist2DSq(it.second.Location, Location);

			if (!Result || thisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}

		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			float thisDist = vDist2DSq(it.second.Location, Location);

			if (!Result || thisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}

		}

	}

	return Result;
}

edict_t* UTIL_GetNearestItemOfType(const NSDeployableItem ItemType, const Vector Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);
	edict_t* Result = nullptr;
	float MinDist = 0.0f;

	for (auto& it : MarineDroppedItemMap)
	{
		if (!it.second.bOnNavMesh || !it.second.bIsReachableMarine) { continue; }

		if (it.second.ItemType != ItemType) { continue; }

		float DistSq = vDist2DSq(it.second.Location, Location);

		if (DistSq < SearchDistSq && (!Result || DistSq < MinDist))
		{
			Result = it.second.edict;
			MinDist = DistSq;
		}
	}

	return Result;
}

edict_t* UTIL_GetNearestUnbuiltStructureWithLOS(bot_t* pBot, const Vector Location, const float SearchDist, const int Team)
{
	edict_t* Result = nullptr;
	float MaxDist = sqrf(SearchDist);
	float MinDist = 0.0f;

	if (Team == MARINE_TEAM)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (it.second.bFullyConstructed) { continue; }

			if (!UTIL_PointIsDirectlyReachable(pBot->pEdict->v.origin, it.second.Location)) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (it.second.bFullyConstructed) { continue; }

			if (!UTIL_PointIsDirectlyReachable(pBot->pEdict->v.origin, it.second.Location)) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_GetNearestItemIndexOfType(const NSDeployableItem ItemType, const Vector Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);
	edict_t* Result = nullptr;
	float MinDist = 0.0f;


	for (auto& it : MarineDroppedItemMap)
	{
		if (!it.second.bOnNavMesh || !it.second.bIsReachableMarine) { continue; }

		if (it.second.ItemType != ItemType) { continue; }

		float DistSq = vDist2DSq(it.second.Location, Location);

		if (DistSq < SearchDistSq && (FNullEnt(Result) || DistSq < MinDist))
		{
			Result = it.second.edict;
			MinDist = DistSq;
		}
	}

	return Result;
}

edict_t* UTIL_GetNearestSpecialPrimaryWeapon(const Vector Location, const NSDeployableItem ExcludeItem, const float SearchDist, bool bUsePhaseDist)
{
	float SearchDistSq = sqrf(SearchDist);
	edict_t* Result = nullptr;
	float MinDist = 0.0f;

	for (auto& it : MarineDroppedItemMap)
	{
		if (!it.second.bOnNavMesh || !it.second.bIsReachableMarine) { continue; }

		if (it.second.ItemType == ExcludeItem || (it.second.ItemType != ITEM_MARINE_HMG && it.second.ItemType != ITEM_MARINE_SHOTGUN && it.second.ItemType != ITEM_MARINE_GRENADELAUNCHER)) { continue; }

		float DistSq = (bUsePhaseDist) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(it.second.Location, Location);

		if (DistSq < SearchDistSq && (FNullEnt(Result) || DistSq < MinDist))
		{
			Result = it.second.edict;
			MinDist = DistSq;
		}
	}

	return Result;
}

char* UTIL_GetClosestMapLocationToPoint(const Vector Point)
{
	if (NumMapLocations == 0)
	{
		return "";
	}

	for (int i = 0; i < NumMapLocations; i++)
	{
		if (Point.x >= MapLocations[i].MinLocation.x && Point.y >= MapLocations[i].MinLocation.y
			&& Point.x <= MapLocations[i].MaxLocation.x && Point.y <= MapLocations[i].MaxLocation.y)
		{
			return MapLocations[i].LocationName;
		}
	}

	int ClosestIndex = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumMapLocations; i++)
	{
		Vector CentrePoint = MapLocations[i].MinLocation + ((MapLocations[i].MaxLocation - MapLocations[i].MinLocation) * 0.5f);

		float ThisDist = vDist2DSq(Point, CentrePoint);

		if (ClosestIndex < 0 || ThisDist < MinDist)
		{
			ClosestIndex = i;
			MinDist = ThisDist;
		}
	}

	if (ClosestIndex > -1)
	{
		return MapLocations[ClosestIndex].LocationName;
	}
	else
	{
		return "";
	}
}

void SetCommanderViewZHeight(float NewValue)
{
	CommanderViewZHeight = NewValue;
}

float GetCommanderViewZHeight()
{
	return CommanderViewZHeight;
}

void AddMapLocation(const char* LocationName, Vector MinLocation, Vector MaxLocation)
{
	for (int i = 0; i < 64; i++)
	{
		if (!MapLocations[i].bIsValid)
		{
			sprintf(MapLocations[i].LocationName, "%s", UTIL_LookUpLocationName(LocationName));
			MapLocations[i].MinLocation = MinLocation;
			MapLocations[i].MaxLocation = MaxLocation;
			MapLocations[i].bIsValid = true;
			NumMapLocations = i + 1;
			return;
		}
		else
		{
			if (FStrEq(LocationName, MapLocations[i].LocationName))
			{
				return;
			}
		}
	}
}

void PrintHiveInfo()
{
	FILE* HiveLog = fopen("HiveInfo.txt", "w+");

	if (!HiveLog) { return; }

	for (int i = 0; i < NumTotalHives; i++)
	{
		fprintf(HiveLog, "Hive: %d\n", i);
		fprintf(HiveLog, "Hive Location: %f, %f, %f\n", Hives[i].Location.x, Hives[i].Location.y, Hives[i].Location.z);
		fprintf(HiveLog, "Hive Edict Location: %f, %f, %f\n", Hives[i].edict->v.origin.x, Hives[i].edict->v.origin.y, Hives[i].edict->v.origin.z);

		switch (Hives[i].Status)
		{
		case HIVE_STATUS_UNBUILT:
			fprintf(HiveLog, "Hive Status: Unbuilt\n");
			break;
		case HIVE_STATUS_BUILDING:
			fprintf(HiveLog, "Hive Status: Building\n");
			break;
		case HIVE_STATUS_BUILT:
			fprintf(HiveLog, "Hive Status: Built\n");
			break;
		default: break;
		}

		switch (Hives[i].TechStatus)
		{
		case HIVE_TECH_NONE:
			fprintf(HiveLog, "Hive Tech: None\n");
			break;
		case HIVE_TECH_DEFENCE:
			fprintf(HiveLog, "Hive Tech: Defence\n");
			break;
		case HIVE_TECH_MOVEMENT:
			fprintf(HiveLog, "Hive Tech: Movement\n");
			break;
		case HIVE_TECH_SENSORY:
			fprintf(HiveLog, "Hive Tech: Sensory\n");
			break;
		default: break;
		}

		fprintf(HiveLog, "Hive Under Attack: %s\n", (Hives[i].bIsUnderAttack) ? "True" : "False");
	}

	fflush(HiveLog);
	fclose(HiveLog);
}

void UTIL_RefreshBuildableStructures()
{
	edict_t* currStructure = NULL;

	// Marine Structures
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_command")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "resourcetower")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_infportal")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_armory")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_turretfactory")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_advturretfactory")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "siegeturret")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "turret")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_advarmory")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_armslab")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_prototypelab")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_observatory")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "phasegate")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}


	// Alien Structures
	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "alienresourcetower")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "defensechamber")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "offensechamber")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "movementchamber")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "sensorychamber")) != NULL) && (!FNullEnt(currStructure)))
	{
		UTIL_UpdateBuildableStructure(currStructure);
	}

	for (auto it = MarineBuildableStructureMap.begin(); it != MarineBuildableStructureMap.end();)
	{
		if (it->second.LastSeen < StructureRefreshFrame)
		{
			if (it->second.StructureType == STRUCTURE_MARINE_RESTOWER)
			{
				for (int i = 0; i < NumTotalResNodes; i++)
				{
					if (ResourceNodes[i].TowerEdict == it->second.edict)
					{
						ResourceNodes[i].TowerEdict = nullptr;
						ResourceNodes[i].bIsOccupied = false;
						ResourceNodes[i].bIsOwnedByMarines = false;
					}
				}
			}

			UTIL_OnStructureDestroyed(it->second.StructureType, it->second.Location);
			UTIL_RemoveTemporaryObstacle(it->second.ObstacleRef);
			it = MarineBuildableStructureMap.erase(it);
		}
		else
		{
			it++;
		}
	}

	for (auto it = AlienBuildableStructureMap.begin(); it != AlienBuildableStructureMap.end();)
	{
		if (it->second.LastSeen < StructureRefreshFrame)
		{
			if (it->second.StructureType == STRUCTURE_ALIEN_RESTOWER)
			{
				for (int i = 0; i < NumTotalResNodes; i++)
				{
					if (ResourceNodes[i].TowerEdict == it->second.edict)
					{
						ResourceNodes[i].TowerEdict = nullptr;
						ResourceNodes[i].bIsOccupied = false;
						ResourceNodes[i].bIsOwnedByMarines = false;
					}
				}
			}

			UTIL_OnStructureDestroyed(it->second.StructureType, it->second.Location);
			UTIL_RemoveTemporaryObstacle(it->second.ObstacleRef);
			it = AlienBuildableStructureMap.erase(it);
		}
		else
		{
			it++;
		}
	}

	StructureRefreshFrame++;
}

void UTIL_OnStructureCreated(buildable_structure* NewStructure)
{
	NSStructureType StructureType = GetStructureTypeFromEdict(NewStructure->edict);

	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bGameIsActive)
	{
		if (bIsMarineStructure)
		{
			for (int i = 0; i < 32; i++)
			{
				if (clients[i] && IsPlayerBot(clients[i]) && IsPlayerCommander(clients[i]))
				{
					bot_t* BotRef = GetBotPointer(clients[i]);

					if (BotRef)
					{
						UTIL_LinkPlacedStructureToAction(BotRef, NewStructure);
					}
				}
			}
		}
		else
		{
			for (int i = 0; i < 32; i++)
			{
				if (clients[i] && IsPlayerOnAlienTeam(clients[i]) && IsPlayerBot(clients[i]))
				{
					bot_t* BotRef = GetBotPointer(clients[i]);

					if (BotRef)
					{
						UTIL_LinkAlienStructureToTask(BotRef, NewStructure->edict);
					}
				}
			}
		}
	}

	if (StructureType == STRUCTURE_MARINE_RESTOWER || StructureType == STRUCTURE_ALIEN_RESTOWER)
	{

		int NearestResNodeIndex = UTIL_FindNearestResNodeIndexToLocation(NewStructure->edict->v.origin);

		if (NearestResNodeIndex > -1)
		{
			ResourceNodes[NearestResNodeIndex].bIsOccupied = true;
			ResourceNodes[NearestResNodeIndex].bIsOwnedByMarines = bIsMarineStructure;
			ResourceNodes[NearestResNodeIndex].TowerEdict = NewStructure->edict;
			ResourceNodes[NearestResNodeIndex].bIsMarineBaseNode = false;
		}
	}

}

void UTIL_LinkPlacedStructureToAction(bot_t* CommanderBot, buildable_structure* NewStructure)
{
	if (FNullEnt(NewStructure->edict)) { return; }

	NSStructureType StructureType = GetStructureTypeFromEdict(NewStructure->edict);

	for (int Priority = 0; Priority < MAX_ACTION_PRIORITIES; Priority++)
	{
		for (int ActionIndex = 0; ActionIndex < MAX_PRIORITY_ACTIONS; ActionIndex++)
		{
			commander_action* action = &CommanderBot->CurrentCommanderActions[Priority][ActionIndex];
			if (!action->bIsActive || action->ActionType != ACTION_BUILD || !action->bHasAttemptedAction || !FNullEnt(action->StructureOrItem) || !UTIL_StructureTypesMatch(StructureType, action->StructureToBuild)) { continue; }

			if (vDist2DSq(NewStructure->edict->v.origin, action->BuildLocation) < sqrf(UTIL_MetresToGoldSrcUnits(10.0f)))
			{
				action->StructureOrItem = NewStructure->edict;
				NewStructure->LastSuccessfulCommanderLocation = action->LastAttemptedCommanderLocation;
				NewStructure->LastSuccessfulCommanderAngle = action->LastAttemptedCommanderAngle;

			}
		}

	}
}

void UTIL_LinkDroppedItemToAction(bot_t* CommanderBot, const dropped_marine_item* NewItem)
{
	if (FNullEnt(NewItem->edict)) { return; }

	NSStructureType StructureType = GetStructureTypeFromEdict(NewItem->edict);

	for (int Priority = 0; Priority < MAX_ACTION_PRIORITIES; Priority++)
	{
		for (int ActionIndex = 0; ActionIndex < MAX_PRIORITY_ACTIONS; ActionIndex++)
		{
			commander_action* action = &CommanderBot->CurrentCommanderActions[Priority][ActionIndex];
			if (!action->bIsActive || action->ActionType != ACTION_DROPITEM) { continue; }
			if (FNullEnt(action->StructureOrItem) && vDist2DSq(NewItem->edict->v.origin, action->BuildLocation) < sqrf(UTIL_MetresToGoldSrcUnits(2.0f)))
			{
				action->StructureOrItem = NewItem->edict;
				return;

			}
		}
	}
}

/*void UTIL_OnStructureDestroyed(const buildable_structure* DestroyedStructure)
{
	if (DestroyedStructure->StructureType == STRUCTURE_MARINE_RESTOWER || DestroyedStructure->StructureType == STRUCTURE_ALIEN_RESTOWER)
	{
		int NearestResNodeIndex = UTIL_FindNearestResNodeIndexToLocation(DestroyedStructure->Location);

		if (NearestResNodeIndex > -1)
		{
			ResourceNodes[NearestResNodeIndex].bIsOccupied = false;
			ResourceNodes[NearestResNodeIndex].bIsOwnedByMarines = false;
			ResourceNodes[NearestResNodeIndex].TowerEdict = nullptr;
		}
	}

	if (DestroyedStructure->ObstacleRef > 0)
	{
		UTIL_RemoveTemporaryObstacle(DestroyedStructure->ObstacleRef);
	}

}*/

void UTIL_OnStructureDestroyed(const NSStructureType Structure, const Vector Location)
{
	if (Structure == STRUCTURE_MARINE_RESTOWER || Structure == STRUCTURE_ALIEN_RESTOWER)
	{
		int NearestResNodeIndex = UTIL_FindNearestResNodeIndexToLocation(Location);

		if (NearestResNodeIndex > -1)
		{
			ResourceNodes[NearestResNodeIndex].bIsOccupied = false;
			ResourceNodes[NearestResNodeIndex].bIsOwnedByMarines = false;
			ResourceNodes[NearestResNodeIndex].TowerEdict = nullptr;
		}
	}
}

bool UTIL_AlienResNodeNeedsReinforcing(int ResNodeIndex)
{
	if (ResNodeIndex < 0 || ResNodeIndex > NumTotalResNodes - 1) { return false; }

	int NumOffenceChambers = UTIL_GetNumPlacedStructuresOfTypeInRadius(STRUCTURE_ALIEN_OFFENCECHAMBER, ResourceNodes[ResNodeIndex].origin, UTIL_MetresToGoldSrcUnits(5.0f));

	if (NumOffenceChambers < 2) { return true; }

	if (UTIL_ActiveHiveWithTechExists(HIVE_TECH_DEFENCE))
	{
		int NumDefenceChambers = UTIL_GetNumPlacedStructuresOfTypeInRadius(STRUCTURE_ALIEN_DEFENCECHAMBER, ResourceNodes[ResNodeIndex].origin, UTIL_MetresToGoldSrcUnits(5.0f));

		if (NumDefenceChambers < 2) { return true; }
	}

	if (UTIL_ActiveHiveWithTechExists(HIVE_TECH_MOVEMENT))
	{
		int NumMovementChambers = UTIL_GetNumPlacedStructuresOfTypeInRadius(STRUCTURE_ALIEN_MOVEMENTCHAMBER, ResourceNodes[ResNodeIndex].origin, UTIL_MetresToGoldSrcUnits(5.0f));

		if (NumMovementChambers < 1) { return true; }
	}

	if (UTIL_ActiveHiveWithTechExists(HIVE_TECH_SENSORY))
	{
		int NumSensoryChambers = UTIL_GetNumPlacedStructuresOfTypeInRadius(STRUCTURE_ALIEN_SENSORYCHAMBER, ResourceNodes[ResNodeIndex].origin, UTIL_MetresToGoldSrcUnits(5.0f));

		if (NumSensoryChambers < 1) { return true; }
	}

	return false;
}

const resource_node* UTIL_GetNearestUnprotectedResNode(const Vector Location)
{
	int result = -1;
	float minDist = 0.0f;
	float MaxDistToClaim = sqrf(UTIL_MetresToGoldSrcUnits(10.0f));

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (FNullEnt(ResourceNodes[i].edict) || !ResourceNodes[i].bIsOccupied || ResourceNodes[i].bIsOwnedByMarines) { continue; }

		if (!UTIL_AlienResNodeNeedsReinforcing(i)) { continue; }

		float thisDist = vDist2DSq(ResourceNodes[i].edict->v.origin, Location);

		if (result < 0 || thisDist < minDist)
		{
			result = i;
			minDist = thisDist;
		}
	}

	if (result > -1)
	{
		return &ResourceNodes[result];
	}

	return nullptr;
}

const hive_definition* UTIL_GetClosestViableUnbuiltHive(const Vector SearchLocation)
{
	int Result = -1;
	float MinDist = 0.0f;


	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid && Hives[i].Status == HIVE_STATUS_UNBUILT)
		{
			if (UTIL_StructureOfTypeExistsInLocation(STRUCTURE_MARINE_PHASEGATE, Hives[i].Location, UTIL_MetresToGoldSrcUnits(15.0f))) { continue; }

			if (UTIL_StructureOfTypeExistsInLocation(STRUCTURE_MARINE_TURRETFACTORY, Hives[i].Location, UTIL_MetresToGoldSrcUnits(15.0f))) { continue; }

			float ThisDist = vDist2DSq(SearchLocation, Hives[i].Location);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

bool UTIL_HiveIsInProgress()
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid && Hives[i].Status == HIVE_STATUS_BUILDING) { return true; }
	}

	return false;
}

const hive_definition* UTIL_GetFirstHiveWithoutTech()
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid && Hives[i].Status == HIVE_STATUS_BUILT && Hives[i].TechStatus == HIVE_TECH_NONE) { return &Hives[i]; }
	}

	return nullptr;
}

const hive_definition* UTIL_GetHiveWithTech(HiveTechStatus Tech)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid && Hives[i].Status == HIVE_STATUS_BUILT && Hives[i].TechStatus == Tech) { return &Hives[i]; }
	}

	return nullptr;
}


edict_t* UTIL_GetClosestPlayerNeedsHealing(const Vector Location, const int Team, const float SearchRadius, edict_t* IgnorePlayer, bool bMustBeDirectlyReachable)
{
	edict_t* Result = nullptr;
	float MaxDist = sqrf(SearchRadius);
	float MinDist = 0.0f;


	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != IgnorePlayer && clients[i]->v.team == Team)
		{
			if (bMustBeDirectlyReachable && !UTIL_PointIsDirectlyReachable(Location, UTIL_GetFloorUnderEntity(clients[i]))) { continue; }

			if (GetPlayerOverallHealthPercent(clients[i]) > 0.95f) { continue; }

			float ThisDist = vDist2DSq(Location, clients[i]->v.origin);

			if (ThisDist < MaxDist && (!Result || ThisDist < MinDist))
			{
				Result = clients[i];
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_GetNearestMarineWithoutFullLoadout(const Vector SearchLocation, const float SearchRadius)
{
	edict_t* Result = nullptr;
	float MaxDist = sqrf(SearchRadius);
	float MinDist = 0.0f;


	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && IsPlayerMarine(clients[i]) && IsPlayerActiveInGame(clients[i]))
		{
			if (PlayerHasEquipment(clients[i]) && PlayerHasSpecialWeapon(clients[i]) && PlayerHasWeapon(clients[i], WEAPON_MARINE_WELDER)) { continue; }

			float ThisDist = vDist2DSq(SearchLocation, clients[i]->v.origin);

			if (ThisDist < MaxDist && (!Result || ThisDist < MinDist))
			{
				Result = clients[i];
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

const hive_definition* UTIL_GetActiveHiveWithoutChambers(HiveTechStatus ChamberType, int NumDesiredChambers)
{
	NSStructureType ChamberStructureType = UTIL_GetChamberTypeForHiveTech(ChamberType);

	if (ChamberStructureType == STRUCTURE_NONE) { return nullptr; }

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status != HIVE_STATUS_BUILT) { continue; }

		int NumChambersOfType = UTIL_GetNumPlacedStructuresOfTypeInRadius(ChamberStructureType, Hives[i].FloorLocation, UTIL_MetresToGoldSrcUnits(10.0f));

		if (NumChambersOfType < NumDesiredChambers) { return &Hives[i]; }
	}

	return nullptr;
}

bool UTIL_ActiveHiveWithTechExists(HiveTechStatus Tech)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status == HIVE_STATUS_BUILT && Hives[i].TechStatus == Tech) { return true; }
	}

	return false;
}

const hive_definition* UTIL_GetNearestHiveOfStatus(const Vector SearchLocation, const HiveStatusType Status)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status != Status) { continue; }

		float ThisDist = vDist2DSq(SearchLocation, Hives[i].FloorLocation);

		if (Result < 0 || ThisDist < MinDist)
		{
			MinDist = ThisDist;
			Result = i;
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

const hive_definition* UTIL_GetFurthestHiveOfStatus(const Vector SearchLocation, const HiveStatusType Status)
{
	int Result = -1;
	float MaxDist = 0.0f;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status != Status) { continue; }

		float ThisDist = vDist2DSq(SearchLocation, Hives[i].FloorLocation);

		if (Result < 0 || ThisDist > MaxDist)
		{
			MaxDist = ThisDist;
			Result = i;
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

const hive_definition* UTIL_GetNearestBuiltHiveToLocation(const Vector SearchLocation)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status == HIVE_STATUS_UNBUILT) { continue; }

		float ThisDist = vDist2DSq(SearchLocation, Hives[i].FloorLocation);

		if (Result < 0 || ThisDist < MinDist)
		{
			MinDist = ThisDist;
			Result = i;
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

edict_t* UTIL_GetNearestPlayerOfTeamInArea(const Vector Location, const float SearchRadius, const int Team, edict_t* IgnorePlayer, NSPlayerClass IgnoreClass)
{
	edict_t* Result = nullptr;
	float CheckDist = sqrf(SearchRadius);
	float MinDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != IgnorePlayer && clients[i]->v.team == Team && GetPlayerClass(clients[i]) != IgnoreClass && IsPlayerActiveInGame(clients[i]))
		{
			float ThisDist = vDist2DSq(clients[i]->v.origin, Location);

			if (ThisDist < CheckDist && (!Result || ThisDist < MinDist))
			{
				Result = clients[i];
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

int UTIL_GetNumPlayersOfTeamInArea(const Vector Location, const float SearchRadius, const int Team, edict_t* IgnorePlayer, NSPlayerClass IgnoreClass, bool bUsePhaseDist)
{
	int Result = 0;
	float CheckDist = sqrf(SearchRadius);
	float MinDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != IgnorePlayer && clients[i]->v.team == Team && GetPlayerClass(clients[i]) != IgnoreClass && IsPlayerActiveInGame(clients[i]))
		{
			float ThisDist = (bUsePhaseDist) ? UTIL_GetPhaseDistanceBetweenPointsSq(clients[i]->v.origin, Location) : vDist2DSq(clients[i]->v.origin, Location);

			if (ThisDist < CheckDist)
			{
				Result++;
			}
		}
	}

	return Result;
}

bool UTIL_IsPlayerOfTeamInArea(const Vector Location, const float SearchRadius, const int Team, edict_t* IgnorePlayer, NSPlayerClass IgnoreClass)
{
	float CheckDist = sqrf(SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && (FNullEnt(IgnorePlayer) || clients[i] != IgnorePlayer) && !IsPlayerCommander(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]) && clients[i]->v.team == Team && GetPlayerClass(clients[i]) != IgnoreClass)
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= CheckDist)
			{
				return true;
			}
		}
	}

	return false;
}

bool UTIL_IsAlienPlayerInArea(const Vector Location, float SearchRadius)
{
	float MaxDist = sqrf(SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && !IsPlayerDead(clients[i]) && IsPlayerOnAlienTeam(clients[i]))
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= MaxDist)
			{
				return true;
			}
		}
	}

	return false;
}

bool UTIL_IsAlienPlayerInArea(const Vector Location, float SearchRadius, edict_t* IgnorePlayer)
{
	float MaxDist = sqrf(SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerOnAlienTeam(clients[i]) && IsPlayerActiveInGame(clients[i]) && clients[i] != IgnorePlayer)
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= MaxDist)
			{
				return true;
			}
		}
	}

	return false;
}

bool UTIL_IsNearActiveHive(const Vector Location, float SearchRadius)
{
	float MaxDist = sqrf(SearchRadius);

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status != HIVE_STATUS_UNBUILT && vDist2DSq(Hives[i].Location, Location) <= MaxDist) { return true; }
	}

	return false;
}

edict_t* UTIL_GetAnyStructureOfTypeNearActiveHive(const NSStructureType StructureType, bool bAllowElectrical, bool bFullyConstructedOnly)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status != HIVE_STATUS_UNBUILT)
		{
			edict_t* ThreateningStructure = UTIL_GetNearestStructureOfTypeInLocation(StructureType, Hives[i].FloorLocation, UTIL_MetresToGoldSrcUnits(30.0f), bAllowElectrical, false);

			if (!FNullEnt(ThreateningStructure) && (!bFullyConstructedOnly || UTIL_StructureIsFullyBuilt(ThreateningStructure)))
			{
				return ThreateningStructure;
			}
		}
	}

	return nullptr;
}

edict_t* UTIL_GetAnyStructureOfTypeNearUnbuiltHive(const NSStructureType StructureType, bool bAllowElectrical, bool bFullyConstructedOnly)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status == HIVE_STATUS_UNBUILT)
		{
			edict_t* ThreateningPhaseGate = UTIL_GetNearestStructureOfTypeInLocation(StructureType, Hives[i].FloorLocation, UTIL_MetresToGoldSrcUnits(30.0f), bAllowElectrical, false);

			if (!FNullEnt(ThreateningPhaseGate) && (!bFullyConstructedOnly || UTIL_StructureIsFullyBuilt(ThreateningPhaseGate)))
			{
				return ThreateningPhaseGate;
			}
		}
	}

	return nullptr;
}

edict_t* UTIL_GetFirstPlacedStructureOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { return it.second.edict; }
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { return it.second.edict; }

		}
	}

	return nullptr;
}

edict_t* UTIL_GetFirstCompletedStructureOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (it.second.bFullyConstructed) { return it.second.edict; }
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (it.second.bFullyConstructed) { return it.second.edict; }
		}
	}

	return nullptr;
}

edict_t* UTIL_GetFirstIdleStructureOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (UTIL_StructureIsRecycling(it.second.edict)) { continue; }
			if (!it.second.bFullyConstructed) { continue; }

			if (!UTIL_StructureIsResearching(it.second.edict) && !UTIL_StructureIsUpgrading(it.second.edict)) { return it.second.edict; }

		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (it.second.bFullyConstructed) { return it.second.edict; }
		}
	}

	return nullptr;

}

int UTIL_GetNumPlacedStructuresOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	int result = 0;

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType)) { result++; }
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType)) { result++; }
		}
	}

	return result;
}

int UTIL_GetNumBuiltStructuresOfTypeInRadius(const NSStructureType StructureType, const Vector Location, const float MaxRadius)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	int result = 0;
	float MaxDist = sqrf(MaxRadius);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType) && it.second.bFullyConstructed)
			{
				if (vDist2DSq(it.second.Location, Location) <= MaxDist)
				{
					result++;
				}

			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType) && it.second.bFullyConstructed)
			{
				if (vDist2DSq(it.second.Location, Location) <= MaxDist)
				{
					result++;
				}

			}
		}
	}

	return result;
}

int UTIL_GetNumPlacedStructuresOfTypeInRadius(const NSStructureType StructureType, const Vector Location, const float MaxRadius)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	int result = 0;
	float MaxDist = sqrf(MaxRadius);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType))
			{
				if (vDist2DSq(it.second.Location, Location) <= MaxDist)
				{
					result++;
				}

			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType))
			{
				if (vDist2DSq(it.second.Location, Location) <= MaxDist)
				{
					result++;
				}

			}
		}
	}

	return result;
}

int UTIL_GetNumBuiltStructuresOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	int result = 0;

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType) && it.second.bFullyConstructed) { result++; }
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(it.second.StructureType, StructureType) && it.second.bFullyConstructed) { result++; }
		}
	}

	return result;
}

int UTIL_GetNearestAvailableResourcePointIndex(const Vector& SearchPoint)
{
	int result = -1;
	float minDist = 0.0f;

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (!ResourceNodes[i].edict || ResourceNodes[i].bIsOccupied) { continue; }

		float thisDist = vDist2DSq(ResourceNodes[i].edict->v.origin, SearchPoint);
		if (result < 0 || thisDist < minDist)
		{
			result = i;
			minDist = thisDist;
		}
	}

	return result;
}

int UTIL_GetNearestOccupiedResourcePointIndex(const Vector& SearchPoint)
{
	int result = -1;
	float minDist = 0.0f;

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (!ResourceNodes[i].edict || !ResourceNodes[i].bIsOccupied) { continue; }

		float thisDist = vDist2DSq(ResourceNodes[i].edict->v.origin, SearchPoint);
		if (result < 0 || thisDist < minDist)
		{
			result = i;
			minDist = thisDist;
		}
	}

	return result;
}

void UTIL_PopulateResourceNodeLocations()
{
	memset(ResourceNodes, 0, sizeof(ResourceNodes));
	NumTotalResNodes = 0;

	edict_t* commChair = NULL;
	Vector CommChairLocation = ZERO_VECTOR;

	commChair = UTIL_FindEntityByClassname(commChair, "team_command");

	if (!FNullEnt(commChair))
	{
		CommChairLocation = commChair->v.origin;
	}

	edict_t* currNode = NULL;
	while (((currNode = UTIL_FindEntityByClassname(currNode, "func_resource")) != NULL) && (!FNullEnt(currNode)))
	{

		bool bReachable = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, CommChairLocation, currNode->v.origin, 8.0f);

		if (bReachable || !CommChairLocation)
		{
			ResourceNodes[NumTotalResNodes].edict = currNode;
			ResourceNodes[NumTotalResNodes].origin = currNode->v.origin;
			ResourceNodes[NumTotalResNodes].TowerEdict = nullptr;
			ResourceNodes[NumTotalResNodes].bIsOccupied = false;
			ResourceNodes[NumTotalResNodes].bIsOwnedByMarines = false;
			ResourceNodes[NumTotalResNodes].bIsMarineBaseNode = false;

			UTIL_AddTemporaryObstacle(currNode->v.origin, 30.0f, 60.0f, DT_AREA_BLOCKED);

			NumTotalResNodes++;
		}
	}

	edict_t* currStructure = NULL;
	while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_command")) != NULL) && (!FNullEnt(currStructure)))
	{
		int NearestResNode = UTIL_FindNearestResNodeIndexToLocation(currStructure->v.origin);

		if (NearestResNode > -1)
		{
			ResourceNodes[NearestResNode].bIsMarineBaseNode = true;
		}
	}
}

HiveStatusType UTIL_GetHiveStatus(const edict_t* Hive)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].edict == Hive)
		{
			return Hives[i].Status;
		}
	}

	return HIVE_STATUS_UNBUILT;
}

int UTIL_GetItemCountOfTypeInArea(const NSDeployableItem ItemType, const Vector& SearchLocation, const float Radius)
{
	int Result = 0;
	float RadiusSq = sqrf(Radius);

	for (auto& it : MarineDroppedItemMap)
	{
		if (!it.second.bOnNavMesh || !it.second.bIsReachableMarine) { continue; }
		if (it.second.ItemType == ItemType && vDist2DSq(it.second.edict->v.origin, SearchLocation) <= RadiusSq)
		{
			Result++;
		}
	}

	return Result;

}

bool UTIL_StructureIsFullyBuilt(const edict_t* Structure)
{
	NSStructureType StructureType = GetStructureTypeFromEdict(Structure);

	if (StructureType == STRUCTURE_ALIEN_HIVE)
	{
		const hive_definition* Hive = UTIL_GetNearestHiveAtLocation(Structure->v.origin);

		return (Hive && Hive->Status != HIVE_STATUS_UNBUILT);
	}
	else
	{
		return !(Structure->v.iuser4 & MASK_BUILDABLE);
	}

}

int UTIL_GetStructureCountOfType(const NSStructureType StructureType)
{
	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	int Result = 0;

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { Result++; }

		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { Result++; }
		}
	}

	return Result;
}

int UTIL_FindNearestResNodeIndexToLocation(const Vector& Location)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (ResourceNodes[i].edict)
		{
			float ThisDist = vDist3DSq(Location, ResourceNodes[i].origin);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

const resource_node* UTIL_FindNearestResNodeToLocation(const Vector& Location)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (ResourceNodes[i].edict)
		{
			float ThisDist = vDist3DSq(Location, ResourceNodes[i].origin);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

void UTIL_ClearMapLocations()
{
	memset(MapLocations, 0, sizeof(MapLocations));
	NumMapLocations = 0;
	CommanderViewZHeight = 0.0f;
}

void UTIL_ClearHiveInfo()
{
	memset(Hives, 0, sizeof(Hives));
	NumTotalHives = 0;
}

void UTIL_ClearMapAIData()
{
	memset(ResourceNodes, 0, sizeof(ResourceNodes));
	NumTotalResNodes = 0;

	UTIL_ClearHiveInfo();

	MarineDroppedItemMap.clear();
	MarineBuildableStructureMap.clear();
	AlienBuildableStructureMap.clear();

	StructureRefreshFrame = 0;
	ItemRefreshFrame = 0;
}

const resource_node* UTIL_FindEligibleResNodeClosestToLocation(const Vector& Location, const int Team, bool bIgnoreElectrified)
{
	int Result = -1;
	float MinDist = 0.0f;


	for (int i = 0; i < NumTotalResNodes; i++)
	{
		bool ResNodeOccupiedByEnemy = (Team == MARINE_TEAM) ? !ResourceNodes[i].bIsOwnedByMarines : ResourceNodes[i].bIsOwnedByMarines;

		if (!ResourceNodes[i].bIsOccupied || (ResNodeOccupiedByEnemy && (bIgnoreElectrified || !UTIL_IsStructureElectrified(ResourceNodes[i].edict))))
		{

			if (Team == ALIEN_TEAM)
			{
				if (ResourceNodes[i].bIsOccupied && ResourceNodes[i].bIsMarineBaseNode) { continue; }

				if (!ResourceNodes[i].bIsOccupied)
				{
					bool bClaimedByOtherBot = false;

					for (int i = 0; i < 32; i++)
					{
						if (bots[i].is_used && IsPlayerOnAlienTeam(bots[i].pEdict) && IsPlayerActiveInGame(bots[i].pEdict))
						{
							if (bots[i].PrimaryBotTask.TaskLocation == ResourceNodes[i].origin || bots[i].SecondaryBotTask.TaskLocation == ResourceNodes[i].origin)
							{
								bClaimedByOtherBot = true;
								break;
							}
						}
					}

					if (bClaimedByOtherBot) { continue; }
				}

			}

			float Dist = vDist2DSq(Location, ResourceNodes[i].origin);
			if (Result < 0 || Dist < MinDist)
			{
				Result = i;
				MinDist = Dist;
			}
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

const resource_node* UTIL_FindEligibleResNodeFurthestFromLocation(const Vector& Location, const int Team, bool bIgnoreElectrified)
{
	int Result = -1;
	float MaxDist = 0.0f;


	for (int i = 0; i < NumTotalResNodes; i++)
	{
		bool ResNodeOccupiedByEnemy = (Team == MARINE_TEAM) ? !ResourceNodes[i].bIsOwnedByMarines : ResourceNodes[i].bIsOwnedByMarines;

		if (!ResourceNodes[i].bIsOccupied || (ResNodeOccupiedByEnemy && (bIgnoreElectrified || !UTIL_IsStructureElectrified(ResourceNodes[i].edict))))
		{

			if (Team == ALIEN_TEAM)
			{
				if (ResourceNodes[i].bIsOccupied && ResourceNodes[i].bIsMarineBaseNode) { continue; }

				if (!ResourceNodes[i].bIsOccupied)
				{
					bool bClaimedByOtherBot = false;

					for (int ii = 0; ii < 32; ii++)
					{
						if (bots[ii].is_used && IsPlayerOnAlienTeam(bots[ii].pEdict) && !IsPlayerDead(bots[ii].pEdict))
						{
							if (bots[ii].PrimaryBotTask.TaskLocation == ResourceNodes[i].origin || bots[ii].SecondaryBotTask.TaskLocation == ResourceNodes[i].origin)
							{
								bClaimedByOtherBot = true;
								break;
							}
						}
					}

					if (bClaimedByOtherBot)
					{
						continue;
					}
				}

			}

			float Dist = vDist2DSq(Location, ResourceNodes[i].origin);
			if (Result < 0 || Dist > MaxDist)
			{
				Result = i;
				MaxDist = Dist;
			}
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

const resource_node* UTIL_MarineFindUnclaimedResNodeNearestLocation(const bot_t* pBot, const Vector& Location, float MinDist)
{
	int Result = -1;
	float MaxDist = 0.0f;
	float MinDistSq = sqrf(MinDist);

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (ResourceNodes[i].bIsOccupied) { continue; }

		float Dist = UTIL_GetPhaseDistanceBetweenPointsSq(Location, ResourceNodes[i].origin);

		if (Dist < MinDistSq) { continue; }

		int ClaimedMarines = 0;

		for (int ii = 0; ii < 32; ii++)
		{
			if (bots[ii].is_used && bots[ii].pEdict != pBot->pEdict && IsPlayerOnMarineTeam(bots[ii].pEdict) && IsPlayerActiveInGame(bots[ii].pEdict))
			{
				if (vEquals(bots[ii].PrimaryBotTask.TaskLocation, ResourceNodes[i].origin, 10.0f) || vEquals(bots[ii].SecondaryBotTask.TaskLocation, ResourceNodes[i].origin, 10.0f))
				{

					ClaimedMarines++;
				}
			}
		}

		if (ClaimedMarines >= 2)
		{
			continue;
		}

		int NumOtherMarines = UTIL_GetNumPlayersOfTeamInArea(ResourceNodes[i].origin, UTIL_MetresToGoldSrcUnits(5.0f), MARINE_TEAM, pBot->pEdict, CLASS_NONE, false);

		if (NumOtherMarines >= 2)
		{
			continue;
		}

		if (Result < 0 || (Dist < MaxDist))
		{
			Result = i;
			MaxDist = Dist;
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

const resource_node* UTIL_AlienFindUnclaimedResNodeFurthestFromLocation(const bot_t* pBot, const Vector& Location, bool bIgnoreElectrified)
{
	int Result = -1;
	float MaxDist = 0.0f;


	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (ResourceNodes[i].bIsOccupied) { continue; }

		bool bClaimedByOtherBot = false;

		for (int ii = 0; ii < 32; ii++)
		{
			if (bots[ii].is_used && bots[ii].pEdict != pBot->pEdict && IsPlayerOnAlienTeam(bots[ii].pEdict) && IsPlayerActiveInGame(bots[ii].pEdict))
			{
				if (vEquals(bots[ii].PrimaryBotTask.TaskLocation, ResourceNodes[i].origin, 10.0f) || vEquals(bots[ii].SecondaryBotTask.TaskLocation, ResourceNodes[i].origin, 10.0f))
				{

					bClaimedByOtherBot = true;
					break;
				}
			}
		}

		if (bClaimedByOtherBot)
		{
			continue;
		}

		edict_t* OtherGorge = UTIL_GetNearestPlayerOfClass(ResourceNodes[i].origin, CLASS_GORGE, UTIL_MetresToGoldSrcUnits(5.0f), pBot->pEdict);

		if (OtherGorge && (GetPlayerResources(OtherGorge) >= kResourceTowerCost && vDist2DSq(OtherGorge->v.origin, ResourceNodes[i].origin) < vDist2DSq(pBot->pEdict->v.origin, ResourceNodes[i].origin)))
		{
			continue;
		}

		edict_t* Egg = UTIL_GetNearestPlayerOfClass(ResourceNodes[i].origin, CLASS_EGG, UTIL_MetresToGoldSrcUnits(5.0f), pBot->pEdict);

		if (Egg && (GetPlayerResources(Egg) >= kResourceTowerCost && vDist2DSq(Egg->v.origin, ResourceNodes[i].origin) < vDist2DSq(pBot->pEdict->v.origin, ResourceNodes[i].origin)))
		{
			continue;
		}

		float Dist = vDist2DSq(Location, ResourceNodes[i].origin);
		if (Result < 0 || Dist > MaxDist)
		{
			Result = i;
			MaxDist = Dist;
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

edict_t* UTIL_GetNearestUndefendedStructureOfTypeUnderAttack(bot_t* pBot, const NSStructureType StructureType)
{
	edict_t* Result = nullptr;
	float MinDist = 0.0f;

	bool bIsMarine = IsPlayerMarine(pBot->pEdict);

	if (StructureType == STRUCTURE_ALIEN_HIVE)
	{
		for (int i = 0; i < NumTotalHives; i++)
		{
			if (!Hives[i].bIsValid || Hives[i].Status == HIVE_STATUS_UNBUILT || !Hives[i].bIsUnderAttack) { continue; }

			int NumPotentialDefenders = UTIL_GetNumPlayersOfTeamInArea(Hives[i].FloorLocation, UTIL_MetresToGoldSrcUnits(15.0f), pBot->pEdict->v.team, pBot->pEdict, CLASS_GORGE, false);

			if (NumPotentialDefenders >= 3) { continue; }

			float ThisDist = (bIsMarine) ? UTIL_GetPhaseDistanceBetweenPointsSq(Hives[i].FloorLocation, pBot->pEdict->v.origin) : vDist2DSq(Hives[i].FloorLocation, pBot->pEdict->v.origin);

			if (FNullEnt(Result) || ThisDist < MinDist)
			{
				Result = Hives[i].edict;
				MinDist = ThisDist;
			}
		}

		return Result;
	}

	bool bMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bMarineStructure)
	{

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bUnderAttack || !it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			int NumPotentialDefenders = UTIL_GetNumPlayersOfTeamInArea(it.second.Location, UTIL_MetresToGoldSrcUnits(10.0f), pBot->pEdict->v.team, pBot->pEdict, CLASS_NONE, bIsMarine);

			if (NumPotentialDefenders >= 2) { continue; }

			float ThisDist = (bIsMarine) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, pBot->pEdict->v.origin) : vDist2DSq(it.second.Location, pBot->pEdict->v.origin);

			if (FNullEnt(Result) || ThisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}

		}

	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bUnderAttack || !it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			int NumPotentialDefenders = UTIL_GetNumPlayersOfTeamInArea(it.second.Location, UTIL_MetresToGoldSrcUnits(10.0f), pBot->pEdict->v.team, pBot->pEdict, CLASS_GORGE, bIsMarine);

			if (NumPotentialDefenders >= 2) { continue; }

			float ThisDist = (bIsMarine) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, pBot->pEdict->v.origin) : vDist2DSq(it.second.Location, pBot->pEdict->v.origin);

			if (FNullEnt(Result) || ThisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_GetNearestUnbuiltStructureOfTypeInLocation(const NSStructureType StructureType, const Vector& Location, const float SearchRadius)
{
	edict_t* Result = nullptr;
	float DistSq = sqrf(SearchRadius);
	float MinDist = 0.0f;

	bool bMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bMarineStructure)
	{

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh || it.second.bFullyConstructed) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			float ThisDist = vDist2DSq(it.second.Location, Location);

			if (ThisDist <= DistSq && (FNullEnt(Result) || ThisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}

		}

	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh || it.second.bFullyConstructed) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			float ThisDist = vDist2DSq(it.second.Location, Location);

			if (ThisDist <= DistSq && (FNullEnt(Result) || ThisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_FindSafePlayerInArea(const int Team, const Vector SearchLocation, float MinRadius, float MaxRadius)
{
	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && IsPlayerActiveInGame(clients[i]) && clients[i]->v.team == Team)
		{
			float Distance = vDist2DSq(clients[i]->v.origin, SearchLocation);

			if (Distance < sqrf(MinRadius) || Distance > sqrf(MaxRadius)) { continue; }

			edict_t* DangerTurret = PlayerGetNearestDangerTurret(clients[i], UTIL_MetresToGoldSrcUnits(15.0f));

			if (!FNullEnt(DangerTurret)) { continue; }

			int EnemyTeam = (Team == MARINE_TEAM) ? ALIEN_TEAM : MARINE_TEAM;

			if (UTIL_AnyPlayerOnTeamWithLOS(clients[i]->v.origin, EnemyTeam, UTIL_MetresToGoldSrcUnits(10.0f))) { continue; }

			return clients[i];
		}
	}

	return nullptr;
}

edict_t* UTIL_GetNearestStructureOfTypeInLocation(const NSStructureType StructureType, const Vector& Location, const float SearchRadius, bool bAllowElectrified, bool bUsePhaseDistance)
{
	edict_t* Result = nullptr;
	float DistSq = sqrf(SearchRadius);
	float MinDist = 0.0f;

	bool bMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bMarineStructure)
	{

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType) || (!bAllowElectrified && it.second.bIsElectrified)) { continue; }

			float ThisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(it.second.Location, Location);

			if (ThisDist <= DistSq && (FNullEnt(Result) || ThisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}

		}

	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			float ThisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(it.second.Location, Location);

			if (ThisDist <= DistSq && (FNullEnt(Result) || ThisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = ThisDist;
			}
		}
	}

	return Result;
}

bool UTIL_StructureOfTypeExistsInLocation(const NSStructureType StructureType, const Vector& Location, const float SearchRadius)
{
	float DistSq = sqrf(SearchRadius);

	bool bMarineStructure = UTIL_IsMarineStructure(StructureType);

	if (bMarineStructure)
	{

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			if (vDist2DSq(it.second.Location, Location) <= DistSq) { return true; }

		}

	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

			if (vDist2DSq(it.second.Location, Location) <= DistSq) { return true; }

		}
	}

	return false;
}

const resource_node* UTIL_GetNearestCappedResNodeToLocation(const Vector Location, int Team, bool bIgnoreElectrified)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		if (ResourceNodes[i].bIsOccupied && ResourceNodes[i].TowerEdict->v.team == Team && (bIgnoreElectrified || !UTIL_IsStructureElectrified(ResourceNodes[i].edict)))
		{
			if (ResourceNodes[i].bIsMarineBaseNode) { continue; }

			float ThisDist = vDist2DSq(Location, ResourceNodes[i].origin);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	if (Result > -1)
	{
		return &ResourceNodes[Result];
	}

	return nullptr;
}

edict_t* UTIL_GetRandomStructureOfType(const NSStructureType StructureType, const edict_t* IgnoreInstance, bool bFullyConstructedOnly)
{
	if (StructureType == STRUCTURE_ALIEN_HIVE)
	{
		edict_t* RandomHive = nullptr;
		int LowestRand = 0;

		for (int i = 0; i < NumTotalHives; i++)
		{
			if (!bFullyConstructedOnly || Hives[i].Status == HIVE_STATUS_BUILT)
			{
				float NewRand = irandrange(0, 100);

				if (!RandomHive || NewRand < LowestRand)
				{
					RandomHive = Hives[i].edict;
					LowestRand = NewRand;
				}
			}
		}

		return RandomHive;
	}

	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	edict_t* Result = nullptr;
	int LowestRand = 0;

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (bFullyConstructedOnly && !it.second.bFullyConstructed) { continue; }

			int NewRand = irandrange(0, 100);

			if (!Result || NewRand < LowestRand)
			{
				Result = it.second.edict;
				LowestRand = NewRand;
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (bFullyConstructedOnly && !it.second.bFullyConstructed) { continue; }

			int NewRand = irandrange(0, 100);

			if (!Result || NewRand < LowestRand)
			{
				Result = it.second.edict;
				LowestRand = NewRand;
			}
		}
	}

	return Result;
}

bool UTIL_CommChairExists()
{
	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (UTIL_StructureTypesMatch(STRUCTURE_MARINE_COMMCHAIR, it.second.StructureType)) { return true; }
	}

	return false;
}

Vector UTIL_GetCommChairLocation()
{
	if (MarineBuildableStructureMap.size() > 0)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (UTIL_StructureTypesMatch(STRUCTURE_MARINE_COMMCHAIR, it.second.StructureType)) { return it.second.Location; }
		}
	}
	else
	{
		edict_t* currStructure = NULL;

		// Marine Structures
		while (((currStructure = UTIL_FindEntityByClassname(currStructure, "team_command")) != NULL) && (!FNullEnt(currStructure)))
		{
			return currStructure->v.origin;
		}
	}

	return ZERO_VECTOR;
}

edict_t* UTIL_GetCommChair()
{
	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (UTIL_StructureTypesMatch(STRUCTURE_MARINE_COMMCHAIR, it.second.StructureType)) { return it.second.edict; }
	}

	return NULL;
}

edict_t* UTIL_GetNearestPlayerOfClass(const Vector Location, const NSPlayerClass SearchClass, const float SearchDist, const edict_t* PlayerToIgnore)
{
	edict_t* result = nullptr;
	float MaxDist = sqrf(SearchDist);
	float MinDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{
		if (FNullEnt(clients[i]) || clients[i] == PlayerToIgnore || IsPlayerDead(clients[i])) { continue; }

		if (!IsPlayerInReadyRoom(clients[i]) && !IsPlayerBeingDigested(clients[i]) && GetPlayerClass(clients[i]) == SearchClass)
		{
			float ThisDist = vDist2DSq(Location, clients[i]->v.origin);

			if (ThisDist < MaxDist && (!result || ThisDist < MinDist))
			{
				result = clients[i];
				MinDist = ThisDist;
			}
		}
	}

	return result;
}

int UTIL_GetNumResNodes()
{
	return NumTotalResNodes;
}

Vector UTIL_GetRandomPointOfInterest()
{

	int NumPointsOfInterest = NumTotalResNodes + NumTotalHives + 1; // +1 for the comm chair

	int RandomIndex = irandrange(0, NumPointsOfInterest - 1);

	// Comm chair is last index
	if (RandomIndex == NumPointsOfInterest - 1)
	{
		return UTIL_GetCommChairLocation();
	}

	// Hives are indexed after resource nodes
	if (RandomIndex > NumTotalResNodes - 1)
	{
		return Hives[RandomIndex - NumTotalResNodes].FloorLocation;
	}
	else
	{
		if (UTIL_PointIsOnNavmesh(ResourceNodes[RandomIndex].origin, ALL_NAV_PROFILE))
		{
			return ResourceNodes[RandomIndex].origin;
		}

	}

	return ZERO_VECTOR;
}

Vector UTIL_GetNearestPointOfInterestToLocation(const Vector SearchLocation, bool bUsePhaseDistance)
{
	Vector Result = UTIL_GetCommChairLocation();
	float MinDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, UTIL_GetCommChairLocation()) : vDist2DSq(SearchLocation, UTIL_GetCommChairLocation());

	for (int i = 0; i < NumTotalHives; i++)
	{
		float ThisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, Hives[i].FloorLocation) : vDist2DSq(SearchLocation, Hives[i].FloorLocation);

		if (ThisDist < MinDist)
		{
			Result = Hives[i].FloorLocation;
			MinDist = ThisDist;
		}
	}

	for (int i = 0; i < NumTotalResNodes; i++)
	{
		float ThisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, ResourceNodes[i].origin) : vDist2DSq(SearchLocation, ResourceNodes[i].origin);

		if (ThisDist < MinDist)
		{
			Result = ResourceNodes[i].origin;
			MinDist = ThisDist;
		}
	}

	return Result;
}

const hive_definition* UTIL_GetNearestHiveUnderSiege(const Vector SearchLocation)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status == HIVE_STATUS_UNBUILT) { continue; }

		if (UTIL_StructureOfTypeExistsInLocation(STRUCTURE_MARINE_PHASEGATE, Hives[i].Location, UTIL_MetresToGoldSrcUnits(25.0f)))
		{
			float ThisDist = vDist2DSq(SearchLocation, Hives[i].Location);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

bool UTIL_IsAnyHumanNearLocation(const Vector& Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerHuman(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]) && !IsPlayerCommander(clients[i]))
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= SearchDistSq)
			{
				return true;
			}
		}
	}

	return false;
}

edict_t* UTIL_GetNearestHumanAtLocation(const Vector& Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);
	edict_t* NearestHuman = NULL;
	float NearestDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerHuman(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]))
		{
			float CurrDist = vDist2DSq(clients[i]->v.origin, Location);
			if (CurrDist <= SearchDistSq)
			{
				if (!NearestHuman || CurrDist < NearestDist)
				{
					NearestHuman = clients[i];
					NearestDist = CurrDist;
				}

			}
		}
	}

	return NearestHuman;
}

bool UTIL_IsAnyHumanNearLocationWithoutWeapon(const NSWeapon WeaponType, const Vector& Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerHuman(clients[i]) && IsPlayerActiveInGame(clients[i]) && clients[i]->v.team == MARINE_TEAM)
		{
			if (!PlayerHasWeapon(clients[i], WeaponType) && vDist2DSq(clients[i]->v.origin, Location) <= SearchDistSq)
			{
				return true;
			}
		}
	}

	return false;
}

bool UTIL_IsAnyHumanNearLocationWithoutSpecialWeapon(const Vector& Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerHuman(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]) && !IsPlayerCommander(clients[i]))
		{
			if (PlayerHasWeapon(clients[i], WEAPON_MARINE_MG) && vDist2DSq(clients[i]->v.origin, Location) <= SearchDistSq)
			{
				return true;
			}
		}
	}

	return false;
}

bool UTIL_IsAnyHumanNearLocationWithoutEquipment(const Vector& Location, const float SearchDist)
{
	float SearchDistSq = sqrf(SearchDist);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerHuman(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]) && !IsPlayerCommander(clients[i]))
		{
			if (!PlayerHasEquipment(clients[i]) && vDist2DSq(clients[i]->v.origin, Location) <= SearchDistSq)
			{
				return true;
			}
		}
	}

	return false;
}

edict_t* UTIL_GetNearestStructureIndexOfType(const Vector& Location, NSStructureType StructureType, const float SearchDist, bool bFullyConstructedOnly, bool bUsePhaseGates)
{
	if (StructureType == STRUCTURE_ALIEN_HIVE)
	{
		edict_t* NearestHive = nullptr;
		float MaxDist = sqrf(SearchDist);
		float MinDist = 0.0f;

		for (int i = 0; i < NumTotalHives; i++)
		{
			if (!bFullyConstructedOnly || Hives[i].Status == HIVE_STATUS_BUILT)
			{
				float ThisDist = (bUsePhaseGates) ? UTIL_GetPhaseDistanceBetweenPointsSq(Hives[i].Location, Location) : vDist2DSq(Hives[i].Location, Location);

				if (ThisDist < MaxDist && (!NearestHive || ThisDist < MinDist))
				{
					NearestHive = Hives[i].edict;
					MinDist = ThisDist;
				}
			}
		}

		return NearestHive;
	}

	bool bIsMarineStructure = UTIL_IsMarineStructure(StructureType);

	edict_t* Result = nullptr;
	float MinDist = 0.0f;
	float SearchDistSq = sqrf(SearchDist);

	if (bIsMarineStructure)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (bFullyConstructedOnly && !it.second.bFullyConstructed) { continue; }

			float thisDist = (bUsePhaseGates) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(Location, it.second.Location);

			if (thisDist < SearchDistSq && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }
			if (bFullyConstructedOnly && !it.second.bFullyConstructed) { continue; }

			float thisDist = (bUsePhaseGates) ? UTIL_GetPhaseDistanceBetweenPointsSq(it.second.Location, Location) : vDist2DSq(Location, it.second.Location);

			if (thisDist < SearchDistSq && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_GetClosestPlayerOnTeamWithLOS(const Vector& Location, const int Team, float SearchRadius, edict_t* IgnorePlayer)
{
	float distSq = sqrf(SearchRadius);
	float MinDist = 0.0f;
	edict_t* Result = nullptr;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != IgnorePlayer && clients[i]->v.team == Team && IsPlayerActiveInGame(clients[i]))
		{
			float ThisDist = vDist2DSq(clients[i]->v.origin, Location);

			if (ThisDist <= distSq && UTIL_PointIsDirectlyReachable(clients[i]->v.origin, Location))
			{
				if (FNullEnt(Result) || ThisDist < MinDist)
				{
					Result = clients[i];
					MinDist = ThisDist;
				}

			}
		}
	}

	return Result;
}

bool UTIL_AnyPlayerOnTeamWithLOS(const Vector& Location, const int Team, float SearchRadius)
{
	float distSq = sqrf(SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i]->v.team == Team && IsPlayerActiveInGame(clients[i]))
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= distSq && UTIL_PointIsDirectlyReachable(clients[i]->v.origin, Location))
			{
				return true;
			}
		}
	}

	return false;
}

int UTIL_FindClosestMarinePlayerToLocation(const edict_t* SearchingPlayer, const Vector& Location, const float SearchRadius)
{
	int nearestPlayer = -1;
	float nearestDist = 0.0f;
	const float maxDist = sqrf(SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] != NULL && clients[i] != SearchingPlayer && IsPlayerOnMarineTeam(clients[i]) && !IsPlayerCommander(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]))
		{
			float playerDist = vDist2DSq(clients[i]->v.origin, Location);

			if (playerDist < maxDist && (nearestPlayer < 0 || playerDist < nearestDist))
			{
				nearestPlayer = i;
				nearestDist = playerDist;
			}
		}
	}

	return nearestPlayer;
}

edict_t* UTIL_FindClosestMarineStructureToLocation(const Vector& Location, const float SearchRadius, bool bAllowElectrified)
{
	edict_t* Result = nullptr;
	float MinDist = 0.0f;
	float MaxDistSq = sqrf(SearchRadius);

	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (!bAllowElectrified && it.second.bIsElectrified) { continue; }

		float thisDist = vDist2DSq(Location, it.second.Location);

		if (thisDist < MaxDistSq && (!Result || thisDist < MinDist))
		{
			Result = it.second.edict;
			MinDist = thisDist;
		}
	}

	return Result;
}

bool UTIL_AnyMarinePlayerNearLocation(const Vector& Location, float SearchRadius)
{
	float distSq = (SearchRadius * SearchRadius);

	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerOnMarineTeam(clients[i]) && !IsPlayerCommander(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]))
		{
			if (vDist2DSq(clients[i]->v.origin, Location) <= distSq)
			{
				return true;
			}
		}
	}

	return false;
}

edict_t* UTIL_FindClosestMarineStructureOfTypeUnbuilt(const NSStructureType StructureType, const Vector& SearchLocation, float SearchRadius, bool bUsePhaseDistance)
{
	edict_t* NearestStructure = NULL;
	float nearestDist = 0.0f;

	float SearchDistSq = sqrf(SearchRadius);

	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (it.second.bFullyConstructed) { continue; }
		if (!UTIL_StructureTypesMatch(StructureType, it.second.StructureType)) { continue; }

		float thisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, it.second.Location) : vDist2DSq(SearchLocation, it.second.Location);

		if (thisDist < SearchDistSq && (!NearestStructure || thisDist < nearestDist))
		{
			NearestStructure = it.second.edict;
			nearestDist = thisDist;
		}
	}

	return NearestStructure;
}

edict_t* UTIL_FindClosestMarineStructureUnbuilt(const Vector& SearchLocation, float SearchRadius, bool bUsePhaseDistance)
{
	edict_t* NearestStructure = NULL;
	float nearestDist = 0.0f;

	float SearchDistSq = sqrf(SearchRadius);

	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (it.second.bFullyConstructed) { continue; }

		float thisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, it.second.Location) : vDist2DSq(SearchLocation, it.second.Location);

		if (thisDist < SearchDistSq && (!NearestStructure || thisDist < nearestDist))
		{
			NearestStructure = it.second.edict;
			nearestDist = thisDist;
		}
	}

	return NearestStructure;
}

edict_t* UTIL_FindClosestMarineStructureUnbuiltWithoutBuilders(bot_t* pBot, const int MaxBuilders, const Vector& SearchLocation, float SearchRadius, bool bUsePhaseDistance)
{
	edict_t* NearestStructure = NULL;
	float nearestDist = 0.0f;

	float SearchDistSq = sqrf(SearchRadius);

	Vector CommChairLocation = UTIL_GetCommChairLocation();

	for (auto& it : MarineBuildableStructureMap)
	{
		if (!it.second.bOnNavmesh) { continue; }
		if (it.second.bFullyConstructed) { continue; }
		bool bReachable = (IsPlayerOnMarineTeam(pBot->pEdict)) ? it.second.bIsReachableMarine : it.second.bIsReachableAlien;

		if (!bReachable) { continue; }

		float thisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, it.second.Location) : vDist2DSq(SearchLocation, it.second.Location);

		if (thisDist > SearchDistSq) { continue; }

		float ActualDistance = vDist2D(SearchLocation, it.second.Location);

		int NumBuilders = UTIL_GetNumPlayersOfTeamInArea(it.second.Location, ActualDistance - 1.0f, MARINE_TEAM, pBot->pEdict, CLASS_NONE, false);

		// If we're building at base, only one builder is needed. Don't need someone to guard them
		int MaxDesiredBuilders = (vDist2DSq(it.second.Location, CommChairLocation) < sqrf(UTIL_MetresToGoldSrcUnits(15.0f))) ? 1 : MaxBuilders;

		if (NumBuilders >= MaxDesiredBuilders) { continue; }

		if (!NearestStructure || thisDist < nearestDist)
		{
			NearestStructure = it.second.edict;
			nearestDist = thisDist;
		}
	}

	return NearestStructure;
}

edict_t* UTIL_FindClosestDamagedStructure(const Vector& SearchLocation, const int Team, float SearchRadius, bool bUsePhaseDistance)
{
	edict_t* Result = nullptr;
	float MaxDist = sqrf(SearchRadius);
	float MinDist = 0.0f;

	if (Team == MARINE_TEAM)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!it.second.bFullyConstructed) { continue; }

			if (it.second.healthPercent >= 1.0f) { continue; }

			float thisDist = (bUsePhaseDistance) ? UTIL_GetPhaseDistanceBetweenPointsSq(SearchLocation, it.second.Location) : vDist2DSq(SearchLocation, it.second.Location);

			if (thisDist < MaxDist && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!it.second.bFullyConstructed) { continue; }

			if (it.second.healthPercent >= 1.0f) { continue; }

			float thisDist = vDist2DSq(SearchLocation, it.second.Location);

			if (thisDist < MaxDist && (!Result || thisDist < MinDist))
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}
		}
	}

	return Result;
}

edict_t* UTIL_FindMarineWithDamagedArmour(const Vector& SearchLocation, float SearchRadius, edict_t* IgnoreEdict)
{
	edict_t* Result = nullptr;
	float MaxDist = sqrf(SearchRadius);
	float MinDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && clients[i] != IgnoreEdict && IsPlayerMarine(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]))
		{
			if (clients[i]->v.armorvalue < GetPlayerMaxArmour(clients[i]))
			{
				float ThisDist = vDist2DSq(SearchLocation, clients[i]->v.origin);

				if (ThisDist < MaxDist && (!Result || ThisDist < MinDist))
				{
					Result = clients[i];
					MinDist = ThisDist;
				}

			}
		}
	}

	return Result;
}

int UTIL_GetNumUnbuiltHives()
{
	int Result = 0;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid && Hives[i].Status == HIVE_STATUS_UNBUILT) { Result++; }
	}

	return Result;
}

void UTIL_RefreshMarineItems()
{
	edict_t* currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "item_health")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_HEALTHPACK);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "item_genericammo")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_AMMO);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "item_heavyarmor")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_HEAVYARMOUR);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "item_jetpack")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_JETPACK);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "item_catalyst")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_CATALYSTS);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "weapon_mine")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_MINES);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "weapon_shotgun")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_SHOTGUN);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "weapon_heavymachinegun")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_HMG);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "weapon_grenadegun")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_GRENADELAUNCHER);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "weapon_welder")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_WELDER);
	}

	currItem = NULL;
	while (((currItem = UTIL_FindEntityByClassname(currItem, "scan")) != NULL) && (!FNullEnt(currItem)) && !(currItem->v.effects & EF_NODRAW))
	{
		UTIL_UpdateMarineItem(currItem, ITEM_MARINE_SCAN);
	}

	for (auto it = MarineDroppedItemMap.begin(); it != MarineDroppedItemMap.end();)
	{
		if (it->second.LastSeen < ItemRefreshFrame)
		{
			it = MarineDroppedItemMap.erase(it);
		}
		else
		{
			it++;
		}
	}

	ItemRefreshFrame++;

}

NSDeployableItem UTIL_GetItemTypeFromEdict(const edict_t* ItemEdict)
{
	if (!ItemEdict) { return ITEM_NONE; }

	for (auto& it : MarineDroppedItemMap)
	{
		if (it.second.edict == ItemEdict)
		{
			return it.second.ItemType;
		}
	}

	return ITEM_NONE;
}

bool IsAlienTraitCategoryAvailable(HiveTechStatus TraitCategory)
{
	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].TechStatus == TraitCategory) { return true; }
	}

	return false;
}

unsigned char UTIL_GetAreaForObstruction(NSStructureType StructureType)
{
	if (StructureType == STRUCTURE_NONE) { return DT_TILECACHE_NULL_AREA; }

	switch (StructureType)
	{
	case STRUCTURE_MARINE_RESTOWER:
	case STRUCTURE_MARINE_COMMCHAIR:
	case STRUCTURE_MARINE_ARMOURY:
	case STRUCTURE_MARINE_ADVARMOURY:
	case STRUCTURE_MARINE_OBSERVATORY:
		return DT_TILECACHE_MSTRUCTURE_AREA;
	case STRUCTURE_ALIEN_RESTOWER:
	case STRUCTURE_ALIEN_HIVE:
		return DT_TILECACHE_ASTRUCTURE_AREA;
	default:
		return DT_TILECACHE_BLOCKED_AREA;
	}

	return DT_TILECACHE_BLOCKED_AREA;
}

float UTIL_GetStructureRadiusForObstruction(NSStructureType StructureType)
{
	if (StructureType == STRUCTURE_NONE) { return 0.0f; }

	switch (StructureType)
	{
	case STRUCTURE_MARINE_TURRETFACTORY:
	case STRUCTURE_MARINE_COMMCHAIR:
		return 60.0f;
	default:
		return 40.0f;

	}

	return 40.0f;
}

bool UTIL_ShouldStructureCollide(NSStructureType StructureType)
{
	if (StructureType == STRUCTURE_NONE) { return false; }

	switch (StructureType)
	{
	case STRUCTURE_MARINE_INFANTRYPORTAL:
	case STRUCTURE_MARINE_PHASEGATE:
	case STRUCTURE_MARINE_TURRET:
		return false;
	default:
		return true;

	}

	return true;
}

void UTIL_OnItemDropped(const dropped_marine_item* NewItem)
{
	for (int i = 0; i < 32; i++)
	{
		if (clients[i] && IsPlayerBot(clients[i]) && IsPlayerCommander(clients[i]))
		{
			bot_t* BotRef = GetBotPointer(clients[i]);

			if (BotRef)
			{
				UTIL_LinkDroppedItemToAction(BotRef, NewItem);
			}
		}
	}
}

void UTIL_UpdateMarineItem(edict_t* Item, NSDeployableItem ItemType)
{
	if (FNullEnt(Item)) { return; }

	int EntIndex = ENTINDEX(Item);
	if (EntIndex < 0) { return; }

	MarineDroppedItemMap[EntIndex].edict = Item;

	if (MarineDroppedItemMap[EntIndex].LastSeen == 0 || !vEquals(Item->v.origin, MarineDroppedItemMap[EntIndex].Location, 5.0f))
	{
		if (ItemType == ITEM_MARINE_SCAN)
		{
			MarineDroppedItemMap[EntIndex].bOnNavMesh = true;
			MarineDroppedItemMap[EntIndex].bIsReachableMarine = true;
		}
		else
		{
			MarineDroppedItemMap[EntIndex].bOnNavMesh = UTIL_PointIsOnNavmesh(MARINE_REGULAR_NAV_PROFILE, Item->v.origin, Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach));

			if (MarineDroppedItemMap[EntIndex].bOnNavMesh)
			{
				MarineDroppedItemMap[EntIndex].bIsReachableMarine = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), Item->v.origin, max_player_use_reach);
			}
		}
	}

	MarineDroppedItemMap[EntIndex].Location = Item->v.origin;
	MarineDroppedItemMap[EntIndex].ItemType = ItemType;

	if (MarineDroppedItemMap[EntIndex].LastSeen == 0)
	{
		UTIL_OnItemDropped(&MarineDroppedItemMap[EntIndex]);
	}

	MarineDroppedItemMap[EntIndex].LastSeen = ItemRefreshFrame;
}

void UTIL_UpdateBuildableStructure(edict_t* Structure)
{
	if (FNullEnt(Structure)) { return; }

	NSStructureType StructureType = UTIL_IUSER3ToStructureType(Structure->v.iuser3);

	if (StructureType == STRUCTURE_NONE) { return; }

	bool bShouldCollide = UTIL_ShouldStructureCollide(StructureType);

	int EntIndex = ENTINDEX(Structure);
	if (EntIndex < 0) { return; }

	if (UTIL_IsMarineStructure(StructureType))
	{
		MarineBuildableStructureMap[EntIndex].edict = Structure;

		if (!vEquals(Structure->v.origin, MarineBuildableStructureMap[EntIndex].Location, 5.0f))
		{
			MarineBuildableStructureMap[EntIndex].bOnNavmesh = UTIL_PointIsOnNavmesh(MARINE_REGULAR_NAV_PROFILE, UTIL_GetEntityGroundLocation(Structure), Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach));
			if (MarineBuildableStructureMap[EntIndex].bOnNavmesh)
			{
				MarineBuildableStructureMap[EntIndex].bIsReachableMarine = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
				MarineBuildableStructureMap[EntIndex].bIsReachableAlien = UTIL_PointIsReachable(SKULK_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
			}
			else
			{
				MarineBuildableStructureMap[EntIndex].bIsReachableMarine = false;
				MarineBuildableStructureMap[EntIndex].bIsReachableAlien = false;
			}
		}

		MarineBuildableStructureMap[EntIndex].Location = Structure->v.origin;

		MarineBuildableStructureMap[EntIndex].bFullyConstructed = !(Structure->v.iuser4 & MASK_BUILDABLE);
		MarineBuildableStructureMap[EntIndex].bIsParasited = (Structure->v.iuser4 & MASK_PARASITED);
		MarineBuildableStructureMap[EntIndex].bIsElectrified = UTIL_IsStructureElectrified(Structure);
		MarineBuildableStructureMap[EntIndex].bDead = (Structure->v.deadflag != DEAD_NO);
		MarineBuildableStructureMap[EntIndex].StructureType = UTIL_IUSER3ToStructureType(Structure->v.iuser3);

		if (MarineBuildableStructureMap[EntIndex].LastSeen == 0)
		{
			MarineBuildableStructureMap[EntIndex].healthPercent = (Structure->v.health / Structure->v.max_health);
			MarineBuildableStructureMap[EntIndex].lastDamagedTime = 0.0f;

			if (bShouldCollide)
			{
				unsigned int area = UTIL_GetAreaForObstruction(StructureType);
				float Radius = UTIL_GetStructureRadiusForObstruction(StructureType);
				MarineBuildableStructureMap[EntIndex].ObstacleRef = UTIL_AddTemporaryObstacle(UTIL_GetCentreOfEntity(MarineBuildableStructureMap[EntIndex].edict), Radius, 100.0f, area);
			}
			else
			{
				MarineBuildableStructureMap[EntIndex].ObstacleRef = 0;
			}

			UTIL_OnStructureCreated(&MarineBuildableStructureMap[EntIndex]);

			MarineBuildableStructureMap[EntIndex].bOnNavmesh = UTIL_PointIsOnNavmesh(MARINE_REGULAR_NAV_PROFILE, UTIL_GetEntityGroundLocation(Structure), Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach));
			if (MarineBuildableStructureMap[EntIndex].bOnNavmesh)
			{
				MarineBuildableStructureMap[EntIndex].bIsReachableMarine = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
				MarineBuildableStructureMap[EntIndex].bIsReachableAlien = UTIL_PointIsReachable(SKULK_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
			}
			else
			{
				MarineBuildableStructureMap[EntIndex].bIsReachableMarine = false;
				MarineBuildableStructureMap[EntIndex].bIsReachableAlien = false;
			}
		}
		else
		{
			float NewHealthPercent = (Structure->v.health / Structure->v.max_health);

			if (NewHealthPercent < MarineBuildableStructureMap[EntIndex].healthPercent)
			{
				MarineBuildableStructureMap[EntIndex].lastDamagedTime = gpGlobals->time;
			}
			MarineBuildableStructureMap[EntIndex].healthPercent = NewHealthPercent;
		}

		if (!MarineBuildableStructureMap[EntIndex].bDead)
		{
			MarineBuildableStructureMap[EntIndex].LastSeen = StructureRefreshFrame;
			MarineBuildableStructureMap[EntIndex].bUnderAttack = (gpGlobals->time - MarineBuildableStructureMap[EntIndex].lastDamagedTime) < 10.0f;
		}

	}
	else
	{
		AlienBuildableStructureMap[EntIndex].edict = Structure;

		if (Structure->v.origin != AlienBuildableStructureMap[EntIndex].Location)
		{
			AlienBuildableStructureMap[EntIndex].bOnNavmesh = UTIL_PointIsOnNavmesh(MARINE_REGULAR_NAV_PROFILE, UTIL_GetEntityGroundLocation(Structure), Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach));
			if (AlienBuildableStructureMap[EntIndex].bOnNavmesh)
			{
				AlienBuildableStructureMap[EntIndex].bIsReachableMarine = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
				AlienBuildableStructureMap[EntIndex].bIsReachableAlien = UTIL_PointIsReachable(SKULK_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
			}
		}

		AlienBuildableStructureMap[EntIndex].Location = Structure->v.origin;

		AlienBuildableStructureMap[EntIndex].bFullyConstructed = !(Structure->v.iuser4 & MASK_BUILDABLE);
		AlienBuildableStructureMap[EntIndex].bIsParasited = (Structure->v.iuser4 & MASK_PARASITED);
		AlienBuildableStructureMap[EntIndex].bIsElectrified = UTIL_IsStructureElectrified(Structure);
		AlienBuildableStructureMap[EntIndex].bDead = (Structure->v.deadflag != DEAD_NO);
		AlienBuildableStructureMap[EntIndex].StructureType = UTIL_IUSER3ToStructureType(Structure->v.iuser3);

		if (AlienBuildableStructureMap[EntIndex].LastSeen == 0)
		{
			AlienBuildableStructureMap[EntIndex].healthPercent = (Structure->v.health / Structure->v.max_health);
			AlienBuildableStructureMap[EntIndex].lastDamagedTime = 0.0f;

			if (bShouldCollide)
			{
				unsigned int area = UTIL_GetAreaForObstruction(StructureType);
				float Radius = UTIL_GetStructureRadiusForObstruction(StructureType);
				AlienBuildableStructureMap[EntIndex].ObstacleRef = UTIL_AddTemporaryObstacle(UTIL_GetCentreOfEntity(AlienBuildableStructureMap[EntIndex].edict), Radius, 100.0f, area);
			}
			else
			{
				AlienBuildableStructureMap[EntIndex].ObstacleRef = 0;
			}

			UTIL_OnStructureCreated(&AlienBuildableStructureMap[EntIndex]);

			AlienBuildableStructureMap[EntIndex].bOnNavmesh = UTIL_PointIsOnNavmesh(MARINE_REGULAR_NAV_PROFILE, UTIL_GetEntityGroundLocation(Structure), Vector(max_player_use_reach, max_player_use_reach, max_player_use_reach));
			if (AlienBuildableStructureMap[EntIndex].bOnNavmesh)
			{
				AlienBuildableStructureMap[EntIndex].bIsReachableMarine = UTIL_PointIsReachable(MARINE_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
				AlienBuildableStructureMap[EntIndex].bIsReachableAlien = UTIL_PointIsReachable(SKULK_REGULAR_NAV_PROFILE, UTIL_GetCommChairLocation(), UTIL_GetEntityGroundLocation(Structure), max_player_use_reach);
			}
			else
			{
				AlienBuildableStructureMap[EntIndex].bIsReachableMarine = false;
				AlienBuildableStructureMap[EntIndex].bIsReachableAlien = false;
			}
		}
		else
		{
			float NewHealthPercent = (Structure->v.health / Structure->v.max_health);

			if (NewHealthPercent < AlienBuildableStructureMap[EntIndex].healthPercent)
			{
				AlienBuildableStructureMap[EntIndex].lastDamagedTime = gpGlobals->time;
			}
			AlienBuildableStructureMap[EntIndex].healthPercent = NewHealthPercent;
		}

		if (!AlienBuildableStructureMap[EntIndex].bDead)
		{
			AlienBuildableStructureMap[EntIndex].LastSeen = StructureRefreshFrame;
			AlienBuildableStructureMap[EntIndex].bUnderAttack = (gpGlobals->time - AlienBuildableStructureMap[EntIndex].lastDamagedTime) < 10.0f;
		}
	}
}

NSWeapon UTIL_GetWeaponTypeFromEdict(const edict_t* ItemEdict)
{
	if (!ItemEdict) { return WEAPON_NONE; }

	for (auto& it : MarineDroppedItemMap)
	{
		if (it.second.edict == ItemEdict)
		{
			switch (it.second.ItemType)
			{
			case ITEM_MARINE_WELDER:
				return WEAPON_MARINE_WELDER;
			case ITEM_MARINE_HMG:
				return WEAPON_MARINE_HMG;
			case ITEM_MARINE_GRENADELAUNCHER:
				return WEAPON_MARINE_GL;
			case ITEM_MARINE_SHOTGUN:
				return WEAPON_MARINE_SHOTGUN;
			case ITEM_MARINE_MINES:
				return WEAPON_MARINE_MINES;
			default:
				return WEAPON_NONE;
			}
		}
	}

	return WEAPON_NONE;
}

const hive_definition* UTIL_GetHiveAtIndex(int Index)
{
	if (Index > -1 && Index < NumTotalHives)
	{
		return &Hives[Index];
	}

	return nullptr;
}

int UTIL_GetNumTotalHives()
{
	return NumTotalHives;
}

int UTIL_GetNumActiveHives()
{
	int Result = 0;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].Status == HIVE_STATUS_BUILT) { Result++; }
	}

	return Result;
}

const hive_definition* UTIL_GetNearestHiveAtLocation(const Vector Location)
{
	int Result = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < NumTotalHives; i++)
	{
		if (Hives[i].bIsValid)
		{
			float ThisDist = vDist2DSq(Location, Hives[i].Location);

			if (Result < 0 || ThisDist < MinDist)
			{
				Result = i;
				MinDist = ThisDist;
			}
		}
	}

	if (Result > -1)
	{
		return &Hives[Result];
	}

	return nullptr;
}

edict_t* UTIL_AlienFindNearestHealingSpot(bot_t* pBot, const Vector SearchLocation)
{
	edict_t* HealingSources[3];

	HealingSources[0] = UTIL_GetNearestStructureIndexOfType(SearchLocation, STRUCTURE_ALIEN_HIVE, UTIL_MetresToGoldSrcUnits(100.0f), true, IsPlayerMarine(pBot->pEdict));
	HealingSources[1] = UTIL_GetNearestStructureIndexOfType(SearchLocation, STRUCTURE_ALIEN_DEFENCECHAMBER, UTIL_MetresToGoldSrcUnits(100.0f), true, IsPlayerMarine(pBot->pEdict));
	HealingSources[2] = UTIL_GetNearestPlayerOfClass(SearchLocation, CLASS_GORGE, UTIL_MetresToGoldSrcUnits(100.0f), pBot->pEdict);

	int NearestHealingSource = -1;
	float MinDist = 0.0f;

	for (int i = 0; i < 3; i++)
	{
		if (!FNullEnt(HealingSources[i]))
		{
			float ThisDist = vDist2DSq(HealingSources[i]->v.origin, SearchLocation);

			if (NearestHealingSource < 0 || ThisDist < MinDist)
			{
				NearestHealingSource = i;
				MinDist = ThisDist;
			}
		}
	}

	if (NearestHealingSource > -1)
	{
		return HealingSources[NearestHealingSource];
	}

	return nullptr;
}

edict_t* PlayerGetNearestDangerTurret(const edict_t* Player, float MaxDistance)
{
	edict_t* Result = nullptr;
	float MinDist = 0;
	float MaxDist = sqrf(MaxDistance);

	Vector Location = Player->v.origin;

	if (IsPlayerOnAlienTeam(Player))
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_ANYTURRET)) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist)
			{
				if (!UTIL_QuickTrace(Player, Location, it.second.Location)) { continue; }

				if (!Result || thisDist < MinDist)
				{
					Result = it.second.edict;
					MinDist = thisDist;
				}
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (it.second.StructureType != STRUCTURE_ALIEN_OFFENCECHAMBER) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist)
			{
				if (!UTIL_QuickTrace(Player, Location, it.second.Location)) { continue; }

				if (!Result || thisDist < MinDist)
				{
					Result = it.second.edict;
					MinDist = thisDist;
				}
			}
		}
	}

	return Result;
}

bool UTIL_AnyTurretWithLOSToLocation(const Vector Location, const int TurretTeam)
{
	if (TurretTeam == MARINE_TEAM)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_TURRET)) { continue; }

			if (!UTIL_QuickTrace(it.second.edict, it.second.edict->v.origin, Location)) { continue; }

			if (vDist2DSq(Location, it.second.edict->v.origin) < UTIL_MetresToGoldSrcUnits(15.0f)) { return true; }
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_ALIEN_OFFENCECHAMBER)) { continue; }

			if (!UTIL_QuickTrace(it.second.edict, it.second.edict->v.origin, Location)) { continue; }

			if (vDist2DSq(Location, it.second.edict->v.origin) < UTIL_MetresToGoldSrcUnits(15.0f)) { return true; }
		}
	}

	return false;
}

edict_t* BotGetNearestDangerTurret(bot_t* pBot, float MaxDistance)
{
	edict_t* Result = nullptr;
	float MinDist = 0;
	float MaxDist = sqrf(MaxDistance);

	Vector Location = pBot->pEdict->v.origin;

	if (IsPlayerOnAlienTeam(pBot->pEdict))
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_TURRET)) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist)
			{
				if (!UTIL_QuickTrace(pBot->pEdict, pBot->CurrentEyePosition, UTIL_GetCentreOfEntity(it.second.edict))) { continue; }

				if (!Result || thisDist < MinDist)
				{
					Result = it.second.edict;
					MinDist = thisDist;
				}
			}
		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }

			if (it.second.StructureType != STRUCTURE_ALIEN_OFFENCECHAMBER) { continue; }

			float thisDist = vDist2DSq(Location, it.second.Location);

			if (thisDist < MaxDist)
			{
				if (!UTIL_QuickTrace(pBot->pEdict, pBot->CurrentEyePosition, UTIL_GetCentreOfEntity(it.second.edict))) { continue; }

				if (FNullEnt(Result) || thisDist < MinDist)
				{
					Result = it.second.edict;
					MinDist = thisDist;
				}
			}
		}
	}

	return Result;
}

bool UTIL_DroppedItemIsPrimaryWeapon(NSDeployableItem ItemType)
{
	switch (ItemType)
	{
	case ITEM_MARINE_GRENADELAUNCHER:
	case ITEM_MARINE_HMG:
	case ITEM_MARINE_SHOTGUN:
		return true;
	default:
		return false;
	}

	return false;
}


bool UTIL_IsMarineStructure(const edict_t* Structure)
{
	if (FNullEnt(Structure)) { return false; }

	NSStructureType StructureType = UTIL_IUSER3ToStructureType(Structure->v.iuser3);
	return UTIL_IsMarineStructure(StructureType);
}

bool UTIL_IsAlienStructure(const edict_t* Structure)
{
	if (!Structure) { return false; }

	NSStructureType StructureType = UTIL_IUSER3ToStructureType(Structure->v.iuser3);

	return UTIL_IsAlienStructure(StructureType);
}

bool UTIL_IsMarineStructure(const NSStructureType StructureType)
{
	switch (StructureType)
	{
	case STRUCTURE_MARINE_ARMOURY:
	case STRUCTURE_MARINE_ADVARMOURY:
	case STRUCTURE_MARINE_ANYARMOURY:
	case STRUCTURE_MARINE_TURRETFACTORY:
	case STRUCTURE_MARINE_ADVTURRETFACTORY:
	case STRUCTURE_MARINE_ANYTURRETFACTORY:
	case STRUCTURE_MARINE_ARMSLAB:
	case STRUCTURE_MARINE_COMMCHAIR:
	case STRUCTURE_MARINE_INFANTRYPORTAL:
	case STRUCTURE_MARINE_OBSERVATORY:
	case STRUCTURE_MARINE_PHASEGATE:
	case STRUCTURE_MARINE_PROTOTYPELAB:
	case STRUCTURE_MARINE_RESTOWER:
	case STRUCTURE_MARINE_SIEGETURRET:
	case STRUCTURE_MARINE_TURRET:
	case STRUCTURE_MARINE_ANYTURRET:
	case STRUCTURE_ANY_MARINE_STRUCTURE:
		return true;
	default:
		return false;
	}
}

bool UTIL_IsAlienStructure(const NSStructureType StructureType)
{
	switch (StructureType)
	{
	case STRUCTURE_ALIEN_HIVE:
	case STRUCTURE_ALIEN_DEFENCECHAMBER:
	case STRUCTURE_ALIEN_MOVEMENTCHAMBER:
	case STRUCTURE_ALIEN_OFFENCECHAMBER:
	case STRUCTURE_ALIEN_SENSORYCHAMBER:
	case STRUCTURE_ALIEN_RESTOWER:
	case STRUCTURE_ANY_ALIEN_STRUCTURE:
		return true;
	default:
		return false;
	}
}

void UTIL_LinkAlienStructureToTask(bot_t* pBot, edict_t* NewStructure)
{
	if (!NewStructure) { return; }

	NSStructureType StructureType = GetStructureTypeFromEdict(NewStructure);

	if (StructureType == STRUCTURE_NONE) { return; }

	if ((pBot->PrimaryBotTask.TaskType == TASK_BUILD || pBot->PrimaryBotTask.TaskType == TASK_CAP_RESNODE) && pBot->PrimaryBotTask.bIsWaitingForBuildLink)
	{
		if (pBot->PrimaryBotTask.StructureType == StructureType)
		{

			if (vDist2DSq(NewStructure->v.origin, pBot->PrimaryBotTask.TaskLocation) < sqrf(UTIL_MetresToGoldSrcUnits(2.0f)))
			{
				pBot->PrimaryBotTask.TaskTarget = NewStructure;
				pBot->PrimaryBotTask.bIsWaitingForBuildLink = false;
			}
		}
	}

	if ((pBot->SecondaryBotTask.TaskType == TASK_BUILD || pBot->SecondaryBotTask.TaskType == TASK_CAP_RESNODE) && pBot->SecondaryBotTask.bIsWaitingForBuildLink)
	{
		if (pBot->SecondaryBotTask.StructureType == StructureType)
		{

			if (vDist2DSq(NewStructure->v.origin, pBot->SecondaryBotTask.TaskLocation) < sqrf(UTIL_MetresToGoldSrcUnits(2.0f)))
			{
				pBot->SecondaryBotTask.TaskTarget = NewStructure;
				pBot->SecondaryBotTask.bIsWaitingForBuildLink = false;
			}
		}
	}
}

int UTIL_GetNumWeaponsOfTypeInPlay(const NSWeapon WeaponType)
{
	int NumPlacedWeapons = UTIL_GetItemCountOfTypeInArea(UTIL_WeaponTypeToDeployableItem(WeaponType), UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(30.0f));
	int NumHeldWeapons = 0;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && IsPlayerOnMarineTeam(clients[i]) && IsPlayerActiveInGame(clients[i]))
		{
			if (PlayerHasWeapon(clients[i], WeaponType))
			{
				NumHeldWeapons++;
			}
		}
	}


	return NumPlacedWeapons + NumHeldWeapons;
}

int UTIL_GetNumEquipmentInPlay()
{

	int NumPlacedEquipment = UTIL_GetItemCountOfTypeInArea(ITEM_MARINE_HEAVYARMOUR, UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(30.0f));
	NumPlacedEquipment += UTIL_GetItemCountOfTypeInArea(ITEM_MARINE_JETPACK, UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(30.0f));
	int NumUsedEquipment = 0;

	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && IsPlayerOnMarineTeam(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]) && !IsPlayerCommander(clients[i]))
		{
			if (PlayerHasEquipment(clients[i]))
			{
				NumUsedEquipment++;
			}
		}
	}


	return NumPlacedEquipment + NumUsedEquipment;
}

bool UTIL_BaseIsInDistress()
{
	int NumDefenders = UTIL_GetNumPlayersOfTeamInArea(UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(15.0f), MARINE_TEAM, nullptr, CLASS_NONE, true);
	int NumMarines = GAME_GetNumPlayersOnTeam(MARINE_TEAM);

	float MarineRatio = ((float)NumDefenders / (float)(NumMarines - 1));

	if (MarineRatio >= 0.3f) { return false; }

	int NumInfantryPortals = UTIL_GetNumBuiltStructuresOfType(STRUCTURE_MARINE_INFANTRYPORTAL);

	int NumOnos = UTIL_GetNumPlayersOfTeamInArea(UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(10.0f), ALIEN_TEAM, nullptr, CLASS_ONOS, false);
	int NumFades = UTIL_GetNumPlayersOfTeamInArea(UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(10.0f), ALIEN_TEAM, nullptr, CLASS_FADE, false);
	int NumSkulks = UTIL_GetNumPlayersOfTeamInArea(UTIL_GetCommChairLocation(), UTIL_MetresToGoldSrcUnits(10.0f), ALIEN_TEAM, nullptr, CLASS_SKULK, false);

	float MarineForce = (float)NumDefenders * 1.0f;
	float AlienForce = ((float)NumSkulks * 1.0f) + ((NumFades + NumOnos) * 1.5f);

	return ((NumInfantryPortals == 0 || AlienForce > 2.0f) && AlienForce > (MarineForce * 2.0f));
}

bool UTIL_ResearchIsComplete(const NSResearch Research)
{
	bool bIsComplete = false;

	switch (Research)
	{
	case RESEARCH_ARMOURY_GRENADES:
	{
		AvHUpgradeMask Mask = UTIL_GetResearchMask(Research);

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_ANYARMOURY)) { continue; }
			if (UTIL_StructureIsRecycling(it.second.edict)) { continue; }
			if (!it.second.bFullyConstructed) { continue; }

			if (!(it.second.edict->v.iuser4 & Mask) && (!UTIL_StructureIsResearching(it.second.edict) || it.second.edict->v.iuser2 != Research))
			{
				return true;
			}

		}

		return false;
	}
	break;
	case RESEARCH_ARMSLAB_ARMOUR1:
	case RESEARCH_ARMSLAB_ARMOUR2:
	case RESEARCH_ARMSLAB_ARMOUR3:
	case RESEARCH_ARMSLAB_WEAPONS1:
	case RESEARCH_ARMSLAB_WEAPONS2:
	case RESEARCH_ARMSLAB_WEAPONS3:
	case RESEARCH_ARMSLAB_CATALYSTS:
	{
		AvHUpgradeMask Mask = UTIL_GetResearchMask(Research);

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_ARMSLAB)) { continue; }
			if (UTIL_StructureIsRecycling(it.second.edict)) { continue; }
			if (!it.second.bFullyConstructed) { continue; }

			if (!(it.second.edict->v.iuser4 & Mask) && (!UTIL_StructureIsResearching(it.second.edict) || it.second.edict->v.iuser2 != Research))
			{
				return true;
			}

		}

		return false;
	}
	break;
	case RESEARCH_OBSERVATORY_DISTRESSBEACON:
	case RESEARCH_OBSERVATORY_MOTIONTRACKING:
	case RESEARCH_OBSERVATORY_PHASETECH:
	{
		AvHUpgradeMask Mask = UTIL_GetResearchMask(Research);

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_OBSERVATORY)) { continue; }
			if (UTIL_StructureIsRecycling(it.second.edict)) { continue; }
			if (!it.second.bFullyConstructed) { continue; }

			if (!(it.second.edict->v.iuser4 & Mask) && (!UTIL_StructureIsResearching(it.second.edict) || it.second.edict->v.iuser2 != Research))
			{
				return true;
			}

		}

		return false;
	}
	break;
	case RESEARCH_PROTOTYPELAB_HEAVYARMOUR:
	case RESEARCH_PROTOTYPELAB_JETPACKS:
	{
		AvHUpgradeMask Mask = UTIL_GetResearchMask(Research);

		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (!UTIL_StructureTypesMatch(it.second.StructureType, STRUCTURE_MARINE_PROTOTYPELAB)) { continue; }
			if (UTIL_StructureIsRecycling(it.second.edict)) { continue; }
			if (!it.second.bFullyConstructed) { continue; }

			if (!(it.second.edict->v.iuser4 & Mask) && (!UTIL_StructureIsResearching(it.second.edict) || it.second.edict->v.iuser2 != Research))
			{
				return true;
			}

		}

		return false;
	}
	break;
	default:
		return false;
	}

	return false;
}

float UTIL_DistToNearestFriendlyPlayer(const Vector& Location, int DesiredTeam)
{
	float smallestDist = 0.0f;

	for (int i = 0; i < 32; i++)
	{

		if (!FNullEnt(clients[i]) && clients[i]->v.team == DesiredTeam && !IsPlayerCommander(clients[i]) && !IsPlayerDead(clients[i]) && !IsPlayerBeingDigested(clients[i]))
		{
			float newDist = vDist2DSq(clients[i]->v.origin, Location);
			if (smallestDist == 0.0f || newDist < smallestDist)
			{
				smallestDist = newDist;
			}
		}
	}

	return sqrtf(smallestDist);
}

float UTIL_GetPhaseDistanceBetweenPoints(const Vector StartPoint, const Vector EndPoint)
{
	int NumPhaseGates = UTIL_GetNumBuiltStructuresOfType(STRUCTURE_MARINE_PHASEGATE);

	float DirectDist = vDist2D(StartPoint, EndPoint);

	if (NumPhaseGates < 2)
	{
		return DirectDist;
	}

	edict_t* StartPhase = UTIL_GetNearestStructureOfTypeInLocation(STRUCTURE_MARINE_PHASEGATE, StartPoint, DirectDist, true, false);

	if (FNullEnt(StartPhase))
	{
		return DirectDist;
	}

	edict_t* EndPhase = UTIL_GetNearestStructureOfTypeInLocation(STRUCTURE_MARINE_PHASEGATE, EndPoint, DirectDist, true, false);

	if (FNullEnt(EndPhase))
	{
		return DirectDist;
	}

	float PhaseDist = vDist2DSq(StartPoint, StartPhase->v.origin) + vDist2DSq(EndPoint, EndPhase->v.origin);
	PhaseDist = sqrtf(PhaseDist);


	return fminf(DirectDist, PhaseDist);
}

float UTIL_GetPhaseDistanceBetweenPointsSq(const Vector StartPoint, const Vector EndPoint)
{
	int NumPhaseGates = UTIL_GetNumBuiltStructuresOfType(STRUCTURE_MARINE_PHASEGATE);

	float DirectDist = vDist2DSq(StartPoint, EndPoint);

	if (NumPhaseGates < 2)
	{
		return DirectDist;
	}

	edict_t* StartPhase = UTIL_GetNearestStructureOfTypeInLocation(STRUCTURE_MARINE_PHASEGATE, StartPoint, DirectDist, true, false);

	if (FNullEnt(StartPhase))
	{
		return DirectDist;
	}

	edict_t* EndPhase = UTIL_GetNearestStructureOfTypeInLocation(STRUCTURE_MARINE_PHASEGATE, EndPoint, DirectDist, true, false);

	if (FNullEnt(EndPhase))
	{
		return DirectDist;
	}

	float PhaseDist = vDist2DSq(StartPoint, StartPhase->v.origin) + vDist2DSq(EndPoint, EndPhase->v.origin);


	return fminf(DirectDist, PhaseDist);
}


NSStructureType GetStructureTypeFromEdict(const edict_t* StructureEdict)
{
	if (FNullEnt(StructureEdict)) { return STRUCTURE_NONE; }

	return UTIL_IUSER3ToStructureType(StructureEdict->v.iuser3);
}

NSStructureType UTIL_IUSER3ToStructureType(const int inIUSER3)
{
	if (inIUSER3 == AVH_USER3_COMMANDER_STATION) { return STRUCTURE_MARINE_COMMCHAIR; }
	if (inIUSER3 == AVH_USER3_RESTOWER) { return STRUCTURE_MARINE_RESTOWER; }
	if (inIUSER3 == AVH_USER3_INFANTRYPORTAL) { return STRUCTURE_MARINE_INFANTRYPORTAL; }
	if (inIUSER3 == AVH_USER3_ARMORY) { return STRUCTURE_MARINE_ARMOURY; }
	if (inIUSER3 == AVH_USER3_ADVANCED_ARMORY) { return STRUCTURE_MARINE_ADVARMOURY; }
	if (inIUSER3 == AVH_USER3_TURRET_FACTORY) { return STRUCTURE_MARINE_TURRETFACTORY; }
	if (inIUSER3 == AVH_USER3_ADVANCED_TURRET_FACTORY) { return STRUCTURE_MARINE_ADVTURRETFACTORY; }
	if (inIUSER3 == AVH_USER3_TURRET) { return STRUCTURE_MARINE_TURRET; }
	if (inIUSER3 == AVH_USER3_SIEGETURRET) { return STRUCTURE_MARINE_SIEGETURRET; }
	if (inIUSER3 == AVH_USER3_ARMSLAB) { return STRUCTURE_MARINE_ARMSLAB; }
	if (inIUSER3 == AVH_USER3_PROTOTYPE_LAB) { return STRUCTURE_MARINE_PROTOTYPELAB; }
	if (inIUSER3 == AVH_USER3_OBSERVATORY) { return STRUCTURE_MARINE_OBSERVATORY; }
	if (inIUSER3 == AVH_USER3_PHASEGATE) { return STRUCTURE_MARINE_PHASEGATE; }

	if (inIUSER3 == AVH_USER3_HIVE) { return STRUCTURE_ALIEN_HIVE; }
	if (inIUSER3 == AVH_USER3_ALIENRESTOWER) { return STRUCTURE_ALIEN_RESTOWER; }
	if (inIUSER3 == AVH_USER3_DEFENSE_CHAMBER) { return STRUCTURE_ALIEN_DEFENCECHAMBER; }
	if (inIUSER3 == AVH_USER3_SENSORY_CHAMBER) { return STRUCTURE_ALIEN_SENSORYCHAMBER; }
	if (inIUSER3 == AVH_USER3_MOVEMENT_CHAMBER) { return STRUCTURE_ALIEN_MOVEMENTCHAMBER; }
	if (inIUSER3 == AVH_USER3_OFFENSE_CHAMBER) { return STRUCTURE_ALIEN_OFFENCECHAMBER; }

	return STRUCTURE_NONE;

}

bool UTIL_StructureTypesMatch(const NSStructureType TypeOne, const NSStructureType TypeTwo)
{
	return (TypeOne == TypeTwo
		|| (TypeOne == STRUCTURE_MARINE_ANYARMOURY && (TypeTwo == STRUCTURE_MARINE_ARMOURY || TypeTwo == STRUCTURE_MARINE_ADVARMOURY))
		|| (TypeOne == STRUCTURE_MARINE_ANYTURRETFACTORY && (TypeTwo == STRUCTURE_MARINE_TURRETFACTORY || TypeTwo == STRUCTURE_MARINE_ADVTURRETFACTORY))
		|| (TypeOne == STRUCTURE_MARINE_ANYTURRET && (TypeTwo == STRUCTURE_MARINE_TURRET || TypeTwo == STRUCTURE_MARINE_SIEGETURRET))
		|| (TypeTwo == STRUCTURE_MARINE_ANYARMOURY && (TypeOne == STRUCTURE_MARINE_ARMOURY || TypeOne == STRUCTURE_MARINE_ADVARMOURY))
		|| (TypeTwo == STRUCTURE_MARINE_ANYTURRETFACTORY && (TypeOne == STRUCTURE_MARINE_TURRETFACTORY || TypeOne == STRUCTURE_MARINE_ADVTURRETFACTORY))
		|| (TypeTwo == STRUCTURE_MARINE_ANYTURRET && (TypeOne == STRUCTURE_MARINE_TURRET || TypeOne == STRUCTURE_MARINE_SIEGETURRET))
		|| (TypeOne == STRUCTURE_ANY_MARINE_STRUCTURE && UTIL_IsMarineStructure(TypeTwo))
		|| (UTIL_IsMarineStructure(TypeOne) && TypeTwo == STRUCTURE_ANY_MARINE_STRUCTURE)
		|| (TypeOne == STRUCTURE_ANY_ALIEN_STRUCTURE && UTIL_IsAlienStructure(TypeTwo))
		|| (UTIL_IsAlienStructure(TypeOne) && TypeTwo == STRUCTURE_ANY_ALIEN_STRUCTURE)
		);
}

NSStructureType UTIL_GetChamberTypeForHiveTech(const HiveTechStatus HiveTech)
{
	switch (HiveTech)
	{
	case HIVE_TECH_MOVEMENT:
		return STRUCTURE_ALIEN_MOVEMENTCHAMBER;
	case HIVE_TECH_DEFENCE:
		return STRUCTURE_ALIEN_DEFENCECHAMBER;
	case HIVE_TECH_SENSORY:
		return STRUCTURE_ALIEN_SENSORYCHAMBER;
	default:
		return STRUCTURE_NONE;
	}
}

bool UTIL_StructureIsRecycling(const edict_t* Structure)
{
	return (Structure && (Structure->v.iuser4 & MASK_RECYCLING));
}

bool UTIL_StructureIsUpgrading(const edict_t* Structure)
{
	NSStructureType StructureType = GetStructureTypeFromEdict(Structure);

	if (StructureType == STRUCTURE_MARINE_ARMOURY)
	{
		return UTIL_IsArmouryUpgrading(Structure);
	}

	if (StructureType == STRUCTURE_MARINE_TURRETFACTORY)
	{
		return UTIL_IsTurretFactoryUpgrading(Structure);
	}

	return false;
}

bool UTIL_IsArmouryUpgrading(const edict_t* ArmouryEdict)
{
	return (ArmouryEdict && UTIL_StructureIsResearching(ArmouryEdict) && (ArmouryEdict->v.iuser2 == IMPULSE_COMMANDER_UPGRADE_ARMOURY));
}

bool UTIL_IsTurretFactoryUpgrading(const edict_t* TurretFactoryEdict)
{
	return (TurretFactoryEdict && UTIL_StructureIsResearching(TurretFactoryEdict) && (TurretFactoryEdict->v.iuser2 == IMPULSE_COMMANDER_UPGRADE_TURRETFACTORY));
}

bool UTIL_StructureIsResearching(const edict_t* Structure)
{
	if (!Structure) { return false; }

	float NormalisedProgress = ((Structure->v.fuser1 / kNormalizationNetworkFactor) - kResearchFuser1Base);
	float ClampedNormalizedProgress = clampf(NormalisedProgress, 0.0f, 1.0f);

	return ClampedNormalizedProgress > 0.0f && ClampedNormalizedProgress < 1.0f;
}

bool UTIL_StructureIsResearching(const edict_t* Structure, const NSResearch Research)
{
	if (!Structure) { return false; }

	return (UTIL_StructureIsResearching(Structure) && Structure->v.iuser2 == (int)Research);
}

bool UTIL_IsStructureElectrified(const edict_t* Structure)
{
	if (FNullEnt(Structure)) { return false; }

	return (UTIL_StructureIsFullyBuilt(Structure) && !UTIL_StructureIsRecycling(Structure) && (Structure->v.deadflag == DEAD_NO) && (Structure->v.iuser4 & MASK_UPGRADE_11));
}

NSDeployableItem UTIL_WeaponTypeToDeployableItem(const NSWeapon WeaponType)
{
	switch (WeaponType)
	{
	case WEAPON_MARINE_SHOTGUN:
		return ITEM_MARINE_SHOTGUN;
	case WEAPON_MARINE_GL:
		return ITEM_MARINE_GRENADELAUNCHER;
	case WEAPON_MARINE_HMG:
		return ITEM_MARINE_HMG;
	case WEAPON_MARINE_WELDER:
		return ITEM_MARINE_WELDER;
	default:
		return ITEM_NONE;
	}

	return ITEM_NONE;
}



AvHUpgradeMask UTIL_GetResearchMask(const NSResearch Research)
{
	switch (Research)
	{
	case RESEARCH_ARMSLAB_ARMOUR1:
		return MASK_UPGRADE_5;
	case RESEARCH_ARMSLAB_ARMOUR2:
		return MASK_UPGRADE_6;
	case RESEARCH_ARMSLAB_ARMOUR3:
		return MASK_UPGRADE_7;
	case RESEARCH_ARMSLAB_WEAPONS1:
		return MASK_UPGRADE_1;
	case RESEARCH_ARMSLAB_WEAPONS2:
		return MASK_UPGRADE_2;
	case RESEARCH_ARMSLAB_WEAPONS3:
		return MASK_UPGRADE_3;
	case RESEARCH_ARMSLAB_CATALYSTS:
		return MASK_UPGRADE_4;
	case RESEARCH_ARMOURY_GRENADES:
		return MASK_UPGRADE_5;
	case RESEARCH_OBSERVATORY_DISTRESSBEACON:
		return MASK_UPGRADE_5;
	case RESEARCH_OBSERVATORY_MOTIONTRACKING:
		return MASK_UPGRADE_6;
	case RESEARCH_OBSERVATORY_PHASETECH:
		return MASK_UPGRADE_2;
	case RESEARCH_PROTOTYPELAB_HEAVYARMOUR:
		return MASK_UPGRADE_5;
	case RESEARCH_PROTOTYPELAB_JETPACKS:
		return MASK_UPGRADE_1;
	default:
		return MASK_NONE;

	}
}

int UTIL_GetCostOfStructureType(NSStructureType StructureType)
{
	switch (StructureType)
	{
	case STRUCTURE_MARINE_ARMOURY:
		return kArmoryCost;
		break;
	case STRUCTURE_MARINE_ARMSLAB:
		return kArmsLabCost;
		break;
	case STRUCTURE_MARINE_COMMCHAIR:
		return kCommandStationCost;
		break;
	case STRUCTURE_MARINE_INFANTRYPORTAL:
		return kInfantryPortalCost;
		break;
	case STRUCTURE_MARINE_OBSERVATORY:
		return kObservatoryCost;
		break;
	case STRUCTURE_MARINE_PHASEGATE:
		return kPhaseGateCost;
		break;
	case STRUCTURE_MARINE_PROTOTYPELAB:
		return kPrototypeLabCost;
		break;
	case STRUCTURE_MARINE_RESTOWER:
	case STRUCTURE_ALIEN_RESTOWER:
		return kResourceTowerCost;
		break;
	case STRUCTURE_MARINE_SIEGETURRET:
		return kSiegeCost;
		break;
	case STRUCTURE_MARINE_TURRET:
		return kSentryCost;
		break;
	case STRUCTURE_MARINE_TURRETFACTORY:
		return kTurretFactoryCost;
		break;
	case STRUCTURE_ALIEN_HIVE:
		return kHiveCost;
		break;
	case STRUCTURE_ALIEN_OFFENCECHAMBER:
		return kOffenseChamberCost;
		break;
	case STRUCTURE_ALIEN_DEFENCECHAMBER:
		return kDefenseChamberCost;
		break;
	case STRUCTURE_ALIEN_MOVEMENTCHAMBER:
		return kMovementChamberCost;
		break;
	case STRUCTURE_ALIEN_SENSORYCHAMBER:
		return kSensoryChamberCost;
		break;
	default:
		return 0;

	}

	return 0;
}

int UTIL_StructureTypeToImpulseCommand(const NSStructureType StructureType)
{
	switch (StructureType)
	{
	case STRUCTURE_MARINE_ARMOURY:
		return IMPULSE_COMMANDER_BUILD_ARMOURY;
	case STRUCTURE_MARINE_ARMSLAB:
		return IMPULSE_COMMANDER_BUILD_ARMSLAB;
	case STRUCTURE_MARINE_COMMCHAIR:
		return IMPULSE_COMMANDER_BUILD_COMMCHAIR;
	case STRUCTURE_MARINE_INFANTRYPORTAL:
		return IMPULSE_COMMANDER_BUILD_INFANTRYPORTAL;
	case STRUCTURE_MARINE_OBSERVATORY:
		return IMPULSE_COMMANDER_BUILD_OBSERVATORY;
	case STRUCTURE_MARINE_PHASEGATE:
		return IMPULSE_COMMANDER_BUILD_PHASEGATE;
	case STRUCTURE_MARINE_PROTOTYPELAB:
		return IMPULSE_COMMANDER_BUILD_PROTOTYPELAB;
	case STRUCTURE_MARINE_RESTOWER:
		return IMPULSE_COMMANDER_BUILD_RESTOWER;
	case STRUCTURE_MARINE_SIEGETURRET:
		return IMPULSE_COMMANDER_BUILD_SIEGETURRET;
	case STRUCTURE_MARINE_TURRET:
		return IMPULSE_COMMANDER_BUILD_TURRET;
	case STRUCTURE_MARINE_TURRETFACTORY:
		return IMPULSE_COMMANDER_BUILD_TURRETFACTORY;

	case STRUCTURE_ALIEN_DEFENCECHAMBER:
		return IMPULSE_ALIEN_BUILD_DEFENCECHAMBER;
	case STRUCTURE_ALIEN_MOVEMENTCHAMBER:
		return IMPULSE_ALIEN_BUILD_MOVEMENTCHAMBER;
	case STRUCTURE_ALIEN_SENSORYCHAMBER:
		return IMPULSE_ALIEN_BUILD_SENSORYCHAMBER;
	case STRUCTURE_ALIEN_OFFENCECHAMBER:
		return IMPULSE_ALIEN_BUILD_OFFENCECHAMBER;
	case STRUCTURE_ALIEN_RESTOWER:
		return IMPULSE_ALIEN_BUILD_RESTOWER;
	case STRUCTURE_ALIEN_HIVE:
		return IMPULSE_ALIEN_BUILD_HIVE;
	default:
		return 0;

	}

	return 0;
}

bool UTIL_IsThereACommander()
{
	for (int i = 0; i < 32; i++)
	{
		if (!FNullEnt(clients[i]) && IsPlayerCommander(clients[i]))
		{
			return true;
		}
	}

	return false;
}

char* UTIL_BotRoleToChar(const BotRole Role)
{
	switch (Role)
	{
	case BOT_ROLE_BUILDER:
		return "Builder";
	case BOT_ROLE_COMMAND:
		return "Command";
	case BOT_ROLE_DESTROYER:
		return "Destroyer";
	case BOT_ROLE_FIND_RESOURCES:
		return "Find Resources";
	case BOT_ROLE_SWEEPER:
		return "Sweeper";
	case BOT_ROLE_HARASS:
		return "Harasser";
	case BOT_ROLE_NONE:
		return "None";
	case BOT_ROLE_RES_CAPPER:
		return "Resource Capper";
	case BOT_ROLE_ASSAULT:
		return "Assault";
	default:
		return "INVALID";
	}

	return "INVALID";
}

const char* UTIL_StructTypeToChar(const NSStructureType StructureType)
{
	switch (StructureType)
	{
	case STRUCTURE_MARINE_COMMCHAIR:
		return "Comm Chair";
	case STRUCTURE_MARINE_RESTOWER:
		return "Marine Resource Tower";
	case STRUCTURE_MARINE_INFANTRYPORTAL:
		return "Infantry Portal";
	case STRUCTURE_MARINE_ARMOURY:
		return "Armoury";
	case STRUCTURE_MARINE_ADVARMOURY:
		return "Advanced Armoury";
	case STRUCTURE_MARINE_TURRETFACTORY:
		return "Turret Factory";
	case STRUCTURE_MARINE_ADVTURRETFACTORY:
		return "Advanced Turret Factory";
	case STRUCTURE_MARINE_TURRET:
		return "Turret";
	case STRUCTURE_MARINE_SIEGETURRET:
		return "Siege Turret";
	case STRUCTURE_MARINE_ARMSLAB:
		return "Arms Lab";
	case STRUCTURE_MARINE_PROTOTYPELAB:
		return "Prototype Lab";
	case STRUCTURE_MARINE_OBSERVATORY:
		return "Observatory";
	case STRUCTURE_MARINE_PHASEGATE:
		return "Phase Gate";

	case STRUCTURE_ALIEN_HIVE:
		return "Hive";
	case STRUCTURE_ALIEN_RESTOWER:
		return "Alien Resource Tower";
	case STRUCTURE_ALIEN_DEFENCECHAMBER:
		return "Defence Chamber";
	case STRUCTURE_ALIEN_SENSORYCHAMBER:
		return "Sensory Chamber";
	case STRUCTURE_ALIEN_MOVEMENTCHAMBER:
		return "Movement Chamber";
	case STRUCTURE_ALIEN_OFFENCECHAMBER:
		return "Offence Chamber";

	default:
		return "INVALID";

	}
}

const char* UTIL_ResearchTypeToChar(const NSResearch ResearchType)
{
	switch (ResearchType)
	{
	case RESEARCH_NONE:
		return "None";
		break;
	case RESEARCH_OBSERVATORY_DISTRESSBEACON:
		return "Distress Beacon";
		break;
	case RESEARCH_OBSERVATORY_MOTIONTRACKING:
		return "Motion Tracking";
		break;
	case RESEARCH_OBSERVATORY_PHASETECH:
		return "Phase Tech";
		break;
	case RESEARCH_ARMSLAB_ARMOUR1:
		return "Armour Level 1";
		break;
	case RESEARCH_ARMSLAB_ARMOUR2:
		return "Armour Level 2";
		break;
	case RESEARCH_ARMSLAB_ARMOUR3:
		return "Armour Level 3";
		break;
	case RESEARCH_ARMSLAB_WEAPONS1:
		return "Weapons Level 1";
		break;
	case RESEARCH_ARMSLAB_WEAPONS2:
		return "Weapons Level 2";
		break;
	case RESEARCH_ARMSLAB_WEAPONS3:
		return "Weapons Level 3";
		break;
	case RESEARCH_PROTOTYPELAB_HEAVYARMOUR:
		return "Heavy Armour";
		break;
	case RESEARCH_PROTOTYPELAB_JETPACKS:
		return "Jetpacks";
		break;
	case RESEARCH_ARMOURY_GRENADES:
		return "Grenades";
		break;
	default:
		return "INVALID";

	}

	return "INVALID";
}

const char* UTIL_DroppableItemTypeToChar(const NSDeployableItem ItemType)
{
	switch (ItemType)
	{
	case ITEM_MARINE_AMMO:
		return "Ammo";
		break;
	case ITEM_MARINE_HEALTHPACK:
		return "Healthpack";
		break;
	case ITEM_MARINE_CATALYSTS:
		return "Catalysts";
		break;
	case ITEM_MARINE_GRENADELAUNCHER:
		return "Grenade Launcher";
		break;
	case ITEM_MARINE_HEAVYARMOUR:
		return "Heavy Armour";
		break;
	case ITEM_MARINE_HMG:
		return "HMG";
		break;
	case ITEM_MARINE_JETPACK:
		return "Jetpack";
		break;
	case ITEM_MARINE_MINES:
		return "Mines";
		break;
	case ITEM_MARINE_SCAN:
		return "Scan";
		break;
	case ITEM_MARINE_SHOTGUN:
		return "Shotgun";
		break;
	case ITEM_MARINE_WELDER:
		return "Welder";
		break;
	default:
		return "Invalid";
		break;
	}

	return "Invalid";
}

edict_t* UTIL_GetNearestUnattackedStructureOfTeamInLocation(const Vector Location, edict_t* IgnoreStructure, const int Team, const float SearchRadius)
{
	edict_t* Result = NULL;
	float MinDist = 0.0f;
	float MaxDist = sqrf(SearchRadius);

	int IgnoreIndex = (!FNullEnt(IgnoreStructure)) ? ENTINDEX(IgnoreStructure) : -1;

	bool bMarineStructures = (Team == MARINE_TEAM);
	int AttackerTeam = (Team == MARINE_TEAM) ? ALIEN_TEAM : MARINE_TEAM;

	if (bMarineStructures)
	{
		for (auto& it : MarineBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if(it.first == IgnoreIndex) { continue; }

			float thisDist = vDist2DSq(it.second.Location, Location);

			if (thisDist > MaxDist) { continue; }

			int NumPlayersAttacking = UTIL_GetNumPlayersOfTeamInArea(it.second.edict->v.origin, UTIL_MetresToGoldSrcUnits(1.5f), AttackerTeam, nullptr, CLASS_GORGE, false);

			if (NumPlayersAttacking >= 2) { continue; }

			if (!Result || thisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}

		}
	}
	else
	{
		for (auto& it : AlienBuildableStructureMap)
		{
			if (!it.second.bOnNavmesh) { continue; }
			if (it.first == IgnoreIndex) { continue; }

			float thisDist = vDist2DSq(it.second.Location, Location);

			if (thisDist > MaxDist) { continue; }

			int NumPlayersAttacking = UTIL_GetNumPlayersOfTeamInArea(it.second.edict->v.origin, UTIL_MetresToGoldSrcUnits(1.5f), AttackerTeam, nullptr, CLASS_GORGE, false);

			if (NumPlayersAttacking >= 2) { continue; }

			

			if (!Result || thisDist < MinDist)
			{
				Result = it.second.edict;
				MinDist = thisDist;
			}

		}

	}

	return Result;
}

bool UTIL_IsBuildableStructureStillReachable(bot_t* pBot, const edict_t* Structure)
{
	int Index = ENTINDEX(Structure);

	bool bIsMarine = IsPlayerMarine(pBot->pEdict);

	NSStructureType StructureType = GetStructureTypeFromEdict(Structure);

	// Hives have static positions so should always be reachable.
	// Resource towers technically do too, but there could be some built by humans which the bots can't get to
	if (StructureType == STRUCTURE_ALIEN_HIVE || StructureType == STRUCTURE_NONE) { return true; }

	if (UTIL_IsMarineStructure(Structure))
	{
		bool IsReachable = (bIsMarine) ? MarineBuildableStructureMap[Index].bIsReachableMarine : MarineBuildableStructureMap[Index].bIsReachableAlien;

		return IsReachable;
	}
	else
	{
		bool IsReachable = (bIsMarine) ? AlienBuildableStructureMap[Index].bIsReachableMarine : AlienBuildableStructureMap[Index].bIsReachableAlien;

		return IsReachable;
	}

	return true;
}

bool UTIL_IsDroppedItemStillReachable(bot_t* pBot, const edict_t* Item)
{
	int Index = ENTINDEX(Item);

	return MarineDroppedItemMap[Index].bIsReachableMarine;
}