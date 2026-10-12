/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CITY_LIFE_MOVEMENT_H
#define CITY_LIFE_MOVEMENT_H

#include "CityLifeTypes.h"
#include "Define.h"
#include "PathGenerator.h"

class Player;

namespace CityLife
{
    namespace Movement
    {
        bool LowestPoiZWithin(Hub const& hub, float x, float y, float radius, float* outMinZ);
        void CheckSinkForensics(Player* bot, float prevZ, bool sinkForensicsEnabled);
        void DismountIfMounted(Player* bot);
        bool CanMountHere(Player* bot);
        bool TryMount(Player* bot);
        void MoveToPosition(Player* bot, uint32 mapId, float x, float y, float z, float o, bool teleport);
        bool IsAtSpot(Player* bot, Spot const& spot, float tolerance = 3.5f);

        bool PathTouchesWater(Player* bot, PathGenerator const& path);
        bool HasCompleteWalkablePath(Player* bot, float x, float y, float z, bool allowWater = false);
        bool ValidateCityDestination(Player* bot, uint32 mapId, float x, float y, float z, float o, bool allowWater = false);
    }
}

#endif
