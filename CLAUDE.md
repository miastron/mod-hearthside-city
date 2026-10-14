# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`mod-hearthside-city` is an AzerothCore (WotLK 3.3.5a) worldserver module that borrows idle
random playerbots from `mod-playerbots` and parks them in cities as "residents" (wandering
districts, standing at AH/bank/mail spots, taverns, fishing, watching the gate duels). It is a fork of upstream
`mod-playerbots-city-life` by iCore, adapted to this workspace's core
(`../azerothcore-wotlk-pb`) and `../mod-playerbots`. Sibling repo `../mod-hearthside-pvp` is the
matching fork of `mod-playerbots-pvp-life`; the two coexist through a shared reservation registry
(below).

Upstream is the `upstream` remote (Ildourol/mod-playerbots-city-life); its commits are reviewed and
ported one by one rather than merged (duels and the telemetry bridge were dropped here, see
Architecture). Synced through upstream `7456568` (2026-10-12).

Upstream names were deliberately kept everywhere except the loader: config file
`mod_playerbots_city_life.conf`, keys `CityLife.*`, GM command `.citylife`, log line
`>> Loaded mod-playerbots-city-life`, `module-info.json`. Don't "finish the rename" without being
asked: operators' existing configs depend on those names.

## Build

There is no standalone build and no test suite. The module compiles into AzerothCore's `modules`
target when placed at `azerothcore-wotlk/modules/mod-hearthside-city/` (`CMakeLists.txt` just
calls `AC_ADD_SCRIPT` on `src/` and `AC_ADD_CONFIG_FILE` on the `.conf.dist`). Compiling,
restarting the worldserver and anything touching MySQL happen on the test realm; see
`../mod-hearthside-chat/CLAUDE.md` for the realm and the local MSVC toolchain notes. Label any
runtime claim you could not exercise as unverified.

- **The loader function name is derived from the directory name.** AzerothCore calls
  `Add<dir_name_with_underscores>Scripts()`, so `src/playerbots_city_life_loader.cpp` must define
  `Addmod_hearthside_cityScripts()`. Renaming the directory means renaming this function.
- Adding a new `.cpp` forces a CMake re-run (glob) and a full rebuild of `modules` on the realm;
  a new header doesn't.

## Core API drift (this core vs. what upstream was written against)

Most local diffs from upstream are adaptations to these. Use the local forms:

- No `WorldSession::IsBot()`. Use `CityLife::IsBotSession(player)` (`CityLifeMgr.cpp`), which
  checks the random-bot account list / addclass bots. `::IsRealPlayer()` alone is wrong during
  login: mod-playerbots attaches its AI *after* `OnPlayerLogin`, so every bot looks human then.
- `Acore::StringFormat(...)`, not `Acore::String::Format(...)`.
- `GameTime::GetGameTime()` / `GetGameTimeMS()` return durations: call `.count()`.
- mod-playerbots renamed config keys `AiPlayerbot.*` → `Playerbots.*` and `PlayerbotAIConfig`
  members to UpperCamelCase (its PR #2854). Read its current headers before touching either;
  `ImportPlayerbotDefaults()` reads the new key with the old one as fallback.

## Architecture

- **Entry points** (`mod_playerbots_city_life.cpp`): `AddCityLifeScripts()` checks the registry
  protocol (the active-module flag is set by `LoadConfig` only while the module is enabled and its
  tables exist), and registers a `WorldScript` (config load → `LoadConfig`, `OnUpdate` → `Update`,
  shutdown), a `PlayerScript` (logout, trade blocking,
  achievement suppression for residents in scripted scenes), a `GroupScript` (a resident joining a group
  with a real player is disengaged at once) and the `.citylife` `CommandScript`. All logic lives
  in the `CityLife::Manager` singleton (`sCityLifeMgr`).
- **`CityLifeMgr.cpp`** (~2.6k lines) is the whole lifecycle. `Update()` pumps async DB callbacks
  and registry arbitration every frame, waits out a startup delay (at least mod-playerbots'
  `DisabledWithoutRealPlayerLoginDelay`), then every `_tickSeconds` runs `MaintainPopulation()`
  (budget/time-of-day/demand → per-hub targets → candidate scan → admission requests) and
  `MaintainResidents()` (per-resident activity state machine, relocation, emotes, lease refresh).
  Admissions per tick and releases per second are capped by `_maxChangesPerTick`.
- **Demand (hybrid):** a hub with a real player in it (`PlayerPresent`: mod-playerbots' wake rule
  over the hub's zone, map or spots, plus `BurstGraceSeconds`) asks for its full population; an
  empty one keeps a floor (`BaselinePercent`, default 50). While a player is there the hub changes
  one resident at a time (`PaceAllows`/`PaceUsed`, `CityLife.Population.WatchedPace*`): an arrival
  walks in from where it stands or from a drop-off out of sight (`AdmitResident` →
  `LifeArrival::FindDropOff`, `ProcessArrivals` starts the walk), or waits if neither works and
  the spot is in sight; a departure is marked due (`ReleaseAt`) and leaves like an ended residence
  (inn bed, then a walk-off). Residents on their way out do not count (`CountStaying`). Unwatched
  hubs fill and empty at once. The user asked for a floor rather than an empty city that fills
  when they arrive (2026-10-14). A roaming resident that finishes an
  activity picks the next by weight (`PickNextActivity`, `CityLife.Weight.*`) and a spot for it
  (`ChooseSpotForActivity`, class-district matching for trainers); fishers and inn dancers stay
  at their venue and alternate with sitting; an expired residence ends asleep at an inn
  (`ReleaseCycle`, `FindInnBedSpot`) before the release.
- **Admission is two-phase**: `MaintainPopulation` queues an admission request with the shared
  registry; `ResolveAdmissionRequests` grants a *Provisional* lease; `ProcessAdmissionGrants` takes
  the grant and `AdmitResident` promotes it to *Active*, snapshots the bot's original position, and
  applies city strategies (`_nonCombatStrategies`). Release is also staged
  (`ReleaseResident` → `_releasingResidents` → `ProcessReleasingResidents`) so the bot's
  strategies/position are restored before the lease is dropped. Every resident action should
  re-validate its lease (`ValidateActiveLease` / `ValidateResidentAction`); a lost lease means
  another module or a real player now owns the bot.
- **`LifeBotReservation.h`**: header-only, process-wide registry (owners, leases with generations,
  admission arbitration, duel staging slots on a 20yd lattice, real-player challenge cooldowns --
  the last two used only by `mod-hearthside-pvp` now) shared with `mod-hearthside-pvp`/artisans. Because both modules compile into one binary, the
  copies must stay **byte-compatible**; `AddCityLifeScripts()` calls `CheckProtocolVersion()`, which
  logs an error when another Life module registered a different `LIFE_BOT_RESERVATION_PROTOCOL_VERSION`.
  Change it in both repos together and bump the version.
- **`LifeMotion.h`**: header-only, owned here and included by `mod-hearthside-pvp` straight from
  this repo (`../../mod-hearthside-city/src/LifeMotion.h`), so a change here changes both modules.
  `Launch` is one validated move (jump or curve, facing a unit, a held angle or the way it goes)
  that mod-playerbots is told to wait out; PvP's duel footwork is built on it. `Routine` plays the
  idle antics (Dance, HopBackForth, HopSideToSide, BunnyHop, and HopDismount for mounted
  arrivals), started at wander, random-emote and duel-spectator spots: `Manager::UpdateAntics` advances them every ~100 ms, `MaintainResidents` leaves a
  resident mid-antic alone, `CityLife.Antics.*` configures them, `.citylife debug antic` forces one.
- **Shared Life headers** (owned here, included by PvP from this repo, like `LifeMotion.h`):
  `LifeBots.h` (bot-account filter behind `IsBotSession`/`IsHumanPlayer`/`IsHumanControlled`,
  archetype filter, budget math, live-bot census, the `StrategyJournal` behind `Resident::Journal`,
  and `HoldLevels`, the level freeze behind `KeepResidentLevelsFixed`: one registry for both
  modules), `LifePresence.h` (real-player snapshot and the wake rule), `LifeArrival.h`
  (`FindDropOff`, `InSight`, `WalkOff`: arrivals and departures where a player could see them) and
  `LifeAudit.h` (the async audit writer; `sCityLifeAudit` writes `Logs/citylife_audit.log`). A
  change here changes both modules: build both.
- **`LifeMotion::HopOnTheWay`** also drives `Manager::UpdateTravelHops` (every 250 ms): any random
  bot on a point move within `CityLife.TravelHops.Radius` of a real player jumps 1-5 times every
  15-45 s. It is world-wide, not limited to residents, and never touches a player's own alt bot.
- **`LifeMedic.h`**: header-only, owned here and included by PvP the same way. Healer bots fixed by
  GUID as "medics" heal the duelists of a duel that just ended in front of them
  (`PlayerScript::OnPlayerDuelEnd` → `QueueDuelEnd` → `UpdateDuelMedics`, `CityLife.DuelMedics.*`).
- **Duels are `mod-hearthside-pvp`'s.** City Life's duel engine and the upstream addon telemetry
  bridge (`LifeTelemetryBridge.h`) were removed with upstream's 2026-10-12 commit; what is left is
  the spectator role (`CitizenRole::Spectator`, `WatchDuels`): residents of hubs whose name contains
  "gate" or "duel" (`Hub::DuelHub`) or at `DuelRing` spots watch any duel in progress nearby.
- **`CityLifeActivities.*`** (crowd chat/AFK, duel spectators, fishing, inn dancing, tavern, NPC
  and mailbox visits, wall patrol, the inn sleep before release), **`CityLifeMovement.*`** (pathing validation, water/canal avoidance, mounting, sink
  detection), **`CityLifeTypes.h`** (Hub/Spot/Resident structs and enums), `CityLifeDialogues.h`
  and `CityLifePopulation.h` (static data tables).
- **Archetype filter (fork-only)**: `CityLife.Bots.Archetypes` restricts which bots may be borrowed
  using `../mod-hearthside-chat`'s GUID-fixed personality. `LifeBots.h` picks it up with
  `__has_include("hs_archetype.h")` and calls `Hs_ArchetypeForBot` / `Hs_ArchetypeMaskForNames`;
  without that module every bot fits. Changing those signatures in the chat module breaks this build.

## Data and SQL

- Hubs and spots live in the WORLD tables `city_life_hub` / `city_life_spot`, loaded by
  `LoadData()`. Config values override `default_population` at runtime. GM `.citylife hub|spot`
  edits go through `QueueMutation()` (async transaction, max 32 queued) and trigger a reload.
- `data/sql/db-world/base/city_life.sql` is the single schema + seed file, applied by AzerothCore's
  updater (only `db-world`-style directories are scanned). It is written to be **re-applied**: the
  updater stores each file's SHA1 and re-runs it whenever it changes (comment edits included), so it
  must stay idempotent: `CREATE TABLE IF NOT EXISTS`, the `AddColumnUnlessExists_CityLife`
  procedure for new columns, `INSERT IGNORE` for seeds so operator-edited rows survive. Upstream's
  `manual/` + `updates/` scripts were folded into it; add new columns/seeds there the same way
  rather than adding an `updates/` file.
- `data/sql/manual/2026_10_10_02_city_life_guard_ambience.sql` is optional guard SmartAI flavor,
  applied by hand only (`manual/` is not scanned).
- `VerifyDatabase()` only checks that both tables exist; its error text points at the updater and
  `Updates.EnableDatabases`. `.citylife spot delete` disables a row instead of deleting it, so a
  re-applied seed does not resurrect it.

## Conventions

- Logging uses the `module` logger with a `[CityLife]` prefix; the startup line uses `server.loading`.
- Every new config key goes in `conf/mod_playerbots_city_life.conf.dist` with a comment, and is read
  in `Manager::LoadConfig()` (re-run on `.reload config` via `OnAfterConfigLoad`). Keys that mean the
  same thing in PvP share their name after the prefix (`Bots.*`, `Population.BudgetMode`,
  `Antics.*`, `DuelMedics.*`, `Audit.*`); keep it that way.
- Keep the README's invariants: borrow bots as-is (no level/talent/stat changes), never touch real
  players or player-owned bots, exempt GMs from greetings/whispers, chatter and economic interactions default off.
