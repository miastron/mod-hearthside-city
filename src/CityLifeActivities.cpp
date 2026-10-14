/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeActivities.h"
#include "CityLifeMovement.h"
#include "CityLifeMgr.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "NearestGameObjects.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "Item.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ScriptedCreature.h"
#include "GameTime.h"

#include <algorithm>
#include <cmath>

namespace CityLife
{
    namespace Activities
    {
        static inline uint32 GameTimeSeconds()
        {
            return static_cast<uint32>(GameTime::GetGameTime().count());
        }

        Player* FindNearbyRealPlayer(Player* bot, float radius)
        {
            if (!bot || !bot->IsInWorld())
                return nullptr;

            std::list<Player*> players;
            Acore::AnyPlayerInObjectRangeCheck check(bot, radius, /*reqAlive=*/true, /*disallowGM=*/true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
            Cell::VisitObjects(bot, searcher, radius);

            Player* best = nullptr;
            float bestDist = radius;

            for (Player* player : players)
            {
                if (!player || player == bot || !player->IsInWorld() || !IsHumanPlayer(player))
                    continue;

                float dist = bot->GetDistance(player);
                if (dist <= bestDist)
                {
                    best = player;
                    bestDist = dist;
                }
            }

            return best;
        }

        void CrowdClearAfk(Player* bot)
        {
            if (!bot)
                return;
            Resident* resident = sCityLifeMgr.FindResident(bot->GetGUID().GetCounter());
            // Ours to undo while we hold the lease, or once it is gone and no other module holds the bot.
            if (!resident || !resident->AfkSetByModule ||
                (!PlayerbotsLife::ValidateActiveLease(resident->Token) &&
                 !PlayerbotsLife::ValidateLeaseState(resident->Token, PlayerbotsLife::LeaseState::Releasing) &&
                 PlayerbotsLife::IsReserved(resident->Bot.GuidLow)))
                return;
            if (bot->isAFK())
                bot->ToggleAFK();
            resident->AfkSetByModule = false;
        }

        void CrowdReleaseChat(Player* bot, Resident& resident)
        {
            if (resident.ChatPartnerGuid != 0)
            {
                ObjectGuid partnerGuid = ObjectGuid::Create<HighGuid::Player>(resident.ChatPartnerGuid);
                if (ObjectAccessor::FindPlayer(partnerGuid))
                {
                    // Clean partner state through manager's resident list
                    sCityLifeMgr.ClearResidentChatPartner(resident.ChatPartnerGuid, bot->GetGUID().GetCounter());
                }
            }

            resident.ChatPartnerGuid = 0;
            resident.ChatDeadlineMs = 0;
            resident.LastCrowdChatMs = static_cast<uint32>(GameTime::GetGameTimeMS().count());
        }

        void StopDance(Player* bot, Resident& resident)
        {
            if (!resident.DancingSetByModule)
                return;
            resident.DancingSetByModule = false;
            if (bot && bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE)
                bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
        }

        void ExecuteLivingCrowd(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                                float playerRadius, uint32 chatCooldownSec, uint32 afkMinSec,
                                uint32 afkMaxSec, bool enableAfk)
        {
            if (!bot || !bot->IsInWorld())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (!enableAfk && bot->isAFK())
            {
                CrowdClearAfk(bot);
                if (resident.CrowdPhaseState == CrowdPhase::Afk)
                    resident.CrowdPhaseState = CrowdPhase::Linger;
            }

            if (resident.Sitting)
            {
                bot->SetStandState(UNIT_STAND_STATE_STAND);
                resident.Sitting = false;
            }

            // Stale conversation claim auto-release
            if (resident.ChatPartnerGuid != 0 && MsReached(now, resident.ChatDeadlineMs))
            {
                CrowdReleaseChat(bot, resident);
            }

            if (resident.CrowdHomeSpotId == 0)
                resident.CrowdHomeSpotId = resident.SpotId;

            // Proximity Wake Gate: If no real player is near, sleep scene cheaply
            Player* realPlayer = FindNearbyRealPlayer(bot, playerRadius);
            if (!realPlayer)
            {
                CrowdClearAfk(bot);
                resident.CrowdPhaseState = CrowdPhase::Linger;
                resident.CrowdPhaseEndMs = 0;
                return;
            }

            switch (resident.CrowdPhaseState)
            {
                case CrowdPhase::Afk:
                    if (enableAfk && !MsReached(now, resident.CrowdPhaseEndMs))
                        return; // Remain completely still
                    CrowdClearAfk(bot);
                    resident.CrowdPhaseState = CrowdPhase::Linger;
                    resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
                    return;

                case CrowdPhase::WanderOut:
                    if (!MsReached(now, resident.CrowdPhaseEndMs))
                        return; // Dwelling at partner spot
                    // Walk back to home spot
                    if (Spot const* homeSpot = sCityLifeMgr.FindSpot(resident.HubId, resident.CrowdHomeSpotId))
                    {
                        Movement::MoveToPosition(bot, hub.MapId, homeSpot->X, homeSpot->Y, homeSpot->Z, homeSpot->O, false);
                    }
                    resident.CrowdPhaseState = CrowdPhase::WanderBack;
                    resident.CrowdPhaseEndMs = now + 30 * 1000;
                    return;

                case CrowdPhase::WanderBack:
                    if (bot->isMoving() && !MsReached(now, resident.CrowdPhaseEndMs))
                        return;
                    resident.CrowdPhaseState = CrowdPhase::Linger;
                    resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
                    return;

                case CrowdPhase::Linger:
                default:
                    break;
            }

            // Now and then pair up with another lingering resident: the two face each other until the
            // pair's deadline (the stale-claim release above). City says nothing; a CrowdPair event lets
            // mod-hearthside-chat give them a conversation.
            if (resident.ChatPartnerGuid == 0 && now - resident.LastCrowdChatMs >= chatCooldownSec * 1000 &&
                roll_chance_i(25))
            {
                if (Player* partner = sCityLifeMgr.FindCrowdChatPartner(resident.HubId, bot))
                    sCityLifeMgr.BeginCrowdChat(bot, partner, resident, now, realPlayer);
            }

            if (!MsReached(now, resident.CrowdPhaseEndMs))
            {
                // Occasional idle emote while lingering
                if (roll_chance_i(12) && MsReached(now, resident.NextEmoteAtMs))
                {
                    resident.NextEmoteAtMs = now + 25 * 1000;
                    static uint32 const idleEmotes[] = {
                        EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_NO, EMOTE_ONESHOT_POINT,
                        EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_QUESTION
                    };
                    bot->HandleEmoteCommand(idleEmotes[urand(0, 4)]);
                }
                return;
            }

            // Hold linger if in conversation
            if (resident.ChatPartnerGuid != 0)
                return;

            // Transition roll
            uint32 roll = urand(1, 100);
            if (enableAfk && roll <= 15)
            {
                if (!bot->isAFK())
                {
                    bot->ToggleAFK();
                    resident.AfkSetByModule = true;
                }
                resident.CrowdPhaseState = CrowdPhase::Afk;
                uint32 minSec = afkMinSec;
                uint32 maxSec = afkMaxSec < minSec ? minSec : afkMaxSec;
                resident.CrowdPhaseEndMs = now + urand(minSec, maxSec) * 1000;
            }
            else if (roll <= 35)
            {
                // Wander out to the partner spot; without one, to the hub's auction house from a bank
                // (or a bank from anywhere else).
                Spot const* curSpot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
                Spot const* partnerSpot = nullptr;
                if (curSpot && curSpot->PartnerSpotId != 0)
                    partnerSpot = sCityLifeMgr.FindSpot(resident.HubId, curSpot->PartnerSpotId);
                if (!partnerSpot && curSpot)
                {
                    CityPoiType const wanted = curSpot->PoiType == CityPoiType::Bank ?
                        CityPoiType::AuctionHouse : CityPoiType::Bank;
                    for (Spot const& candidate : hub.Spots)
                        if (candidate.PoiType == wanted && candidate.Id != curSpot->Id)
                        {
                            partnerSpot = &candidate;
                            break;
                        }
                }
                if (partnerSpot)
                {
                    Movement::MoveToPosition(bot, hub.MapId, partnerSpot->X, partnerSpot->Y, partnerSpot->Z, partnerSpot->O, false);
                    resident.CrowdPhaseState = CrowdPhase::WanderOut;
                    resident.CrowdPhaseEndMs = now + urand(30, 120) * 1000;
                    return;
                }
                resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
            }
            else
            {
                resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
            }
        }

        // Sits down, on a chair when one is within reach (a bench or the floor otherwise).
        static void SitDown(Player* bot, Resident& resident)
        {
            std::list<GameObject*> gameobjects;
            AnyGameObjectInObjectRangeCheck check(bot, 3.5f);
            Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, gameobjects, check);
            Cell::VisitObjects(bot, searcher, 3.5f);

            bool onChair = false;
            for (GameObject* go : gameobjects)
            {
                if (go && go->GetGoType() == GAMEOBJECT_TYPE_CHAIR)
                {
                    bot->SetFacingToObject(go);
                    onChair = true;
                    break;
                }
            }

            bot->SetStandState(onChair ? UNIT_STAND_STATE_SIT_CHAIR : UNIT_STAND_STATE_SIT);
            resident.Sitting = true;
        }

        static void StandUp(Player* bot, Resident& resident)
        {
            if (resident.Sitting || bot->getStandState() != UNIT_STAND_STATE_STAND)
            {
                bot->SetStandState(UNIT_STAND_STATE_STAND);
                resident.Sitting = false;
            }
        }

        void ExecuteDuelSpectator(Resident& resident, Player* bot, Hub const& hub, uint32 now, float watchRadius)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);
            StandUp(bot, resident);

            // The nearest duel in progress. The duels are mod-hearthside-pvp's (or anyone's); this
            // module only watches them.
            std::list<Player*> players;
            Acore::AnyPlayerInObjectRangeCheck check(bot, watchRadius, /*reqAlive=*/true, /*disallowGM=*/true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
            Cell::VisitObjects(bot, searcher, watchRadius);

            Player* duelist = nullptr;
            float bestDist = watchRadius;
            for (Player* player : players)
            {
                if (!player || player == bot || !player->duel || !player->duel->Opponent ||
                    player->duel->State != DUEL_STATE_IN_PROGRESS)
                    continue;
                float dist = bot->GetDistance(player);
                if (dist < bestDist)
                {
                    duelist = player;
                    bestDist = dist;
                }
            }

            if (!duelist)
            {
                // Nothing to watch: wait at the stand, now and then chatting with a neighbour.
                if (MsReached(now, resident.NextEmoteAtMs))
                {
                    resident.NextEmoteAtMs = now + urand(20, 45) * 1000;
                    if (roll_chance_i(30))
                        bot->HandleEmoteCommand(roll_chance_i(50) ? EMOTE_ONESHOT_TALK : EMOTE_ONESHOT_POINT);
                }
                return;
            }

            Player* opponent = duelist->duel->Opponent;
            float const midX = (duelist->GetPositionX() + opponent->GetPositionX()) * 0.5f;
            float const midY = (duelist->GetPositionY() + opponent->GetPositionY()) * 0.5f;
            float const toBotX = bot->GetPositionX() - midX;
            float const toBotY = bot->GetPositionY() - midY;
            float const distToFight = std::sqrt(toBotX * toBotX + toBotY * toBotY);

            // Standing in the fight: back off to a ring around it and make that the new stand.
            constexpr float MinWatchDistance = 8.0f;
            constexpr float WatchDistance = 12.0f;
            if (distToFight < MinWatchDistance)
            {
                float const angle = distToFight > 0.5f ? std::atan2(toBotY, toBotX) : frand(0.0f, 6.28318531f);
                float x = midX + std::cos(angle) * WatchDistance;
                float y = midY + std::sin(angle) * WatchDistance;
                float z = bot->GetPositionZ();
                bot->UpdateAllowedPositionZ(x, y, z);
                if (Movement::ValidateCityDestination(bot, hub.MapId, x, y, z, 0.0f))
                {
                    resident.TargetX = x;
                    resident.TargetY = y;
                    resident.TargetZ = z;
                    resident.AtSpot = false;
                    resident.LeashGraceUntil = GameTimeSeconds() + 15;
                    sCityLifeMgr.SetStayPosition(bot, resident);
                    Movement::MoveToPosition(bot, hub.MapId, x, y, z, 0.0f, false);
                    return;
                }
            }

            bot->SetFacingTo(bot->GetAbsoluteAngle(midX, midY));

            if (MsReached(now, resident.NextEmoteAtMs))
            {
                resident.NextEmoteAtMs = now + urand(6, 15) * 1000;
                static uint32 const cheers[] = {
                    EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_LAUGH
                };
                if (roll_chance_i(60))
                    bot->HandleEmoteCommand(cheers[urand(0, 3)]);
            }
        }

        void ExecuteFishing(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (bot->GetMapId() != hub.MapId || (hub.ZoneId != 0 && bot->GetZoneId() != hub.ZoneId))
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);

            // Resting by the water between spells of fishing (MaintainResidents alternates the two).
            if (resident.Sitting)
                return;

            StandUp(bot, resident);

            Spot const* spot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (spot && std::fabs(bot->GetOrientation() - spot->O) > 0.05f)
                bot->SetFacingTo(spot->O);

            if (bot->IsNonMeleeSpellCast(false))
                return;

            // A bot that fishes (pole in hand, the skill learned) casts its highest Fishing rank: the
            // spell chain of 7620 -- Grand Master, Master, Artisan, Expert, Journeyman, Apprentice.
            Item* mainItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            if (bot->GetSkillValue(SKILL_FISHING) > 0 && mainItem && mainItem->GetTemplate() &&
                mainItem->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
                mainItem->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE)
            {
                static uint32 const fishingRanks[] = { 51294, 33095, 18248, 7732, 7731, 7620 };
                for (uint32 rank : fishingRanks)
                    if (bot->HasSpell(rank))
                    {
                        bot->CastSpell(bot, rank, false);
                        return;
                    }
            }

            // Anyone else plays the fishing animation: nothing learned, equipped or looted.
            if (MsReached(now, resident.NextEmoteAtMs))
            {
                bot->HandleEmoteCommand(roll_chance_i(30) ? EMOTE_ONESHOT_FISHING : EMOTE_STATE_FISHING);
                resident.NextEmoteAtMs = now + urand(10, 20) * 1000;
            }
        }

        void ExecuteInnDance(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);

            // Resting between sets (MaintainResidents alternates dancing and sitting).
            if (resident.Sitting)
                return;

            StandUp(bot, resident);

            // A state emote, held through UNIT_NPC_EMOTESTATE; StopDance clears it.
            if (bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) != EMOTE_STATE_DANCE)
            {
                bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DANCE);
                resident.DancingSetByModule = true;
            }
        }

        void ExecutePlazaSocial(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            StandUp(bot, resident);

            // Another bot standing in the square: turn to it and wave or cheer.
            std::list<Player*> players;
            Acore::AnyPlayerInObjectRangeCheck check(bot, 15.0f, /*reqAlive=*/true, /*disallowGM=*/true);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(bot, players, check);
            Cell::VisitObjects(bot, searcher, 15.0f);

            Player* neighbour = nullptr;
            float bestDist = 15.0f;
            for (Player* player : players)
            {
                if (!player || player == bot || !IsBotSession(player))
                    continue;
                float dist = bot->GetDistance(player);
                if (dist < bestDist)
                {
                    neighbour = player;
                    bestDist = dist;
                }
            }

            if (neighbour && roll_chance_i(40))
            {
                bot->SetFacingToObject(neighbour);
                bot->HandleEmoteCommand(roll_chance_i(50) ? EMOTE_ONESHOT_WAVE : EMOTE_ONESHOT_CHEER);
                return;
            }

            if (roll_chance_i(35))
            {
                static uint32 const emotes[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_LAUGH };
                bot->HandleEmoteCommand(emotes[urand(0, 2)]);
            }
        }

        void ExecuteTavernSitting(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (!resident.Sitting)
            {
                SitDown(bot, resident);
                if (hub.SocialHub && roll_chance_i(35))
                    bot->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
            }
            else if (roll_chance_i(15))
            {
                bot->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
            }
        }

        void ExecuteTavernDrinking(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (!resident.Sitting)
                SitDown(bot, resident);

            if (roll_chance_i(35))
                bot->HandleEmoteCommand(EMOTE_ONESHOT_EAT_NO_SHEATHE);
            else if (roll_chance_i(25))
                bot->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);
        }

        // A trainer for the bot's own class: its subname names the class ("Warrior Trainer").
        static bool TrainsClass(Creature const* creature, uint8 botClass)
        {
            CreatureTemplate const* info = creature->GetCreatureTemplate();
            if (!info)
                return false;

            std::string sub = info->SubName;
            std::transform(sub.begin(), sub.end(), sub.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            switch (botClass)
            {
                case CLASS_WARRIOR:      return sub.find("warrior") != std::string::npos;
                case CLASS_PALADIN:      return sub.find("paladin") != std::string::npos;
                case CLASS_HUNTER:       return sub.find("hunter") != std::string::npos;
                case CLASS_ROGUE:        return sub.find("rogue") != std::string::npos;
                case CLASS_PRIEST:       return sub.find("priest") != std::string::npos;
                case CLASS_DEATH_KNIGHT: return sub.find("death knight") != std::string::npos;
                case CLASS_SHAMAN:       return sub.find("shaman") != std::string::npos;
                case CLASS_MAGE:         return sub.find("mage") != std::string::npos;
                case CLASS_WARLOCK:      return sub.find("warlock") != std::string::npos;
                case CLASS_DRUID:        return sub.find("druid") != std::string::npos;
                default:                 return false;
            }
        }

        void ExecuteVisitNpc(Resident& resident, Player* bot, Hub const& hub, uint32 now, uint32 npcFlag)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);
            StandUp(bot, resident);

            // A nearby NPC that offers the service; at a trainer spot the bot's own class trainer
            // comes first, any trainer otherwise.
            std::list<Creature*> creatures;
            Acore::AnyUnitInObjectRangeCheck check(bot, 25.0f);
            Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, creatures, check);
            Cell::VisitObjects(bot, searcher, 25.0f);

            Creature* target = nullptr;
            for (Creature* creature : creatures)
            {
                if (!creature || !creature->IsAlive())
                    continue;

                bool matches = false;
                if (npcFlag == UNIT_NPC_FLAG_TRAINER)
                    matches = creature->IsTrainer();
                else if (npcFlag == UNIT_NPC_FLAG_VENDOR)
                    matches = creature->IsVendor() || creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR_FOOD);
                else
                    matches = creature->HasNpcFlag(static_cast<NPCFlags>(npcFlag));
                if (!matches)
                    continue;

                if (npcFlag == UNIT_NPC_FLAG_TRAINER && TrainsClass(creature, bot->getClass()))
                {
                    target = creature;
                    break;
                }
                if (!target)
                    target = creature;
            }

            if (!target)
                return;

            bot->SetFacingToObject(target);
            // Gossip needs interaction range; from further off the visit is just facing the NPC.
            if (bot->IsWithinDistInMap(target, INTERACTION_DISTANCE))
            {
                WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
                packet << target->GetGUID();
                bot->GetSession()->HandleGossipHelloOpcode(packet);
            }

            if (MsReached(now, resident.NextEmoteAtMs))
            {
                resident.NextEmoteAtMs = now + urand(10, 20) * 1000;
                static uint32 const npcEmotes[] = {
                    EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION,
                    EMOTE_ONESHOT_YES, EMOTE_ONESHOT_SALUTE, EMOTE_ONESHOT_BOW
                };
                bot->HandleEmoteCommand(npcEmotes[urand(0, 4)]);
            }
        }

        void ExecuteVisitMailbox(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);
            StandUp(bot, resident);

            std::list<GameObject*> gameobjects;
            AnyGameObjectInObjectRangeCheck check(bot, 20.0f);
            Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, gameobjects, check);
            Cell::VisitObjects(bot, searcher, 20.0f);

            for (GameObject* go : gameobjects)
            {
                if (go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                {
                    bot->SetFacingToObject(go);
                    if (bot->IsWithinDistInMap(go, INTERACTION_DISTANCE))
                        go->Use(bot);
                    if (MsReached(now, resident.NextEmoteAtMs))
                    {
                        resident.NextEmoteAtMs = now + urand(8, 16) * 1000;
                        static uint32 const mailEmotes[] = {
                            EMOTE_STATE_USE_STANDING, EMOTE_ONESHOT_LOOT,
                            EMOTE_STATE_USE_STANDING_NO_SHEATHE, EMOTE_ONESHOT_YES
                        };
                        bot->HandleEmoteCommand(mailEmotes[urand(0, 3)]);
                    }
                    break;
                }
            }
        }

        void ExecuteBuyFood(Resident& resident, Player* bot, Hub const& hub, uint32 now, bool enableInteractions)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (enableInteractions)
            {
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    ai->DoSpecificAction("rpg buy", Event("rpg action", "vendor"), true);
                    return;
                }
            }

            // Visual visit: the vendor's gossip, then a bite or a word.
            ExecuteVisitNpc(resident, bot, hub, now, UNIT_NPC_FLAG_VENDOR);
            if (roll_chance_i(30))
                bot->HandleEmoteCommand(roll_chance_i(50) ? EMOTE_ONESHOT_EAT_NO_SHEATHE : EMOTE_ONESHOT_TALK);
        }

        void ExecuteWallPatrol(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Spot const* spot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (!spot)
                return;

            if (!Movement::IsAtSpot(bot, *spot, 4.0f))
            {
                if (!bot->isMoving())
                    Movement::MoveToPosition(bot, hub.MapId, spot->X, spot->Y, spot->Z, spot->O, false);
                return;
            }

            // About twenty seconds at each post. (NextMoveAt belongs to the roaming relocation, which
            // takes the bot off the walls altogether.)
            if (now - resident.ActivityStartMs < 20 * IN_MILLISECONDS)
                return;

            // On to the next post: the spot's partner, or any other wall spot of the hub.
            Spot const* next = spot->PartnerSpotId ? sCityLifeMgr.FindSpot(resident.HubId, spot->PartnerSpotId) : nullptr;
            if (!next)
                for (Spot const& candidate : hub.Spots)
                    if (candidate.PoiType == CityPoiType::WallPatrol && candidate.Id != spot->Id)
                    {
                        next = &candidate;
                        break;
                    }
            if (!next)
                return;

            sCityLifeMgr.AssignSpot(resident, hub, *next, false);
            resident.Activity = CitizenActivity::WalkCityWalls;
            resident.LeashGraceUntil = GameTimeSeconds() + std::min<uint32>(120, 15 +
                static_cast<uint32>(bot->GetDistance2d(resident.TargetX, resident.TargetY) / 4.0f));
            sCityLifeMgr.SetStayPosition(bot, resident);
            Movement::MoveToPosition(bot, hub.MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, false);
        }

        void ExecuteReleaseCycle(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Spot const* spot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (!spot)
                return;

            if (!Movement::IsAtSpot(bot, *spot, std::max(3.5f, spot->Radius + 1.0f)))
            {
                if (!bot->isMoving())
                    Movement::MoveToPosition(bot, hub.MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, false);
                return;
            }

            Movement::DismountIfMounted(bot);

            // Asleep in the inn until RemoveUnavailableResidents releases the bot.
            if (bot->getStandState() != UNIT_STAND_STATE_SLEEP)
                bot->SetStandState(UNIT_STAND_STATE_SLEEP);
        }

        void ExecuteRandomEmote(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            static uint32 const emotes[] = {
                EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_BOW,
                EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_APPLAUD
            };

            bot->HandleEmoteCommand(emotes[urand(0, 5)]);
        }
    }
}
