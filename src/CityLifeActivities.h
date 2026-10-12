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
                                bool enableSay, uint32 sayCooldownSec, float playerRadius,
                                uint32 chatCooldownSec, uint32 afkMinSec, uint32 afkMaxSec,
                                bool enableAfk = true);

        void ExecuteDuel(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                         uint32 duelCooldownSec, uint32 realPlayerCooldownSec, float duelMaxDist, bool meetingPointEnabled = true);

        void ExecuteDuelSpectator(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        // Tactical Duel Behaviors & Rendezvous
        void ComputeDuelMeetPoint(Player* bot, Player* opponent, Spot const& anchorSpot,
                                  float maxDistance, float& meetX, float& meetY, float& meetZ);
        void ComputeDuelStandPoint(Player* bot, Player* opponent, float meetX, float meetY,
                                   float meetZ, float& standX, float& standY, float& standZ);
        bool IsRangedDueler(Player* bot);
        bool IsMeleeDueler(Player* bot);
        bool MoveToRangedDuelDistance(Player* bot, Resident& resident, Player* opponent,
                                      Spot const* anchorSpot, float leashRadius);
        // acceptPlayerChallenges = CityLife.Duels.AllowPlayerChallenge: a challenge from a human is
        // accepted only when it is set; one this module did not arrange is otherwise declined.
        void DriveActiveDuel(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                             float leashRadius, bool tacticalRange, bool acceptPlayerChallenges);
        void CancelCityDuel(Player* bot, char const* reason);

        void ExecuteFishing(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteInnDance(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                             bool enableSay, uint32 sayCooldownSec);

        void ExecutePlazaSocial(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                                bool enableUnrestricted, uint32 randomSayChance, uint32 sayCooldownSec);

        void ExecuteTavernSitting(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteTavernDrinking(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteVisitNpc(Resident& resident, Player* bot, Hub const& hub, uint32 now, uint32 npcFlag);

        void ExecuteVisitMailbox(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteBuyFood(Resident& resident, Player* bot, Hub const& hub, uint32 now, bool enableInteractions);

        void ExecuteWallPatrol(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        void ExecuteRandomEmote(Resident& resident, Player* bot, Hub const& hub, uint32 now);

        // Safe State Cleaners
        void CrowdClearAfk(Player* bot);
        void CrowdReleaseChat(Player* bot, Resident& resident);
        void StopDance(Player* bot, Resident& resident);
        Player* FindNearbyRealPlayer(Player* bot, float radius);
    }
}

#endif
