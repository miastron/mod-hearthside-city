/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeMovement.h"

#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <cmath>
#include <limits>

namespace CityLife
{
    namespace Movement
    {
        bool LowestPoiZWithin(Hub const& hub, float x, float y, float radius, float* outMinZ)
        {
            if (!outMinZ || hub.Spots.empty())
                return false;

            float minZ = std::numeric_limits<float>::max();
            bool found = false;

            for (Spot const& spot : hub.Spots)
            {
                float dx = spot.X - x;
                float dy = spot.Y - y;
                float dist2d = std::sqrt(dx * dx + dy * dy);
                if (dist2d <= radius)
                {
                    if (spot.Z < minZ)
                    {
                        minZ = spot.Z;
                        found = true;
                    }
                }
            }

            if (found)
            {
                *outMinZ = minZ;
                return true;
            }

            return false;
        }

        void CheckSinkForensics(Player* bot, float prevZ, bool sinkForensicsEnabled)
        {
            if (!sinkForensicsEnabled || !bot || !bot->IsInWorld() || prevZ == 0.0f)
                return;

            if (bot->IsBeingTeleported())
                return;

            float currentZ = bot->GetPositionZ();
            float drop = prevZ - currentZ;

            if (drop > 8.0f)
            {
                if (bot->IsInWater() || drop > 20.0f)
                {
                    LOG_WARN("module", "[CityLife] SINK WARNING: Bot '{}' (GUID {}) dropped {:.1f}y (Z {:.1f} -> {:.1f}) at ({:.1f}, {:.1f}) | moving={} inWater={}",
                        bot->GetName(), bot->GetGUID().GetCounter(), drop, prevZ, currentZ,
                        bot->GetPositionX(), bot->GetPositionY(), bot->isMoving() ? 1 : 0, bot->IsInWater() ? 1 : 0);
                }
                else
                {
                    LOG_DEBUG("module", "[CityLife] SINK INFO: Bot '{}' (GUID {}) descended {:.1f}y (Z {:.1f} -> {:.1f}) at ({:.1f}, {:.1f})",
                        bot->GetName(), bot->GetGUID().GetCounter(), drop, prevZ, currentZ,
                        bot->GetPositionX(), bot->GetPositionY());
                }
            }
        }

        void DismountIfMounted(Player* bot)
        {
            if (!bot)
                return;

            if (bot->HasMountedAura() || bot->IsMounted())
            {
                bot->Dismount();
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            }
        }

        void ClearMountAndFlight(Player* bot)
        {
            if (!bot)
                return;

            // A bot borrowed mid-air (flying mount, flight form) and teleported into a city keeps its
            // fly movement flags and hovers on the floor; mod-playerbots' ClearStaleFlightFlags clears
            // the same flags once the fly auras are gone.
            DismountIfMounted(bot);
            bot->RemoveAurasByType(SPELL_AURA_FLY);
            bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
            if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY))
            {
                bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_CAN_FLY);
                if (!bot->IsRooted())
                    bot->SendMovementFlagUpdate();
            }
        }

        bool CanMountHere(Player* bot)
        {
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                return false;

            if (bot->IsInCombat() || bot->duel || bot->IsInWater() || bot->IsBeingTeleported())
                return false;

            if (bot->GetMap() && bot->GetMap()->Instanceable())
                return false;

            return bot->IsOutdoors();
        }

        bool TryMount(Player* bot)
        {
            if (!CanMountHere(bot) || bot->HasMountedAura() || bot->isMoving())
                return false;

            // First, scan bot's learned spells for the fastest ground mount
            uint32 bestSpell = 0;
            int32 bestSpeed = -1;
            for (auto const& entry : bot->GetSpellMap())
            {
                uint32 const spellId = entry.first;
                if (!entry.second || entry.second->State == PLAYERSPELL_REMOVED || !entry.second->Active)
                    continue;

                SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                if (!spellInfo || spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED || spellInfo->IsPassive())
                    continue;

                if (spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                    spellInfo->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                    spellInfo->Id == 54729)
                    continue;

                int32 const speed = std::max(spellInfo->Effects[1].BasePoints, spellInfo->Effects[2].BasePoints);
                if (speed > bestSpeed)
                {
                    bestSpeed = speed;
                    bestSpell = spellId;
                }
            }

            if (bestSpell != 0)
            {
                bot->CastSpell(bot, bestSpell, true);
                return true;
            }

            // Fallback to standard racial and class mounts
            static uint32 const mountSpells[] = {
                // Class Mounts
                48778,  // Acherus Deathcharger (Death Knight)
                23214,  // Charger (Paladin 60)
                23161,  // Dreadsteed (Warlock 60)
                13819,  // Warhorse (Paladin 20/40)
                5784,   // Felsteed (Warlock 20/40)

                // Epic Racial Mounts (100% speed)
                23227,  // Swift White Steed (Human)
                23228,  // Swift Brown Steed (Human)
                23229,  // Swift Palomino (Human)
                23238,  // Swift Brown Ram (Dwarf)
                23239,  // Swift Gray Ram (Dwarf)
                23240,  // Swift White Ram (Dwarf)
                23219,  // Swift Mistsaber (Night Elf)
                23221,  // Swift Frostsaber (Night Elf)
                23338,  // Swift Stormsaber (Night Elf)
                23222,  // Swift Green Mechanostrider (Gnome)
                23223,  // Swift White Mechanostrider (Gnome)
                23225,  // Swift Yellow Mechanostrider (Gnome)
                35717,  // Great Green Elekk (Draenei)
                35718,  // Great Blue Elekk (Draenei)
                35719,  // Great Purple Elekk (Draenei)
                23250,  // Swift Brown Wolf (Orc)
                23251,  // Swift Timber Wolf (Orc)
                23252,  // Swift Gray Wolf (Orc)
                17465,  // Green Skeletal Warhorse (Undead)
                23246,  // Great White Kodo (Tauren)
                23247,  // Great Brown Kodo (Tauren)
                23248,  // Great Gray Kodo (Tauren)
                23241,  // Swift Blue Raptor (Troll)
                23242,  // Swift Olive Raptor (Troll)
                23243,  // Swift Orange Raptor (Troll)
                35711,  // Swift Pink Hawkstrider (Blood Elf)
                35712,  // Swift Green Hawkstrider (Blood Elf)
                35713,  // Swift Purple Hawkstrider (Blood Elf)

                // Standard Racial Mounts (60% speed)
                458,    // Brown Horse (Human)
                470,    // Black Stallion (Human)
                472,    // Pinto (Human)
                6648,   // Chestnut Mare (Human)
                6777,   // Gray Ram (Dwarf)
                6898,   // White Ram (Dwarf)
                6899,   // Brown Ram (Dwarf)
                8394,   // Striped Nightsaber (Night Elf)
                10793,  // Striped Frostsaber (Night Elf)
                10796,  // Spotted Frostsaber (Night Elf)
                10873,  // Red Mechanostrider (Gnome)
                10969,  // Blue Mechanostrider (Gnome)
                10971,  // Green Mechanostrider (Gnome)
                17453,  // Unpainted Mechanostrider (Gnome)
                34406,  // Brown Elekk (Draenei)
                35715,  // Gray Elekk (Draenei)
                35716,  // Purple Elekk (Draenei)
                580,    // Timber Wolf (Orc)
                6653,   // Gray Wolf (Orc)
                6654,   // Brown Wolf (Orc)
                824,    // Black Skeletal Horse (Undead)
                17462,  // Red Skeletal Horse (Undead)
                17463,  // Blue Skeletal Horse (Undead)
                17464,  // Brown Skeletal Horse (Undead)
                18989,  // Gray Kodo (Tauren)
                18990,  // Brown Kodo (Tauren)
                18991,  // White Kodo (Tauren)
                8395,   // Emerald Raptor (Troll)
                10799,  // Turquoise Raptor (Troll)
                10800,  // Violet Raptor (Troll)
                35018,  // Blue Hawkstrider (Blood Elf)
                35020,  // Brown Hawkstrider (Blood Elf)
                35022,  // Purple Hawkstrider (Blood Elf)
                35710   // Black Hawkstrider (Blood Elf)
            };

            for (uint32 spellId : mountSpells)
            {
                if (bot->HasSpell(spellId))
                {
                    bot->CastSpell(bot, spellId, true);
                    return true;
                }
            }

            return false;
        }

        void MoveToPosition(Player* bot, uint32 mapId, float x, float y, float z, float o, bool teleport)
        {
            if (!bot || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(o))
                return;

            if (teleport || bot->GetMapId() != mapId)
            {
                // mod-playerbots' PlayerbotHolder::UpdateSessions acks the teleport on its next update.
                bot->TeleportTo(mapId, x, y, z, o);
            }
            else
            {
                bot->ClearUnitState(UNIT_STATE_CHASE | UNIT_STATE_FOLLOW);
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, x, y, z);
            }
        }

        bool IsAtSpot(Player* bot, Spot const& spot, float tolerance)
        {
            if (!bot)
                return false;

            return bot->GetDistance2d(spot.X, spot.Y) <= tolerance;
        }

        bool PathTouchesWater(Player* bot, PathGenerator const& path)
        {
            if (!bot)
                return false;

            Map const* map = bot->GetMap();
            if (!map)
                return false;

            uint32 const phase = bot->GetPhaseMask();
            float const ch = bot->GetCollisionHeight();
            ::Movement::PointsArray const& pts = path.GetPath();

            for (G3D::Vector3 const& p : pts)
            {
                if (map->IsInWater(phase, p.x, p.y, p.z, ch))
                    return true;
            }

            // Path points are several yards apart; sample the midpoints so a narrow canal
            // cannot slip between two dry points on opposite banks.
            for (std::size_t i = 1; i < pts.size(); ++i)
            {
                G3D::Vector3 const m = (pts[i - 1] + pts[i]) * 0.5f;
                if (map->IsInWater(phase, m.x, m.y, m.z, ch))
                    return true;
            }

            // If path is incomplete, check 5y radial ring around tail to ensure it doesn't end into water edge
            if (!pts.empty() && (path.GetPathType() & PATHFIND_INCOMPLETE))
            {
                G3D::Vector3 const& e = pts.back();
                static float const kRing[][2] = {
                    {5.0f, 0.0f}, {-5.0f, 0.0f}, {0.0f, 5.0f}, {0.0f, -5.0f},
                    {3.5f, 3.5f}, {-3.5f, 3.5f}, {3.5f, -3.5f}, {-3.5f, -3.5f}
                };
                for (auto const& r : kRing)
                {
                    if (map->IsInWater(phase, e.x + r[0], e.y + r[1], e.z, ch))
                        return true;
                }
            }

            return false;
        }

        bool HasCompleteWalkablePath(Player* bot, float x, float y, float z, bool allowWater)
        {
            if (!bot || !bot->IsInWorld())
                return false;

            PathGenerator path(bot);
            if (!path.CalculatePath(x, y, z, false))
                return false;

            uint32 const type = path.GetPathType();
            if (type & (PATHFIND_NOPATH | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
                return false;

            if (type & PATHFIND_INCOMPLETE)
            {
                G3D::Vector3 const& end = path.GetActualEndPosition();
                float const tx = end.x - x;
                float const ty = end.y - y;
                float const tz = end.z - z;
                if (tx * tx + ty * ty + tz * tz > 15.0f * 15.0f)
                    return false;
            }

            if (!allowWater && PathTouchesWater(bot, path))
                return false;

            return true;
        }

        bool ValidateCityDestination(Player* bot, uint32 mapId, float x, float y, float z, float o, bool allowWater)
        {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(o))
                return false;

            Map const* map = nullptr;
            if (bot && bot->IsInWorld())
            {
                if (bot->GetMapId() == mapId)
                    map = bot->GetMap();
            }
            if (!map)
                map = sMapMgr->FindBaseMap(mapId);

            if (!map)
                return false;

            if (map->Instanceable())
                return false;

            if (bot)
            {
                if (bot->InBattleground() || bot->InBattlegroundQueue())
                    return false;

                if (bot->GetMap() && bot->GetMap()->Instanceable())
                    return false;

                if (!allowWater)
                {
                    uint32 phase = bot->GetPhaseMask();
                    float ch = bot->GetCollisionHeight();
                    if (map->IsInWater(phase, x, y, z, ch))
                        return false;
                }

                if (bot->GetMapId() == mapId)
                {
                    float groundZ = map->GetHeight(bot->GetPhaseMask(), x, y, z, true, 50.0f);
                    if (groundZ != INVALID_HEIGHT && std::abs(groundZ - z) > 30.0f)
                        return false;
                }
            }

            return true;
        }
    }
}
