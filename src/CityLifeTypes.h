/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CITY_LIFE_TYPES_H
#define CITY_LIFE_TYPES_H

#include "Define.h"
// The shared Life headers and their one registry are mod-hearthside-life's.
#if __has_include("../../mod-hearthside-life/src/LifeBotReservation.h")
#include "../../mod-hearthside-life/src/LifeBotReservation.h"
#else
#error "mod-hearthside-city requires mod-hearthside-life beside it in modules/"
#endif
#include "../../mod-hearthside-life/src/LifeMotion.h"
#include "../../mod-hearthside-life/src/LifeBots.h"
#include "../../mod-hearthside-life/src/LifeActivity.h"
#include "../../mod-hearthside-life/src/LifeEvents.h"

#include <string>
#include <vector>

namespace CityLife
{
    // Millisecond deadlines are uint32 game-time stamps, which wrap; compare by signed difference.
    // A deadline of 0 means "none set" and is always reached.
    inline bool MsReached(uint32 now, uint32 deadline)
    {
        return deadline == 0 || static_cast<int32>(now - deadline) >= 0;
    }

    enum class TeamSide : uint8
    {
        Alliance = 0,
        Horde = 1,
        Any = 2
    };

    enum class CitizenRole : uint8
    {
        Flex = 0,
        InnDancer = 1,
        PlazaTalker = 2,
        Spectator = 3, // watches mod-hearthside-pvp's gate and Goldshire duels
        Fisher = 5,
        Crowd = 6
    };

    enum class CitizenActivity : uint8
    {
        LeaveInn = 0,
        VisitAuctionHouse = 1,
        VisitMailbox = 2,
        BuyFood = 3,
        WanderDistrict = 4,
        SitInTavern = 5,
        WatchDuels = 6,
        VisitTrainer = 7,
        DrinkInTavern = 8,
        WalkCityWalls = 9,
        ReturnToInn = 10,
        ReleaseCycle = 11, // residence over: asleep in an inn bed until the release
        RandomEmote = 12,
        FishAtDocks = 13,
        Idle = 14,
        InnDance = 16,
        PlazaSocial = 17,
        CrowdSocial = 18
    };

    enum class CityPoiType : uint8
    {
        Inn = 0,
        AuctionHouse = 1,
        Mailbox = 2,
        Tavern = 3,
        Trainer = 4,
        Dock = 5,
        WallPatrol = 6,
        Plaza = 7,
        Vendor = 8,
        DuelRing = 9, // spectator stand at a mod-hearthside-pvp duel zone
        InnBed = 10,
        Bank = 11
    };

    enum class CrowdPhase : uint8
    {
        Linger = 0,
        Afk = 1,
        WanderOut = 2,
        WanderBack = 3
    };

    enum class DemandMode : uint8
    {
        Fixed = 0,
        Dynamic = 1,
        Hybrid = 2
    };

    enum class DistributionMode : uint8
    {
        Legacy = 0,
        Scaled = 1
    };

    struct Spot
    {
        uint32 Id = 0;
        uint32 HubId = 0;
        std::string Name;
        std::string Category;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float Radius = 3.0f;
        uint32 Weight = 100;
        CityPoiType PoiType = CityPoiType::Plaza;
        std::string District;
        uint32 PartnerSpotId = 0;
    };

    struct Hub
    {
        uint32 Id = 0;
        std::string Name;
        bool Enabled = true;
        TeamSide Team = TeamSide::Any;
        uint8 MinLevel = 1;
        uint8 MaxLevel = 80;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        uint32 DefaultPopulation = 0;
        uint32 ConfiguredPopulation = 0;
        uint32 MinCount = 0;
        uint32 Priority = 100;
        bool UseTimeScaling = true;
        uint32 ResidenceSeconds = 0;
        bool DuelHub = false; // gate/Goldshire duel ground: every resident is a spectator
        bool SocialHub = false;
        std::vector<Spot> Spots;
    };

    struct BotCandidate
    {
        uint32 GuidLow = 0;
        uint32 AccountId = 0;
        std::string Name;
        uint8 Level = 1;
        uint8 Race = 0;
        TeamSide Team = TeamSide::Any;
    };

    struct Resident
    {
        BotCandidate Bot;
        uint32 HubId = 0;
        uint32 SpotId = 0;
        uint32 TargetMap = 0;
        uint32 OriginalMap = 0;
        float OriginalX = 0.0f;
        float OriginalY = 0.0f;
        float OriginalZ = 0.0f;
        float OriginalO = 0.0f;
        float TargetX = 0.0f;
        float TargetY = 0.0f;
        float TargetZ = 0.0f;
        float TargetO = 0.0f;
        uint32 NextMoveAt = 0;
        uint32 NextEmoteAtMs = 0;
        uint32 NextReservationRefreshAt = 0;
        uint32 ReleaseAt = 0;
        bool Stationary = false;

        // Unified Activity & Role State
        CitizenRole Role = CitizenRole::Flex;
        CitizenActivity Activity = CitizenActivity::Idle;
        uint32 ActivityStartMs = 0;
        uint32 ActivityDurationMs = 0;
        bool AtSpot = false;
        bool Sitting = false;
        bool AfkSetByModule = false;
        bool DancingSetByModule = false;

        // Navigation, Mounting & Sink Forensics
        // Position at the last sweep; LastSweepZ 0 means no baseline (admission, after a teleport).
        float LastSweepX = 0.0f;
        float LastSweepY = 0.0f;
        float LastSweepZ = 0.0f;
        uint32 LeashGraceUntil = 0; // game seconds: no leash teleport while a walk to a new spot is under way

        // Living Crowd Scene State
        CrowdPhase CrowdPhaseState = CrowdPhase::Linger;
        uint32 CrowdPhaseEndMs = 0;
        uint32 CrowdHomeSpotId = 0;
        uint32 LastCrowdChatMs = 0;
        uint32 ChatPartnerGuid = 0;
        uint32 ChatDeadlineMs = 0;

        // Alternating District Roam State
        bool LastRoamWasHub = false;

        // Idle antics (LifeMotion.h, CityLife.Antics.*); HopDismountRolled: this walk's arrival was rolled
        LifeMotion::Routine Antic;
        bool HopDismountRolled = false;

        // Dropped off out of sight on arrival: walks in to its spot once the teleport has landed
        // (game ms; 0 = no walk pending).
        uint32 ArrivalWalkAtMs = 0;

        // Duel spectator a fight came too close to: the step back it planned, taken at its own moment
        // rather than on the management tick with every other spectator (game ms; 0 = none).
        uint32 SpectatorStepAtMs = 0;
        float SpectatorStepX = 0.0f;
        float SpectatorStepY = 0.0f;
        float SpectatorStepZ = 0.0f;

        // Tokenized Cooperation Lease & State Mutation Journal (LifeBots.h)
        PlayerbotsLife::LeaseToken Token;
        LifeBots::StrategyJournal Journal;
    };
}

#endif
