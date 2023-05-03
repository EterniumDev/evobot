//
// EvoBot - Neoptolemus' Natural Selection bot, based on Botman's HPB bot template
//
// player_util.h
// 
// Handles all helper functions to do with a player's state
//

#pragma once

#ifndef PLAYER_UTIL_H
#define PLAYER_UTIL_H

#include <extdll.h>

#include "NS_Constants.h"

// How far a bot can be from a useable object when trying to interact with it. Used also for melee attacks
static const float max_player_use_reach = 55.0f;

// Minimum time a bot can wait between attempts to use something in seconds (when not holding the use key down)
static const float min_player_use_interval = 0.5f;

// Minimum time a bot can wait between attempts to use something in seconds (when not holding the use key down)
static const float max_player_jump_height = 60.0f;

/****************

Player Status Checks

*****************/

// Is the player currently a Skulk?
bool IsPlayerSkulk(const edict_t* Player);
// Is the player currently a Gorge?
bool IsPlayerGorge(const edict_t* Player);
// Is the player currently a Lerk?
bool IsPlayerLerk(const edict_t* Player);
// Is the player currently a Fade?
bool IsPlayerFade(const edict_t* Player);
// Is the player currently an Onos?
bool IsPlayerOnos(const edict_t* Player);
// Is the player currently a Marine (not commander)? Includes both light and heavy marines
bool IsPlayerMarine(const edict_t* Player);
// Is the player currently a Marine (not commander)? Includes both light and heavy marines
bool IsPlayerAlien(const edict_t* Player);
// Is the player the commander?
bool IsPlayerCommander(const edict_t* Player);
// Is the player currently climbing a wall?
bool IsPlayerClimbingWall(const edict_t* Player);
// Is the player in the ready room (i.e. not in the map proper)?
bool IsPlayerInReadyRoom(const edict_t* Player);
// Returns true if the player is not in the ready room, is on a team, is alive, is not being digested, and is not commander
bool IsPlayerActiveInGame(const edict_t* Player);
// Is the player a human?
bool IsPlayerHuman(const edict_t* Player);
// Is the player a bot (includes non-EvoBot fake clients)?
bool IsPlayerBot(const edict_t* Player);
// Is the player dead and waiting to respawn?
bool IsPlayerDead(const edict_t* Player);
// Is player stunned by Onos stomp?
bool IsPlayerStunned(const edict_t* Player);
// Is the player currently spectating?
bool IsPlayerSpectator(const edict_t* Player);
// Is the player currently being digested by an Onos?
bool IsPlayerBeingDigested(const edict_t* Player);
// Is the player an Onos and currently digesting someone?
bool IsPlayerDigesting(const edict_t* Player);
// Is the player currently gestating?
bool IsPlayerGestating(const edict_t* Player);
// Is the player on the marine team (i.e. not spectating or in the ready room)
bool IsPlayerOnMarineTeam(const edict_t* Player);
// Is the player on the alien team (i.e. not spectating or in the ready room)
bool IsPlayerOnAlienTeam(const edict_t* Player);
// Is the player affected by parasite?
bool IsPlayerParasited(const edict_t* Player);
// Is the player being marked through walls to enemies through being sighted by an ally or affected by motion tracking?
bool IsPlayerMotionTracked(const edict_t* Player);
// Is the player currently on a ladder? Always false for Skulks and Lerks as they can't climb ladders
bool IsPlayerOnLadder(const edict_t* Player);
// Is the player an onos under the effect of charge?
bool IsPlayerCharging(const edict_t* Player);

// Returns the player's max armour, based on armour research levels (marines) or class and carapace level (aliens)
int GetPlayerMaxArmour(const edict_t* Player);

// Returns the player's current energy (between 0.0 and 1.0)
float GetPlayerEnergy(const edict_t* Player);

// What player class is this player currently?
NSPlayerClass GetPlayerClass(const edict_t* Player);

// Returns player resources (for marines will be team resources)
int GetPlayerResources(const edict_t* Player);

// Returns the player radius based on their current state
float GetPlayerRadius(const edict_t* pEdict);

// Returns the hull index that should be used for this player when performing hull traces. Depends on if player is crouching right now or not
int GetPlayerHullIndex(const edict_t* pEdict);

// Returns the hull index that should be used for this player when performing hull traces, can manually specify if it's their crouching hull index or not
int GetPlayerHullIndex(const edict_t* pEdict, const bool bIsCrouching);

// How fast the player current regenerates energy, taking adrenaline upgrades into account
float GetPlayerEnergyRegenPerSecond(edict_t* Player);

// Expresses the combined health and armour of a player vs max
float GetPlayerOverallHealthPercent(const edict_t* Player);

// Gets the world position of the player's viewpoint (i.e. camera position)
Vector GetPlayerEyePosition(const edict_t* Player);

// Player's current height based on their player class, can manually specify if you want the crouched height or not
float GetPlayerHeight(const edict_t* Player, const bool bIsCrouching);

// The origin offset from the bottom of the player's collision box, depending on crouching or not
Vector GetPlayerOriginOffsetFromFloor(const edict_t* pEdict, const bool bIsCrouching);

// The bottom-centre of the player's collision box
Vector GetPlayerBottomOfCollisionHull(const edict_t* pEdict);

// The top-centre of the player's collision box based on their player class and if they're currently crouching
Vector GetPlayerTopOfCollisionHull(const edict_t* pEdict);

// The top-centre of the player's collision box based on their player class, manually specifying if they're crouching or not
Vector GetPlayerTopOfCollisionHull(const edict_t* pEdict, const bool bIsCrouching);

// Based on current movement inputs from the player, what direction are they trying to move? Ignores actual velocity.
Vector GetPlayerAttemptedMoveDirection(const edict_t* Player);

// The player's index into the clients array (see game_state.h)
int GetPlayerIndex(const edict_t* Edict);

// Returns true if the supplied edict is a player (bot or human)
bool IsEdictPlayer(const edict_t* edict);

bool IsPlayerInUseRange(const edict_t* Player, const edict_t* Target);

bool PlayerHasHeavyArmour(const edict_t* Player);

bool PlayerHasJetpack(edict_t* Player);

bool PlayerHasEquipment(edict_t* Player);
bool PlayerHasSpecialWeapon(edict_t* Player);

bool PlayerHasWeapon(edict_t* Player, NSWeapon WeaponType);

bool UTIL_PlayerHasLOSToEntity(const edict_t* Player, const edict_t* Target, const float MaxRange, const bool bUseHullSweep);
bool UTIL_PlayerHasLOSToLocation(const edict_t* Player, const Vector Target, const float MaxRange);

bool PlayerHasAlienUpgradeOfType(const edict_t* Player, const HiveTechStatus TechType);

float GetPlayerCloakAmount(const edict_t* Player);

#endif