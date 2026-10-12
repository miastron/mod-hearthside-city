/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeActivities.h"
#include "CityLifeDialogues.h"
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
                if (!player || player == bot || !player->IsInWorld() || !::IsRealPlayer(player))
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
            if (!resident || !resident->AfkSetByModule ||
                (!PlayerbotsLife::ValidateActiveLease(resident->Token) &&
                 !PlayerbotsLife::ValidateLeaseState(resident->Token, PlayerbotsLife::LeaseState::Releasing)))
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
                if (Player* partner = ObjectAccessor::FindPlayer(partnerGuid))
                {
                    // Clean partner state through manager's resident list
                    sCityLifeMgr.ClearResidentChatPartner(resident.ChatPartnerGuid, bot->GetGUID().GetCounter());
                }
            }

            resident.ChatPartnerGuid = 0;
            resident.ChatInitiator = false;
            resident.ChatDialogueId = -1;
            resident.ChatLineIdx = 0;
            resident.ChatDeadlineMs = 0;
            resident.LastCrowdChatMs = static_cast<uint32>(GameTime::GetGameTimeMS().count());
        }

        void ExecuteLivingCrowd(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                                bool enableSay, uint32 sayCooldownSec, float playerRadius,
                                uint32 chatCooldownSec, uint32 afkMinSec, uint32 afkMaxSec,
                                bool enableAfk)
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
            if (resident.ChatPartnerGuid != 0 && now >= resident.ChatDeadlineMs)
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
                    if (enableAfk && now < resident.CrowdPhaseEndMs)
                        return; // Remain completely still
                    CrowdClearAfk(bot);
                    resident.CrowdPhaseState = CrowdPhase::Linger;
                    resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
                    return;

                case CrowdPhase::WanderOut:
                    if (now < resident.CrowdPhaseEndMs)
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
                    if (bot->isMoving() && now < resident.CrowdPhaseEndMs)
                        return;
                    resident.CrowdPhaseState = CrowdPhase::Linger;
                    resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
                    return;

                case CrowdPhase::Linger:
                default:
                    break;
            }

            // Dialogue delivery & initiator state
            if (!enableSay)
            {
                if (resident.ChatPartnerGuid != 0)
                    CrowdReleaseChat(bot, resident);
            }
            else
            {
                if (resident.ChatInitiator && resident.ChatDialogueId >= 0 &&
                    resident.ChatDialogueId < Dialogues::kCrowdDialogueCount)
                {
                    if (now >= resident.ChatDeadlineMs)
                    {
                        CrowdReleaseChat(bot, resident);
                    }
                    else
                    {
                        Dialogues::DialogueEntry const& d = Dialogues::kCrowdDialogues[resident.ChatDialogueId];
                        if (resident.ChatLineIdx < d.LineCount &&
                            now >= resident.LastSayMs + urand(3000, 6000))
                        {
                            bool const partnerLine = (resident.ChatLineIdx % 2) == 1;
                            Player* speaker = bot;
                            if (partnerLine)
                            {
                                ObjectGuid partnerGuid = ObjectGuid::Create<HighGuid::Player>(resident.ChatPartnerGuid);
                                speaker = ObjectAccessor::FindPlayer(partnerGuid);
                                if (!speaker || !speaker->IsInWorld() || bot->GetDistance(speaker) > 20.0f)
                                {
                                    CrowdReleaseChat(bot, resident);
                                    return;
                                }
                            }

                            speaker->Say(d.Lines[resident.ChatLineIdx], LANG_UNIVERSAL);
                            resident.LastSayMs = now;
                            ++resident.ChatLineIdx;

                            if (resident.ChatLineIdx >= d.LineCount)
                                CrowdReleaseChat(bot, resident);
                        }
                    }
                }
                else if (resident.ChatPartnerGuid == 0 &&
                         now >= resident.LastCrowdChatMs + chatCooldownSec * 1000 &&
                         roll_chance_i(25))
                {
                    // Scan for eligible partner in linger
                    Player* partner = sCityLifeMgr.FindCrowdChatPartner(resident.HubId, bot);
                    if (partner)
                    {
                        sCityLifeMgr.BeginCrowdChat(bot, partner, resident, now);
                    }
                }
            }

            if (now < resident.CrowdPhaseEndMs)
            {
                // Occasional idle emote while lingering
                if (roll_chance_i(12) && now >= resident.NextEmoteAtMs)
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
                // Wander out to partner spot
                Spot const* curSpot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
                if (curSpot && curSpot->PartnerSpotId != 0)
                {
                    if (Spot const* partnerSpot = sCityLifeMgr.FindSpot(resident.HubId, curSpot->PartnerSpotId))
                    {
                        Movement::MoveToPosition(bot, hub.MapId, partnerSpot->X, partnerSpot->Y, partnerSpot->Z, partnerSpot->O, false);
                        resident.CrowdPhaseState = CrowdPhase::WanderOut;
                        resident.CrowdPhaseEndMs = now + urand(30, 120) * 1000;
                        return;
                    }
                }
                resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
            }
            else
            {
                resident.CrowdPhaseEndMs = now + urand(20, 90) * 1000;
            }
        }

        bool IsRangedDueler(Player* bot)
        {
            if (!bot)
                return false;

            switch (bot->getClass())
            {
                case CLASS_HUNTER:
                case CLASS_MAGE:
                case CLASS_PRIEST:
                case CLASS_WARLOCK:
                    return true;
                default:
                    return false;
            }
        }

        bool IsMeleeDueler(Player* bot)
        {
            if (!bot || IsRangedDueler(bot))
                return false;

            return true;
        }

        void ComputeDuelMeetPoint(Player* bot, Player* opponent, Spot const& anchorSpot,
                                  float maxDistance, float& meetX, float& meetY, float& meetZ)
        {
            float const anchorX = anchorSpot.X;
            float const anchorY = anchorSpot.Y;
            float const anchorZ = anchorSpot.Z;

            meetX = (bot->GetPositionX() + opponent->GetPositionX()) * 0.5f;
            meetY = (bot->GetPositionY() + opponent->GetPositionY()) * 0.5f;
            meetZ = (bot->GetPositionZ() + opponent->GetPositionZ()) * 0.5f;

            float const dx = meetX - anchorX;
            float const dy = meetY - anchorY;
            float const distFromAnchor = std::sqrt(dx * dx + dy * dy);
            if (distFromAnchor > maxDistance && distFromAnchor > 0.1f)
            {
                float const scale = maxDistance / distFromAnchor;
                meetX = anchorX + dx * scale;
                meetY = anchorY + dy * scale;
                meetZ = anchorZ;
            }

            bot->UpdateAllowedPositionZ(meetX, meetY, meetZ);
        }

        void ComputeDuelStandPoint(Player* bot, Player* opponent, float meetX, float meetY,
                                   float meetZ, float& standX, float& standY, float& standZ)
        {
            standX = meetX;
            standY = meetY;
            standZ = meetZ;
            if (!bot || !opponent)
                return;

            bool const botIsLowerGuid =
                bot->GetGUID().GetCounter() < opponent->GetGUID().GetCounter();
            float dx = botIsLowerGuid
                ? opponent->GetPositionX() - bot->GetPositionX()
                : bot->GetPositionX() - opponent->GetPositionX();
            float dy = botIsLowerGuid
                ? opponent->GetPositionY() - bot->GetPositionY()
                : bot->GetPositionY() - opponent->GetPositionY();
            if (std::fabs(dx) < 0.1f && std::fabs(dy) < 0.1f)
            {
                dx = 1.0f;
                dy = 0.0f;
            }

            float const angle = std::atan2(dy, dx) + static_cast<float>(M_PI) * 0.5f;
            float const side = botIsLowerGuid ? 1.0f : -1.0f;
            // Approach within 2 yards of center (4 yards separation) so bots are safely within
            // the 10-yard legal range of spell 7266 (Duel). Tactical ranged spacing (14-17yd)
            // is deferred until countdown and active combat phases.
            float const offset = 2.0f;

            standX += std::cos(angle) * offset * side;
            standY += std::sin(angle) * offset * side;
            bot->UpdateAllowedPositionZ(standX, standY, standZ);
        }

        bool MoveToRangedDuelDistance(Player* bot, Resident& resident, Player* opponent,
                                      Spot const* anchorSpot, float leashRadius)
        {
            if (!bot || !opponent || !IsRangedDueler(bot) || bot->IsBeingTeleported() ||
                bot->IsNonMeleeSpellCast(false) ||
                bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
                return false;

            float const currentDistance = bot->GetDistance(opponent);
            if (currentDistance >= 11.0f && currentDistance <= 20.0f)
                return false;

            float const preferredDistance = bot->getClass() == CLASS_HUNTER ? 16.0f : 14.0f;
            float dx = bot->GetPositionX() - opponent->GetPositionX();
            float dy = bot->GetPositionY() - opponent->GetPositionY();
            float len = std::sqrt(dx * dx + dy * dy);
            if (len < 0.1f)
            {
                float const angle = frand(0.0f, static_cast<float>(2.0 * M_PI));
                dx = std::cos(angle);
                dy = std::sin(angle);
                len = 1.0f;
            }

            float targetX = opponent->GetPositionX() + (dx / len) * preferredDistance;
            float targetY = opponent->GetPositionY() + (dy / len) * preferredDistance;
            float targetZ = opponent->GetPositionZ();

            if (anchorSpot)
            {
                float const ax = targetX - anchorSpot->X;
                float const ay = targetY - anchorSpot->Y;
                float const distFromAnchor = std::sqrt(ax * ax + ay * ay);
                if (distFromAnchor > leashRadius && distFromAnchor > 0.1f)
                {
                    float const scale = leashRadius / distFromAnchor;
                    targetX = anchorSpot->X + ax * scale;
                    targetY = anchorSpot->Y + ay * scale;
                    targetZ = anchorSpot->Z;
                }
            }

            bot->UpdateAllowedPositionZ(targetX, targetY, targetZ);
            Movement::DismountIfMounted(bot);
            bot->GetMotionMaster()->MovePoint(0, targetX, targetY, targetZ);
            return true;
        }

        void CancelCityDuel(Player* bot, char const* reason)
        {
            Resident* resident = bot ? sCityLifeMgr.FindResident(bot->GetGUID().GetCounter()) : nullptr;
            if (!resident || !bot->duel || !bot->duel->Opponent || !resident->CoreDuelArbiter ||
                bot->GetGuidValue(PLAYER_DUEL_ARBITER).GetRawValue() != resident->CoreDuelArbiter ||
                bot->duel->Opponent->GetGUID().GetCounter() != resident->DuelPartnerGuid ||
                (!PlayerbotsLife::ValidateActiveLease(resident->Token) &&
                 !PlayerbotsLife::ValidateLeaseState(resident->Token, PlayerbotsLife::LeaseState::Releasing)))
                return;
            bot->DuelComplete(DUEL_INTERRUPTED);
            resident->DuelState = DuelPairState::None;
            resident->CoreDuelArbiter = 0;
            LOG_DEBUG("module", "[CityLife] Canceled owned duel for {}: {}", bot->GetName(), reason);
        }

        void DriveActiveDuel(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                             float leashRadius, bool tacticalRange)
        {
            (void)hub;
            if (!bot || !bot->IsInWorld() || !bot->duel)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Player* opponent = bot->duel->Opponent;
            if (!opponent || !opponent->IsInWorld() || !bot->IsAlive() || !opponent->IsAlive())
            {
                CancelCityDuel(bot, "missing opponent or death");
                return;
            }

            uint32 opponentGuid = opponent->GetGUID().GetCounter();
            if (resident.DuelPartnerGuid && resident.DuelPartnerGuid != opponentGuid)
                return;
            if (!resident.DuelPartnerGuid)
            {
                if (!::IsRealPlayer(opponent) || bot == bot->duel->Initiator)
                    return;
                resident.DuelPartnerGuid = opponentGuid;
            }
            uint64 arbiter = bot->GetGuidValue(PLAYER_DUEL_ARBITER).GetRawValue();
            if (resident.CoreDuelArbiter && resident.CoreDuelArbiter != arbiter)
                return;
            resident.CoreDuelArbiter = arbiter;

            Spot const* anchor = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (anchor)
            {
                float distA = bot->GetDistance2d(anchor->X, anchor->Y);
                float distB = opponent->GetDistance2d(anchor->X, anchor->Y);
                if (distA > leashRadius + 15.0f || distB > leashRadius + 15.0f)
                {
                    CancelCityDuel(bot, "out of duel boundary");
                    return;
                }
            }

            // Throttle macro-positioning to at most once per 1000ms
            if (now < resident.DuelLastRepositionMs + 1000)
                return;

            resident.DuelLastRepositionMs = now;

            if (bot->duel->State == DUEL_STATE_CHALLENGED)
            {
                resident.DuelState = DuelPairState::Challenged;
                if (bot != bot->duel->Initiator && bot->GetSession())
                {
                    if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                        return;
                    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
                    packet << bot->GetGuidValue(PLAYER_DUEL_ARBITER);
                    bot->GetSession()->HandleDuelAcceptedOpcode(packet);
                }
                return;
            }

            if (bot->duel->State == DUEL_STATE_COUNTDOWN)
            {
                resident.DuelState = DuelPairState::Countdown;
                if (tacticalRange && IsRangedDueler(bot))
                    MoveToRangedDuelDistance(bot, resident, opponent, anchor, leashRadius);
                return;
            }

            if (bot->duel->State == DUEL_STATE_IN_PROGRESS)
            {
                resident.DuelState = DuelPairState::InProgress;
                float dist = bot->GetDistance(opponent);

                if (!tacticalRange)
                    return;

                // Check line of sight
                if (!bot->IsWithinLOSInMap(opponent))
                {
                    bot->GetMotionMaster()->MovePoint(0, opponent->GetPositionX(), opponent->GetPositionY(), opponent->GetPositionZ());
                    return;
                }

                if (IsRangedDueler(bot))
                {
                    if (dist < 11.0f)
                    {
                        MoveToRangedDuelDistance(bot, resident, opponent, anchor, leashRadius);
                    }
                }
                else if (IsMeleeDueler(bot))
                {
                    if (dist > 5.5f && !bot->IsNonMeleeSpellCast(false) &&
                        !bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
                    {
                        bot->GetMotionMaster()->MovePoint(0, opponent->GetPositionX(), opponent->GetPositionY(), opponent->GetPositionZ());
                    }
                }
            }
        }

        void ExecuteDuel(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                         uint32 duelCooldownSec, float duelMaxDist, bool meetingPointEnabled)
        {
            if (!bot || !bot->IsInWorld())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (bot->duel || bot->IsInCombat())
                return;

            // Cooldown check
            if (now < resident.DuelCooldownUntilMs)
            {
                // Occasional flourish or sit while on cooldown
                if (roll_chance_i(10) && !bot->isMoving())
                    bot->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
                return;
            }

            // Post-duel recovery if state was flagged and not waiting for meeting
            if (resident.DuelPartnerGuid != 0 && !resident.DuelMeetingSet)
            {
                if (resident.DuelStagingSlotId != 0)
                {
                    PlayerbotsLife::ReleaseDuelStagingSlot(resident.DuelStagingSlotId, PlayerbotsLife::ReservationOwner::CityLife);
                    resident.DuelStagingSlotId = 0;
                }
                if (resident.DuelPairId != 0)
                {
                    PlayerbotsLife::ReleaseDuelStagingSlotByPair(resident.DuelPairId, PlayerbotsLife::ReservationOwner::CityLife);
                    resident.DuelPairId = 0;
                }
                // Native duel completion owns combat and resource recovery.
                resident.DuelPartnerGuid = 0;
                resident.CoreDuelArbiter = 0;
                resident.DuelState = DuelPairState::None;
                resident.DuelCooldownUntilMs = now + duelCooldownSec * 1000;
                return;
            }

            // Find duel opponent within duel ring
            Spot const* spot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (!spot)
                return;

            // Rendezvous state handling
            if (resident.DuelMeetingSet)
            {
                // Timeout check (15 seconds)
                if (now >= resident.DuelRendezvousDeadlineMs)
                {
                    if (resident.DuelStagingSlotId != 0)
                    {
                        PlayerbotsLife::ReleaseDuelStagingSlot(resident.DuelStagingSlotId, PlayerbotsLife::ReservationOwner::CityLife);
                        resident.DuelStagingSlotId = 0;
                    }
                    if (resident.DuelPairId != 0)
                    {
                        PlayerbotsLife::ReleaseDuelStagingSlotByPair(resident.DuelPairId, PlayerbotsLife::ReservationOwner::CityLife);
                        resident.DuelPairId = 0;
                    }
                    resident.DuelMeetingSet = false;
                    resident.DuelPartnerGuid = 0;
                    resident.DuelState = DuelPairState::None;
                    resident.DuelCooldownUntilMs = now + 10 * 1000;
                    return;
                }

                ObjectGuid oppGuid = ObjectGuid::Create<HighGuid::Player>(resident.DuelPartnerGuid);
                Player* opponent = ObjectAccessor::FindPlayer(oppGuid);
                if (!opponent || !opponent->IsInWorld() || opponent->duel || opponent->IsInCombat())
                {
                    if (resident.DuelStagingSlotId != 0)
                    {
                        PlayerbotsLife::ReleaseDuelStagingSlot(resident.DuelStagingSlotId, PlayerbotsLife::ReservationOwner::CityLife);
                        resident.DuelStagingSlotId = 0;
                    }
                    if (resident.DuelPairId != 0)
                    {
                        PlayerbotsLife::ReleaseDuelStagingSlotByPair(resident.DuelPairId, PlayerbotsLife::ReservationOwner::CityLife);
                        resident.DuelPairId = 0;
                    }
                    resident.DuelMeetingSet = false;
                    resident.DuelPartnerGuid = 0;
                    resident.DuelState = DuelPairState::None;
                    resident.DuelCooldownUntilMs = now + 10 * 1000;
                    return;
                }

                float const dist = bot->GetDistance(opponent);
                // When within standoff proximity or duel spell range (10 yards), face and challenge
                if (dist <= 10.0f)
                {
                    if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                        return;

                    resident.DuelMeetingSet = false;
                    resident.DuelState = DuelPairState::Challenged;
                    bot->SetFacingToObject(opponent);
                    if (resident.DuelInitiator)
                    {
                        if (::IsRealPlayer(opponent))
                        {
                            uint32 nowSec = GameTimeSeconds();
                            if (!PlayerbotsLife::TryClaimRealPlayerChallenge(resident.DuelPartnerGuid, nowSec, duelCooldownSec))
                                return;

                            if (bot->CastSpell(opponent, 7266, false) != SPELL_CAST_OK)
                                PlayerbotsLife::ReleaseRealPlayerChallengeClaim(resident.DuelPartnerGuid, nowSec + duelCooldownSec);
                        }
                        else
                        {
                            bot->CastSpell(opponent, 7266, false);
                        }
                    }
                }
                else
                {
                    if (!bot->isMoving())
                    {
                        float standX = resident.DuelMeetX;
                        float standY = resident.DuelMeetY;
                        float standZ = resident.DuelMeetZ;
                        ComputeDuelStandPoint(bot, opponent, resident.DuelMeetX, resident.DuelMeetY, resident.DuelMeetZ,
                                              standX, standY, standZ);
                        Movement::MoveToPosition(bot, hub.MapId, standX, standY, standZ, 0.0f, false);
                    }
                }
                return;
            }

            // Search for eligible duel opponent
            Player* opponent = sCityLifeMgr.FindDuelOpponent(hub, bot, spot->X, spot->Y, 35.0f);
            if (!opponent)
            {
                // Return to duel ring center if strayed
                if (bot->GetDistance2d(spot->X, spot->Y) > duelMaxDist)
                {
                    Movement::MoveToPosition(bot, hub.MapId, spot->X, spot->Y, spot->Z, spot->O, false);
                }
                return;
            }

            uint32 oppGuidLow = opponent->GetGUID().GetCounter();
            resident.DuelPartnerGuid = oppGuidLow;
            resident.DuelInitiator = true;

            if (meetingPointEnabled)
            {
                // Calculate mutual meeting point
                float meetX = spot->X, meetY = spot->Y, meetZ = spot->Z;
                ComputeDuelMeetPoint(bot, opponent, *spot, duelMaxDist, meetX, meetY, meetZ);

                uint64 pairId = (static_cast<uint64>(std::min(bot->GetGUID().GetCounter(), oppGuidLow)) << 32) |
                                 std::max(bot->GetGUID().GetCounter(), oppGuidLow);

                PlayerbotsLife::DuelStagingSlot slot;
                if (!PlayerbotsLife::ClaimDuelStagingSlot(hub.MapId, meetX, meetY, meetZ, duelMaxDist,
                    resident.Token.ActivityId, PlayerbotsLife::ReservationOwner::CityLife, now, 300000, slot, pairId))
                {
                    // No valid staging space available (spacing collision), wait and retry later
                    resident.DuelPartnerGuid = 0;
                    resident.DuelInitiator = false;
                    resident.DuelCooldownUntilMs = now + 4000;
                    return;
                }

                meetX = slot.CenterX;
                meetY = slot.CenterY;
                resident.DuelStagingSlotId = slot.SlotId;
                resident.DuelPairId = pairId;

                resident.DuelMeetX = meetX;
                resident.DuelMeetY = meetY;
                resident.DuelMeetZ = meetZ;
                resident.DuelMeetingSet = true;
                resident.DuelState = DuelPairState::Rendezvous;
                resident.DuelRendezvousDeadlineMs = now + 15 * 1000;

                // If opponent is also a resident bot, synchronize rendezvous
                if (Resident* oppResident = sCityLifeMgr.FindResident(oppGuidLow); oppResident && PlayerbotsLife::ValidateActiveLease(oppResident->Token))
                {
                    oppResident->DuelPartnerGuid = bot->GetGUID().GetCounter();
                    oppResident->DuelInitiator = false;
                    oppResident->DuelPairId = pairId;
                    oppResident->DuelMeetX = meetX;
                    oppResident->DuelMeetY = meetY;
                    oppResident->DuelMeetZ = meetZ;
                    oppResident->DuelMeetingSet = true;
                    oppResident->DuelState = DuelPairState::Rendezvous;
                    oppResident->DuelRendezvousDeadlineMs = resident.DuelRendezvousDeadlineMs;
                    oppResident->DuelStagingSlotId = slot.SlotId;

                    float oppStandX = meetX, oppStandY = meetY, oppStandZ = meetZ;
                    ComputeDuelStandPoint(opponent, bot, meetX, meetY, meetZ, oppStandX, oppStandY, oppStandZ);
                    Movement::MoveToPosition(opponent, hub.MapId, oppStandX, oppStandY, oppStandZ, 0.0f, false);
                }

                float standX = meetX, standY = meetY, standZ = meetZ;
                ComputeDuelStandPoint(bot, opponent, meetX, meetY, meetZ, standX, standY, standZ);
                Movement::MoveToPosition(bot, hub.MapId, standX, standY, standZ, 0.0f, false);
            }
            else
            {
                // Direct approach fallback
                if (bot->GetDistance(opponent) > 10.0f)
                {
                    Movement::MoveToPosition(bot, hub.MapId, opponent->GetPositionX(), opponent->GetPositionY(), opponent->GetPositionZ(), 0.0f, false);
                    return;
                }

                if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                    return;

                bot->SetFacingToObject(opponent);
                resident.DuelState = DuelPairState::Challenged;
                if (::IsRealPlayer(opponent))
                {
                    uint32 nowSec = GameTimeSeconds();
                    if (!PlayerbotsLife::TryClaimRealPlayerChallenge(oppGuidLow, nowSec, duelCooldownSec))
                        return;

                    if (bot->CastSpell(opponent, 7266, false) != SPELL_CAST_OK)
                        PlayerbotsLife::ReleaseRealPlayerChallengeClaim(oppGuidLow, nowSec + duelCooldownSec);
                }
                else
                {
                    bot->CastSpell(opponent, 7266, false);
                }
            }
        }

        void ExecuteDuelSpectator(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving())
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Spot const* spot = sCityLifeMgr.FindSpot(resident.HubId, resident.SpotId);
            if (!spot)
                return;

            // Face center of ring
            bot->SetFacingTo(spot->O);

            if (roll_chance_i(25))
            {
                if (roll_chance_i(50))
                    bot->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
                else
                    bot->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);
            }
        }

        void ExecuteFishing(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);

            if (bot->GetSkillValue(SKILL_FISHING) == 0)
                return;

            Item* mainItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            if (!mainItem || !mainItem->GetTemplate() ||
                mainItem->GetTemplate()->Class != ITEM_CLASS_WEAPON ||
                mainItem->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                return;

            uint32 fishingSpell = 7620;
            if (!bot->HasSpell(fishingSpell))
                return;

            if (bot->IsNonMeleeSpellCast(false))
                return;

            bot->CastSpell(bot, fishingSpell, false);
        }

        void ExecuteInnDance(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                             bool enableSay, uint32 sayCooldownSec)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            Movement::DismountIfMounted(bot);

            if (resident.Sitting)
            {
                bot->SetStandState(UNIT_STAND_STATE_STAND);
                resident.Sitting = false;
            }

            bot->HandleEmoteCommand(EMOTE_STATE_DANCE);

            // Greet nearby real player
            if (enableSay)
            {
                Player* nearby = FindNearbyRealPlayer(bot, 6.0f);
                if (nearby && now >= resident.LastSayMs + sayCooldownSec * 1000)
                {
                    bot->SetFacingToObject(nearby);
                    resident.LastSayMs = now;
                    static char const* greetings[] = {
                        "Hey there, {}!",
                        "Welcome to the inn, {}!",
                        "Enjoy your stay, {}!",
                        "Hi, {}!"
                    };
                    bot->Say(Acore::StringFormat(greetings[urand(0, 3)], nearby->GetName()), LANG_UNIVERSAL);
                }
            }
        }

        void ExecutePlazaSocial(Resident& resident, Player* bot, Hub const& hub, uint32 now,
                                bool enableUnrestricted, uint32 randomSayChance, uint32 sayCooldownSec)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            if (resident.Sitting)
            {
                bot->SetStandState(UNIT_STAND_STATE_STAND);
                resident.Sitting = false;
            }

            if (roll_chance_i(35))
            {
                static uint32 const emotes[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_LAUGH };
                bot->HandleEmoteCommand(emotes[urand(0, 2)]);
            }

            if (enableUnrestricted && randomSayChance > 0 && roll_chance_i(randomSayChance) && now >= resident.LastSayMs + sayCooldownSec * 1000)
            {
                resident.LastSayMs = now;
                static char const* shouts[] = {
                    "Lovely weather in the city today!",
                    "Who's up for an adventure?",
                    "Busy day in the square!",
                    "Good to see everyone around!"
                };
                bot->Say(shouts[urand(0, 3)], LANG_UNIVERSAL);
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
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                resident.Sitting = true;
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
            {
                bot->SetStandState(UNIT_STAND_STATE_SIT);
                resident.Sitting = true;
            }

            if (roll_chance_i(30))
            {
                bot->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);
            }
        }

        void ExecuteVisitNpc(Resident& resident, Player* bot, Hub const& hub, uint32 now, uint32 npcFlag)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            // Search for nearby creature with matching NPCFlag
            std::list<Creature*> creatures;
            Acore::AnyUnitInObjectRangeCheck check(bot, 25.0f);
            Acore::CreatureListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, creatures, check);
            Cell::VisitObjects(bot, searcher, 25.0f);

            for (Creature* creature : creatures)
            {
                if (!creature || !creature->IsAlive())
                    continue;

                if (creature->HasNpcFlag(static_cast<NPCFlags>(npcFlag)))
                {
                    bot->SetFacingToObject(creature);
                    WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
                    packet << creature->GetGUID();
                    bot->GetSession()->HandleGossipHelloOpcode(packet);
                    break;
                }
            }
        }

        void ExecuteVisitMailbox(Resident& resident, Player* bot, Hub const& hub, uint32 now)
        {
            if (!bot || !bot->IsInWorld() || bot->isMoving() || !resident.AtSpot)
                return;

            if (!sCityLifeMgr.ValidateResidentAction(bot, resident))
                return;

            std::list<GameObject*> gameobjects;
            AnyGameObjectInObjectRangeCheck check(bot, 20.0f);
            Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, gameobjects, check);
            Cell::VisitObjects(bot, searcher, 20.0f);

            for (GameObject* go : gameobjects)
            {
                if (go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)
                {
                    bot->SetFacingToObject(go);
                    go->Use(bot);
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

            // Visual visit fallback
            ExecuteVisitNpc(resident, bot, hub, now, UNIT_NPC_FLAG_VENDOR);
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
                Movement::MoveToPosition(bot, hub.MapId, spot->X, spot->Y, spot->Z, spot->O, false);
            }
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
