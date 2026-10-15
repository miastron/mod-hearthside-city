# mod-hearthside-city

AzerothCore (WotLK 3.3.5a) module that borrows idle random playerbots and parks them in cities as
residents: wandering districts, AH/bank/mail spots, taverns, fishing, watching gate duels. Fork of
upstream `mod-playerbots-city-life` (iCore; `upstream` remote, Ildourol). Upstream commits are
reviewed and ported one by one, never merged; synced through `7456568` (2026-10-12).

## The Hearthside collection

| Module | Role |
|---|---|
| `mod-hearthside-life` | Coordinator: the shared `Life*.h` headers and their state. City and PvP require it. |
| `mod-hearthside-chat` | All bot speech. Talks to Life through `hs_life.*`; never included by the others. |
| `mod-hearthside-city` | Bots as city residents (this repo). |
| `mod-hearthside-pvp` | Bots in world PvP and duels (fork of `mod-playerbots-pvp-life`). |

## Build

No standalone build, no tests. Compiles into AzerothCore's `modules` target; building, restarting
and MySQL happen on the test realm (see `../mod-hearthside-chat/CLAUDE.md`). Label any runtime
claim you could not exercise as unverified.

- The loader `Addmod_hearthside_cityScripts()` (`src/playerbots_city_life_loader.cpp`) is named
  after the folder; renaming one means renaming the other.
- A new `.cpp` forces a CMake re-run and a full rebuild of `modules`; a new header does not.
- Everything else keeps upstream names on purpose (`CityLife.*` keys,
  `mod_playerbots_city_life.conf`, `.citylife`, the `>> Loaded` line, `module-info.json`):
  operators' configs depend on them. Don't finish the rename unless asked.

## Upstream baseline

The core and mod-playerbots commits each Hearthside module was last verified against live in one
table in `../mod-hearthside-life/CLAUDE.md` ("Upstream baseline"), with
`../mod-hearthside-life/tools/Get-UpstreamDrift.ps1` to list what changed since. Bump this module's
row there only after checking this module, and commit that change in Life right away (its own commit;
pushing is optional) even if nothing else in Life changed: its history is the baseline record.

## Core API drift

Use the local forms, not upstream's:

- No `WorldSession::IsBot()`: use `CityLife::IsBotSession(player)`. `IsRealPlayer()` alone is wrong
  at login, since mod-playerbots attaches its AI after `OnPlayerLogin`.
- `Acore::StringFormat(...)`, not `Acore::String::Format(...)`.
- `GameTime::GetGameTime()`/`GetGameTimeMS()` return durations: call `.count()`.
- mod-playerbots renamed keys `AiPlayerbot.*` → `Playerbots.*` and `PlayerbotAIConfig` members to
  UpperCamelCase (its PR #2854). Read its current headers first.

## Shared Life headers

- They are `../mod-hearthside-life`'s, included as `../../mod-hearthside-life/src/<header>`;
  `CityLifeTypes.h` `#error`s without that module. Change them there and build City and PvP.
- Upstream commits touching `LifeBotReservation.h` or another `Life*.h` are ported to Life, not here.
- `src/LifeBotReservation.h` is a one-line forwarding header kept only because upstream
  `mod-playerbots-artisans` includes it by this path. Never put code in it.

## Architecture

- **Entry** (`mod_playerbots_city_life.cpp`): WorldScript (config, update, shutdown), PlayerScript
  (logout, trade blocking, achievement suppression), GroupScript (a resident joining a real
  player's group is released at once), `.citylife`. All logic is `CityLife::Manager`
  (`sCityLifeMgr`, `CityLifeMgr.cpp`).
- **Tick:** `Update()` pumps async DB and registry arbitration each frame; every `_tickSeconds` it
  runs `MaintainPopulation()` (demand → hub targets → admission requests) and `MaintainResidents()`
  (activity state machine). `_maxChangesPerTick` caps admissions and releases.
- **Demand:** a hub with a real player asks for its full population; an empty one keeps a floor
  (`BaselinePercent`, the user's choice over an empty city). While watched, a hub changes one
  resident at a time: arrivals walk in (`LifeArrival::FindDropOff`), departures leave via an inn
  bed then a walk-off. Unwatched hubs change at once.
- **Leases:** admission is two-phase (request → Provisional → Active in `AdmitResident`), release is
  staged (`ReleaseResident` → `ProcessReleasingResidents`) so strategies and position are restored
  before the lease drops. Every resident action re-validates its lease; a lost lease means another
  module or a player owns the bot.
- **Files:** `CityLifeActivities.*` (activities), `CityLifeMovement.*` (pathing, water avoidance,
  mounting), `CityLifeTypes.h` (structs, enums), `CityLifePopulation.h` (static tables).
- **Duels belong to PvP.** City keeps only spectators (`WatchDuels` at gate/duel hubs and
  `DuelRing` spots) and medics (`LifeMedic`).
- **Chat integration:** `CityLife.Bots.Archetypes` filters through Life's `LifeArchetypes` (every
  bot fits without a provider). Crowd pairs publish `LifeEvents::CrowdPair`, which Chat turns into a
  /say script. A bot Chat marks busy (`Manager::Talking`) is not moved, re-tasked, released or
  picked until the scene ends.

## Data and SQL

- Hubs and spots: WORLD tables `city_life_hub`/`city_life_spot`. GM edits go through
  `QueueMutation()`; `.citylife spot delete` disables a row rather than deleting it.
- `data/sql/db-world/base/city_life.sql` is the single schema and seed file and is re-applied
  whenever its hash changes, so keep it idempotent (`CREATE TABLE IF NOT EXISTS`,
  `AddColumnUnlessExists_CityLife`, `INSERT IGNORE`). Add to it; don't add `updates/` files.
- `data/sql/manual/` is applied by hand only.

## Conventions

- Logging: the `module` logger with a `[CityLife]` prefix (startup line on `server.loading`).
- A new config key gets a commented entry in `conf/mod_playerbots_city_life.conf.dist` and a read
  in `Manager::LoadConfig()`. Keys shared in meaning with PvP keep the same name after the prefix
  (`Bots.*`, `Population.BudgetMode`, `Antics.*`, `DuelMedics.*`, `Audit.*`).
- Invariants: borrow bots as-is (no level, talent or stat changes); never touch real players or
  their bots; **no bot speech** (`ApplyCityStrategies` strips `chat` and `emote`; speech is Chat's);
  economic interactions default off.
