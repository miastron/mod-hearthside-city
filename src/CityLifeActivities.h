/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CITY_LIFE_ACTIVITIES_H
#define CITY_LIFE_ACTIVITIES_H

#include "CityLifeTypes.h"
#include "Define.h"

class Player;

namespace CityLife
{
    class Manager;

    namespace Activities
    {
        void ExecuteLivingCrowd(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                                float playerRadius, uint32 chatCooldownSec, uint32 afkMinSec,
                                uint32 afkMaxSec, bool enableAfk = true);

        // Faces the nearest duel in progress within watchRadius (mod-hearthside-pvp's, or any other)
        // and cheers it on; with no duel running the spectator just waits at its stand.
        void ExecuteDuelSpectator(Resident& resident, Player* bot, Hub const& hub, uint32 now, float watchRadius);

        // The step back out of a fight ExecuteDuelSpectator planned, once its own moment comes
        // (Resident::SpectatorStepAtMs; Manager::UpdateSpectatorSteps).
        void StepBackSpectator(Resident& resident, Player* bot, Hub const& hub);

        void ExecuteFishing(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteInnDance(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecutePlazaSocial(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteTavernSitting(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteTavernDrinking(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteVisitNpc(Resident& resident, Player* bot, Hub const& hub, uint32 now, uint32 npcFlag);

        void ExecuteVisitMailbox(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteBuyFood(Resident& resident, Player* bot, Hub const& hub, uint32 now, bool enableInteractions);

        void ExecuteWallPatrol(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteReleaseCycle(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteRandomEmote(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        // Safe State Cleaners
        void CrowdClearAfk(Player* bot);
        void CrowdReleaseChat(Player* bot, Resident& resident);
        void StopDance(Player* bot, Resident& resident);
        Player* FindNearbyRealPlayer(Player* bot, float radius);
    }
}

#endif
