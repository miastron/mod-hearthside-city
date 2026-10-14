/* Hearthside Life shared motion; GPL-2.0-or-later, matching the modules. */
#ifndef LIFE_MOTION_H
#define LIFE_MOTION_H

// Player-like movement for borrowed bots, shared by the Life modules. City Life owns this file;
// mod-hearthside-pvp includes it from ../../mod-hearthside-city/src/, so one copy serves both.
//
// Two layers:
//  - Launch: one validated move (a jump or a smooth run) with a choice of facing, that mod-playerbots
//    is told to wait out. PvP Life's duel footwork is built on it.
//  - Routine: a short timed antic an idle bot plays out (dance, strafe, hop about), advanced a step
//    at a time by the owning module, which re-validates its lease before every Advance call.

#include "Config.h"
#include "LastMovementValue.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "MovementTypedefs.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "Random.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace LifeMotion
{
    // Game-time milliseconds wrap; compare by signed difference.
    inline bool Reached(uint32 now, uint32 deadline)
    {
        return static_cast<int32>(now - deadline) >= 0;
    }

    // Where a move may land. Both circles are optional (radius 0 = none).
    struct Bound
    {
        float X = 0.0f, Y = 0.0f;
        float Radius = 0.0f;
        float HomeX = 0.0f, HomeY = 0.0f;
        float HomeRadius = 0.0f;
    };

    // Jumps longer than this run instead. With JumpSpeedZ a hop is ~0.83 s in the air, a player's arc.
    constexpr float MaxJumpDistance = 9.0f;
    constexpr float JumpSpeedZ = 7.96f;
    constexpr float JumpAirSeconds = 0.83f;

    struct Move
    {
        // Lands Distance yards from From along Angle.
        Position From;
        float Angle = 0.0f;
        float Distance = 0.0f;
        bool Jump = false;
        // Facing: keep facing Face, else hold FacingAngle if HoldFacing, else face where it is going.
        Unit* Face = nullptr;
        bool HoldFacing = false;
        float FacingAngle = 0.0f;
        // A run at walking pace (an idle stroll), not running speed. Jumps ignore it.
        bool Walk = false;
        // Also point mod-playerbots' "stay" position at the landing spot, so its "return to stay
        // position" does not walk an idle bot straight back between steps.
        bool MoveStay = false;
        // Tell mod-playerbots to wait the move out (its "last movement"). Off for a hop in the middle
        // of the bot's own run: its path resumes by itself afterwards, and its plan should stand.
        bool TellPlayerbots = true;
        // A jump longer than this runs instead.
        float JumpLimit = MaxJumpDistance;
    };

    struct Landing
    {
        float X = 0.0f, Y = 0.0f, Z = 0.0f;
        int32 DurationMs = 0;
        bool Jumped = false;
    };

    // One move: refused (false) if the spot is unreachable, water entered from dry land, a ledge away or outside the bound.
    // A run bends around From in angle and radius (From = the bot's own position runs straight), so
    // circling an opponent follows it and a back-off runs straight out; facing a target or holding a
    // facing while moving away from it backpedals.
    inline bool Launch(Player* bot, Move const& move, Bound const& bound, Landing* out = nullptr)
    {
        float x = move.From.GetPositionX() + std::cos(move.Angle) * move.Distance;
        float y = move.From.GetPositionY() + std::sin(move.Angle) * move.Distance;
        float z = bot->GetPositionZ();
        if (!bot->GetMap()->CanReachPositionAndGetValidCoords(bot, x, y, z) || std::fabs(z - bot->GetPositionZ()) > 3.0f)
            return false;
        if (bound.Radius > 0.0f && std::hypot(x - bound.X, y - bound.Y) > bound.Radius)
            return false;
        if (bound.HomeRadius > 0.0f && std::hypot(x - bound.HomeX, y - bound.HomeY) > bound.HomeRadius)
            return false;
        // Never from dry land into water; a bot already in it may still move (and so get back out).
        if (!bot->IsInWater() && bot->GetMap()->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            return false;
        float step = bot->GetExactDist2d(x, y);
        if (step < 1.0f)
            return false;

        int32 durationMs = 0;
        bool jumped = move.Jump && step <= move.JumpLimit;
        if (jumped)
        {
            float speedXY = std::max(step / JumpAirSeconds, 2.0f);
            if (move.HoldFacing && !move.Face)
            {
                // MoveJump can only face a unit; a strafe or backward hop holds its own facing.
                float moveTimeHalf = JumpSpeedZ / ::Movement::gravity;
                float maxHeight = -::Movement::computeFallElevation(moveTimeHalf, false, -JumpSpeedZ);
                bot->GetMotionMaster()->MoveIdle();
                ::Movement::MoveSplineInit init(bot);
                init.MoveTo(x, y, z);
                init.SetParabolic(maxHeight, 0);
                init.SetVelocity(speedXY);
                init.SetFacing(move.FacingAngle);
                durationMs = init.Launch();
            }
            else
            {
                // A player's jump arc, facing the target (or, with none, the way it is going).
                bot->GetMotionMaster()->MoveJump(x, y, z, speedXY, JumpSpeedZ, 0, move.Face);
                durationMs = int32(step / speedXY * 1000.0f);
            }
        }
        else
        {
            float startAngle = move.From.GetAngle(bot->GetPositionX(), bot->GetPositionY());
            float startRadius = move.From.GetExactDist2d(bot->GetPositionX(), bot->GetPositionY());
            float sweep = Position::NormalizeOrientation(move.Angle - startAngle);
            if (sweep > float(M_PI))
                sweep -= 2.0f * float(M_PI);
            if (startRadius < 1.0f)
            {
                startAngle = move.Angle;
                sweep = 0.0f;
            }
            uint32 points = std::clamp<uint32>(uint32(step / 1.5f), 2, 8);
            ::Movement::PointsArray path;
            path.push_back(G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
            for (uint32 i = 1; i <= points; ++i)
            {
                float t = float(i) / float(points);
                float pa = startAngle + sweep * t;
                float pr = startRadius + (move.Distance - startRadius) * t;
                float px = i == points ? x : move.From.GetPositionX() + std::cos(pa) * pr;
                float py = i == points ? y : move.From.GetPositionY() + std::sin(pa) * pr;
                float pz = i == points ? z : bot->GetPositionZ();
                if (i != points)
                    bot->UpdateAllowedPositionZ(px, py, pz);
                if (std::fabs(pz - path.back().z) > 2.5f)
                    return false; // a ledge or a wall mid-curve: skip this move
                path.push_back(G3D::Vector3(px, py, pz));
            }
            // Drop the chase or point movement first, or its next update relaunches over this spline.
            bot->GetMotionMaster()->MoveIdle();
            ::Movement::MoveSplineInit init(bot);
            init.MovebyPath(path);
            if (move.Walk)
                init.SetWalk(true);
            if (move.Face || move.HoldFacing)
            {
                float faceAngle = move.Face ? bot->GetAngle(move.Face) : move.FacingAngle;
                if (move.Face)
                    init.SetFacing(move.Face);
                else
                    init.SetFacing(faceAngle);
                float travel = std::atan2(y - bot->GetPositionY(), x - bot->GetPositionX());
                if (std::cos(travel - faceAngle) < -0.7f)
                    init.SetOrientationInversed(); // giving ground: backpedal
            }
            durationMs = init.Launch();
        }

        // Ask mod-playerbots' own movement to wait the move out rather than redirect it halfway.
        if (PlayerbotAI* ai = move.TellPlayerbots ? GET_PLAYERBOT_AI(bot) : nullptr)
        {
            ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().Set(
                bot->GetMapId(), x, y, z, bot->GetOrientation(), float(std::max(durationMs, 300)),
                MovementPriority::MOVEMENT_FORCED);
            if (move.MoveStay)
            {
                PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
                if (positions["stay"].isSet())
                    positions["stay"].Set(x, y, z, bot->GetMapId());
            }
        }

        if (out)
        {
            out->X = x;
            out->Y = y;
            out->Z = z;
            out->DurationMs = durationMs;
            out->Jumped = jumped;
        }
        return true;
    }

    // ---------------------------------------------------------------- hops on the way

    // A bot running somewhere on its own (a point move: mod-playerbots' travel, a resident walking to
    // its spot) on the ground, out of combat, with at least a jump's length still to go. Moves the
    // Life modules launch themselves are not point moves, so footwork, antics and strolls never qualify.
    inline bool CanHopOnTheWay(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->isMoving() || bot->IsInCombat() || bot->duel ||
            bot->IsInFlight() || bot->IsFlying() || bot->IsInWater() || bot->IsWalking() || bot->IsFalling() ||
            bot->IsNonMeleeSpellCast(false) || bot->GetVehicle() ||
            bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING) ||
            bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE ||
            bot->movespline->Finalized())
            return false;
        G3D::Vector3 end = bot->movespline->FinalDestination();
        float speed = bot->GetSpeed(MOVE_RUN);
        return bot->GetExactDist2d(end.x, end.y) > speed * JumpAirSeconds + 4.0f;
    }

    // A jump straight on along the run, as far as the bot's own speed carries it in the air, so the
    // run does not stutter. The point move picks up again from where it lands.
    inline bool HopOnTheWay(Player* bot, Landing* out = nullptr)
    {
        if (!CanHopOnTheWay(bot))
            return false;
        Move move;
        move.From = bot->GetPosition();
        move.Angle = bot->GetOrientation();
        // A galloping mount covers ~12 yards in the air; any more is a flying mount's speed on the ground.
        move.Distance = std::min(bot->GetSpeed(MOVE_RUN) * JumpAirSeconds, 12.0f);
        move.JumpLimit = 12.0f;
        move.Jump = true;
        move.TellPlayerbots = false;
        if (!Launch(bot, move, Bound(), out))
            return false;
        // The jump sits on top of the point move, which only resets (stops) when the jump ends and
        // then, finding its spline finished, takes itself for arrived: the bot stood still until
        // mod-playerbots sent it on again (realm 2026-10-14: bots stopping after every hop). Flagging
        // a speed change makes it relaunch the rest of its path from wherever the bot lands.
        bot->GetMotionMaster()->propagateSpeedChange();
        return true;
    }

    // ---------------------------------------------------------------- idle antics

    enum class Antic : uint8
    {
        None = 0,
        Dance,         // the /dance state, held
        HopBackForth,  // hop forward and back: backpedalling, or turning round between hops
        HopSideToSide, // strafe hops, facing fixed
        BunnyHop,      // hop after hop round a circle, a figure 8 or a zigzag
        HopDismount,   // a mounted arrival hops and gets off in mid-air (not a timed antic)
    };

    // The timed antics, in config order.
    constexpr uint8 TimedAnticCount = 4;
    inline Antic TimedAntic(uint8 index) { return Antic(uint8(Antic::Dance) + index); }

    inline char const* AnticName(Antic antic)
    {
        switch (antic)
        {
            case Antic::Dance: return "Dance";
            case Antic::HopBackForth: return "HopBackForth";
            case Antic::HopSideToSide: return "HopSideToSide";
            case Antic::BunnyHop: return "BunnyHop";
            case Antic::HopDismount: return "HopDismount";
            default: return "None";
        }
    }

    // Case-insensitive name to antic; None if unknown.
    inline Antic AnticFromName(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (uint8 i = uint8(Antic::Dance); i <= uint8(Antic::HopDismount); ++i)
        {
            std::string candidate = AnticName(Antic(i));
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (candidate == name)
                return Antic(i);
        }
        return Antic::None;
    }

    struct AnticConfig
    {
        bool Enable = true;
        uint32 ChancePerMinute = 10;  // % an idle bot starts one in any given minute
        uint32 CooldownSeconds = 120; // per bot, between the end of one and the start of the next
        uint32 HopDismountChance = 35;
        float Radius = 8.0f;          // how far a hopping antic strays from where it started
        uint32 Weight[TimedAnticCount] = { 20, 20, 20, 20 };
        uint32 MinSeconds[TimedAnticCount] = { 30, 5, 5, 15 };
        uint32 MaxSeconds[TimedAnticCount] = { 60, 10, 10, 30 };
    };

    // Reads <prefix>.Enable, .ChancePerMinute, .CooldownSeconds, .HopDismount.Chance, .Radius and,
    // per timed antic, .<Name>.Weight/.MinSeconds/.MaxSeconds, keeping `defaults` for what is unset.
    inline AnticConfig LoadAnticConfig(std::string const& prefix, AnticConfig const& defaults = AnticConfig())
    {
        AnticConfig c = defaults;
        c.Enable = sConfigMgr->GetOption<bool>(prefix + ".Enable", defaults.Enable);
        c.ChancePerMinute = std::min<uint32>(100, sConfigMgr->GetOption<uint32>(prefix + ".ChancePerMinute", defaults.ChancePerMinute));
        c.CooldownSeconds = std::min<uint32>(86400, sConfigMgr->GetOption<uint32>(prefix + ".CooldownSeconds", defaults.CooldownSeconds));
        c.HopDismountChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>(prefix + ".HopDismount.Chance", defaults.HopDismountChance));
        c.Radius = std::clamp(sConfigMgr->GetOption<float>(prefix + ".Radius", defaults.Radius), 3.0f, 20.0f);
        for (uint8 i = 0; i < TimedAnticCount; ++i)
        {
            std::string key = prefix + "." + AnticName(TimedAntic(i));
            c.Weight[i] = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>(key + ".Weight", defaults.Weight[i]));
            c.MinSeconds[i] = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(key + ".MinSeconds", defaults.MinSeconds[i]), 1, 600);
            c.MaxSeconds[i] = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(key + ".MaxSeconds", defaults.MaxSeconds[i]), c.MinSeconds[i], 600);
        }
        return c;
    }

    // A weighted pick among the timed antics; None if every weight is 0.
    inline Antic PickAntic(AnticConfig const& c)
    {
        uint32 total = 0;
        for (uint8 i = 0; i < TimedAnticCount; ++i)
            total += c.Weight[i];
        if (!total)
            return Antic::None;
        uint32 roll = urand(1, total);
        for (uint8 i = 0; i < TimedAnticCount; ++i)
        {
            if (roll <= c.Weight[i])
                return TimedAntic(i);
            roll -= c.Weight[i];
        }
        return Antic::None;
    }

    // Whether an idle bot starts an antic on a check made every `intervalMs`, at ChancePerMinute.
    inline bool RollAntic(AnticConfig const& c, uint32 intervalMs)
    {
        return c.Enable && c.ChancePerMinute && frand(0.0f, 100.0f) < float(c.ChancePerMinute) * float(intervalMs) / 60000.0f;
    }

    // One bot's antic in progress. Kind None = idle (CooldownUntilMs still applies).
    struct Routine
    {
        Antic Kind = Antic::None;
        uint32 EndMs = 0;
        uint32 NextMs = 0;
        uint32 CooldownUntilMs = 0;
        uint32 Step = 0;
        uint8 Misses = 0;   // refused moves in a row; too many ends the antic
        uint8 Variant = 0;  // HopBackForth: 0 backpedal, 1 turn round; BunnyHop: 0 circle, 1 figure 8, 2 zigzag
        int8 Side = 1;
        bool Dancing = false;
        float AnchorX = 0.0f, AnchorY = 0.0f; // centre of the antic
        float Facing = 0.0f;
        float Size = 0.0f;   // BunnyHop shape radius
        float Phase = 0.0f;  // BunnyHop position along the shape
        float LastHop = 0.0f;
        float TargetX = 0.0f, TargetY = 0.0f; // HopDismount: where it is riding to
    };

    inline bool Running(Routine const& r) { return r.Kind != Antic::None; }
    inline bool Ready(Routine const& r, uint32 now) { return !Running(r) && Reached(now, r.CooldownUntilMs); }

    // Undoes what the antic left on the bot (the dance state). Safe on a const record, so a
    // module can call it from a release path.
    inline void Halt(Player* bot, Routine const& r)
    {
        if (bot && r.Dancing && bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE)
            bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    }

    inline void Dismount(Player* bot)
    {
        if (bot->IsMounted() || bot->HasMountedAura())
        {
            bot->Dismount();
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }
    }

    // Ends the antic and starts the bot's cooldown. A mid-air dismount cut short still dismounts.
    inline void End(Player* bot, Routine& r, uint32 now, AnticConfig const& c)
    {
        Halt(bot, r);
        if (bot && bot->IsInWorld() && r.Kind == Antic::HopDismount)
            Dismount(bot);
        bool timed = r.Kind != Antic::HopDismount && r.Kind != Antic::None;
        uint32 cooldownUntil = r.CooldownUntilMs;
        r = Routine();
        r.CooldownUntilMs = timed ? now + c.CooldownSeconds * 1000 : cooldownUntil;
    }

    // Starts a timed antic where the bot stands. False (and nothing started) if the kind is not timed.
    inline bool Begin(Player* bot, Routine& r, Antic kind, uint32 now, AnticConfig const& c)
    {
        if (kind == Antic::None || kind == Antic::HopDismount)
            return false;
        uint8 index = uint8(kind) - uint8(Antic::Dance);
        uint32 cooldownUntil = r.CooldownUntilMs;
        r = Routine();
        r.CooldownUntilMs = cooldownUntil;
        r.Kind = kind;
        r.EndMs = now + urand(c.MinSeconds[index], c.MaxSeconds[index]) * 1000;
        r.NextMs = now;
        r.AnchorX = bot->GetPositionX();
        r.AnchorY = bot->GetPositionY();
        r.Facing = bot->GetOrientation();
        r.Side = urand(0, 1) ? 1 : -1;
        Dismount(bot);
        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        if (kind == Antic::HopBackForth)
            r.Variant = uint8(urand(0, 1));
        else if (kind == Antic::BunnyHop)
        {
            r.Variant = uint8(urand(0, 2));
            r.Size = frand(std::max(3.0f, c.Radius * 0.5f), std::max(3.0f, c.Radius * 0.75f));
            if (r.Variant == 0)
            {
                // A circle through where the bot stands: its centre lies Size away.
                float toCentre = frand(0.0f, 2.0f * float(M_PI));
                r.AnchorX += std::cos(toCentre) * r.Size;
                r.AnchorY += std::sin(toCentre) * r.Size;
                r.Phase = toCentre + float(M_PI); // the bot's own angle round the centre
            }
        }
        return true;
    }

    // A mounted bot riding in to (targetX, targetY): next Advance hops toward it and dismounts at the top.
    inline bool BeginHopDismount(Player* bot, Routine& r, float targetX, float targetY, uint32 now)
    {
        if (!bot->IsMounted() && !bot->HasMountedAura())
            return false;
        uint32 cooldownUntil = r.CooldownUntilMs;
        r = Routine();
        r.CooldownUntilMs = cooldownUntil;
        r.Kind = Antic::HopDismount;
        r.EndMs = now + 5000;
        r.NextMs = now;
        r.TargetX = targetX;
        r.TargetY = targetY;
        return true;
    }

    namespace Detail
    {
        inline bool Step(Player* bot, Routine& r, Bound const& bound, float angle, float distance,
            bool jump, bool holdFacing, Landing& landing)
        {
            Move move;
            move.From = bot->GetPosition();
            move.Angle = angle;
            move.Distance = distance;
            move.Jump = jump;
            move.HoldFacing = holdFacing;
            move.FacingAngle = r.Facing;
            move.MoveStay = true;
            return Launch(bot, move, bound, &landing);
        }

        // A hop to a point (BunnyHop shapes), facing the way it goes.
        inline bool HopTo(Player* bot, Routine& r, Bound const& bound, float x, float y, Landing& landing)
        {
            float angle = std::atan2(y - bot->GetPositionY(), x - bot->GetPositionX());
            return Step(bot, r, bound, angle, bot->GetExactDist2d(x, y), true, false, landing);
        }

        // The next point of a BunnyHop shape, about `chord` yards on from the current one.
        inline void NextShapePoint(Routine& r, float chord, float& x, float& y)
        {
            float cs = std::cos(r.Facing), sn = std::sin(r.Facing);
            switch (r.Variant)
            {
                case 0: // circle round the anchor
                    r.Phase += float(r.Side) * chord / r.Size;
                    x = r.AnchorX + std::cos(r.Phase) * r.Size;
                    y = r.AnchorY + std::sin(r.Phase) * r.Size;
                    return;
                case 1: // figure 8 through the anchor: (S sin t, S sin t cos t) along/across the facing
                {
                    auto point = [&](float t, float& px, float& py)
                    {
                        float u = r.Size * std::sin(t), v = r.Size * std::sin(t) * std::cos(t);
                        px = r.AnchorX + u * cs - v * sn;
                        py = r.AnchorY + u * sn + v * cs;
                    };
                    float sx, sy;
                    point(r.Phase, sx, sy);
                    float t = r.Phase;
                    for (int i = 0; i < 64; ++i)
                    {
                        t += float(r.Side) * 0.05f;
                        point(t, x, y);
                        if (std::hypot(x - sx, y - sy) >= chord)
                            break;
                    }
                    r.Phase = t;
                    return;
                }
                default: // zigzag along the facing line, turning back at the ends
                {
                    r.Phase += float(r.Side) * chord * 0.8f;
                    if (std::fabs(r.Phase) > r.Size)
                    {
                        r.Side = int8(-r.Side);
                        r.Phase = std::clamp(r.Phase, -r.Size, r.Size);
                    }
                    float lateral = (r.Step % 2 ? 1.0f : -1.0f) * chord * 0.6f;
                    x = r.AnchorX + r.Phase * cs - lateral * sn;
                    y = r.AnchorY + r.Phase * sn + lateral * cs;
                    return;
                }
            }
        }
    }

    // Plays the next step of the antic when it is due. False when it is over or cannot go on (the
    // bot fought, died, was moved off, kept hitting walls): the caller then calls End. `home` limits
    // where any step may land (its HomeX/HomeY/HomeRadius); the antic adds its own radius.
    inline bool Advance(Player* bot, Routine& r, uint32 now, AnticConfig const& c, Bound const& home)
    {
        if (!Running(r) || !bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->duel ||
            bot->IsBeingTeleported() || bot->IsInFlight() || bot->IsNonMeleeSpellCast(false) ||
            bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
            return false;
        // A hop or step under way always lands first: ending it in mid-air hands the bot back to
        // module or mod-playerbots code that stops it dead, and it drops straight down.
        bool airborne = !bot->movespline->Finalized();
        if (Reached(now, r.EndMs))
            return airborne && !Reached(now, r.EndMs + 3000);
        if (!Reached(now, r.NextMs))
            return true;

        Bound bound = home;
        bound.X = r.AnchorX;
        bound.Y = r.AnchorY;
        bound.Radius = (r.Kind == Antic::BunnyHop ? r.Size : c.Radius) + 2.0f;

        Landing landing;
        bool moved = false;
        uint32 pauseMs = 0;
        switch (r.Kind)
        {
            case Antic::HopDismount:
            {
                if (r.Step == 0)
                {
                    float distance = bot->GetExactDist2d(r.TargetX, r.TargetY);
                    float angle = distance >= 1.0f ? std::atan2(r.TargetY - bot->GetPositionY(), r.TargetX - bot->GetPositionX())
                        : bot->GetOrientation();
                    Bound open = home;
                    if (!Detail::Step(bot, r, open, angle, std::clamp(distance, 2.5f, 5.0f), true, false, landing))
                    {
                        Dismount(bot);
                        return false;
                    }
                    // Get off a little before the top of the arc, so the rider drops the rest of the way.
                    r.Step = 1;
                    r.NextMs = now + uint32(std::clamp(landing.DurationMs * 2 / 5, 200, 400));
                    return true;
                }
                if (r.Step == 1)
                {
                    Dismount(bot);
                    r.Step = 2;
                }
                // Then carry on through the rest of the arc and end only on landing.
                return airborne;
            }
            case Antic::Dance:
                if (!r.Dancing)
                {
                    bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DANCE);
                    r.Dancing = true;
                }
                r.NextMs = now + 1000;
                return true;
            case Antic::HopBackForth:
            {
                bool forward = r.Step % 2 == 0;
                if (r.Variant == 1 && !forward)
                {
                    // Turn round, then hop "forward" back the way it came.
                    r.Facing = Position::NormalizeOrientation(r.Facing + float(M_PI));
                    bot->SetFacingTo(r.Facing);
                    ++r.Step;
                    r.NextMs = now + urand(300, 500);
                    return true;
                }
                if (forward)
                {
                    r.LastHop = frand(2.5f, 4.0f);
                    moved = Detail::Step(bot, r, bound, r.Facing, r.LastHop, true, true, landing);
                }
                else
                {
                    // A backpedal hop: slower than running, so it falls short and the bot drifts on.
                    moved = Detail::Step(bot, r, bound, r.Facing + float(M_PI), r.LastHop * 0.65f, true, true, landing);
                }
                if (!moved && forward)
                {
                    // A wall, a ledge or the edge of the area ahead: turn round and go the other way.
                    r.Facing = Position::NormalizeOrientation(r.Facing + float(M_PI));
                    bot->SetFacingTo(r.Facing);
                }
                pauseMs = urand(100, 350);
                break;
            }
            case Antic::HopSideToSide:
            {
                r.Side = int8(-r.Side);
                float across = r.Facing + float(r.Side) * float(M_PI) / 2.0f;
                float reach = frand(1.2f, 2.0f);
                float tx = r.AnchorX + std::cos(across) * reach;
                float ty = r.AnchorY + std::sin(across) * reach;
                float angle = std::atan2(ty - bot->GetPositionY(), tx - bot->GetPositionX());
                moved = Detail::Step(bot, r, bound, angle, bot->GetExactDist2d(tx, ty), true, true, landing);
                pauseMs = urand(100, 350);
                break;
            }
            case Antic::BunnyHop:
            {
                float x, y;
                Detail::NextShapePoint(r, frand(3.0f, 4.0f), x, y);
                moved = Detail::HopTo(bot, r, bound, x, y, landing);
                if (!moved)
                    r.Side = int8(-r.Side); // blocked: run the shape the other way
                pauseMs = urand(40, 180);
                break;
            }
            default:
                return false;
        }

        if (!moved)
        {
            if (++r.Misses > 4)
                return false;
            r.NextMs = now + 300;
            return true;
        }
        r.Misses = 0;
        ++r.Step;
        r.NextMs = now + uint32(std::max(landing.DurationMs, 0)) + pauseMs;
        return true;
    }
}

#endif
