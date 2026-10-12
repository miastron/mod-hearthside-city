# City Life (Comprehensive Autonomous Urban Population & Roleplay)

![City Life Banner](assets/banner.png)

Repository/module name: `mod-playerbots-city-life`  
Created by **iCore**. Incorporates 100% of capabilities and micro-behaviors from `mod-city-bots`.

City Life populates capitals, towns, and popular social hubs with configurable AzerothCore Playerbots. It makes cities feel lively, bustling, and immersive: residents wander between districts, shop at market stalls, socialize in taverns, fish along city waterways, and spar in duel rituals outside capital gates.

**Single Installed Module Requirement:** Users install only `mod-playerbots-city-life` alongside AzerothCore and `mod-playerbots`. `mod-city-bots` is completely unnecessary and should not be installed or enabled.

---

## Key Features & Subsystems

- **Living Urban Population:** Per-hub population settings across 16+ bundled cities and hubs (Stormwind, Orgrimmar, Ironforge, Undercity, Darnassus, Thunder Bluff, Exodar, Silvermoon, Shattrath, Dalaran, Gadgetzan, Goldshire, Honor Hold, Thrallmar, Wintergrasp camps).
- **Multi-District Exploration:** Bots periodically explore alternating districts (e.g. Mage Quarter -> Trade District -> Old Town -> Cathedral Square) rather than staying fixed to one area.
- **Capital Gate Duel Rituals:** Bots organize friendly duels outside Stormwind and Orgrimmar gates, calculating a fair midpoint rendezvous, planting duel flags, and fighting until victory/defeat.
- **Tactical Combat Range-Keeping:** Duel participants respect tactical spacing—melee bots press close, while casters and hunters backpedal to keep optimal 15–25 yard engagement distance.
- **Canal Water Avoidance:** Automated terrain and water-depth detection prevents bots from falling into Stormwind canals or drowning, automatically snapping waypoints to dry quays and stone bridges.
- **Weighted Hotspot Distribution:** Dynamic distribution across functional categories: `auction`, `bank`, `mail`, `flight`, `square`, `tavern`, and `travel`. Auction house and bank spots receive boosted weighting with stationary patron ratios.
- **Time-of-Day Dynamic Scaling:** Realistic morning, daytime, evening, and nighttime population curves with gradual arrivals and departures.
- **Mutual Anti-Stealing Reservation Barrier:** Thread-safe, standalone-safe shared reservation registry (`PlayerbotsLife::ReservationRegistry`) ensuring bots reserved by City Life are never stolen or commanded by `mod-playerbots-pvp-life`, and vice versa.
- **Conservative Population Borrowing Budget:** Strict default cap of 10% eligible pool / 120 bots max (configured for a 1,000-bot server) guarantees that ≥ 80% (800+ bots) of random playerbots remain completely free to roam, quest, run dungeons, and engage in open-world activities.
- **Zero Bot Tampering Invariant:** Bots are borrowed strictly AS-IS from the natural Playerbot pool with zero artificial level modifications, zero talent changes, and zero stat inflation.
- **Zero GM Spam Protection:** Game Masters and administrators are strictly exempt from unsolicited duel challenges, whispers, or broadcasts.
- **Silent-by-Default Operation:** All ambient chatter, scripted dialogues, and greetings default to OFF (`0`) in config for a completely unobtrusive server experience out of the box.
- **Instant Disengagement on Invite or Logout:** Residents immediately drop city behaviors if invited to a player group, commanded by an owner, or logged out.
- **Shared Duel Staging Lattice (20yd Spacing):** Prevents duel ring overlap outside capital gates when running alongside `mod-playerbots-pvp-life`.
- **Zero Feature Suppression Coexistence:** Fully compatible with `mod-playerbots-pvp-life`. Both modules can run simultaneously—including dueling outside capital gates—without disabling or yielding any features.
- **Comprehensive GM Commands:** Real-time runtime tools for tuning hubs, recording exact hotspot coordinates in-game, and monitoring system health.

---

## Requirements

- [`mod-playerbots/azerothcore-wotlk`, Playerbot branch](https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot)
- [`mod-playerbots/mod-playerbots`, master branch](https://github.com/mod-playerbots/mod-playerbots)
- C++17-capable build environment (AzerothCore baseline).
- MySQL database (AzerothCore baseline).

---

## Installation & Setup

1. Place `mod-playerbots-city-life` into your `modules/` directory:
   ```text
   azerothcore-wotlk/modules/mod-playerbots-city-life
   ```
2. Apply the schema and seed data to your **WORLD** database:
   ```bash
   mysql -u root -p acore_world < data/sql/manual/world_city_life.sql
   ```
3. Re-run CMake and rebuild AzerothCore.
4. Copy `conf/mod_playerbots_city_life.conf.dist` to `mod_playerbots_city_life.conf` in your worldserver config directory and customize settings.
5. Start worldserver and look for `>> Loaded mod-playerbots-city-life` in your startup log.

---

## Coexistence with `mod-playerbots-pvp-life`

`mod-playerbots-city-life` and `mod-playerbots-pvp-life` are architected for seamless, zero-conflict simultaneous execution:
- **Zero Feature Suppression:** Neither module disables or yields any feature for the other.
- **Concurrent Capital Gate Duels:** Both modules host dueling outside city gates simultaneously. Each module pairs and manages only its own claimed bots.
- **Candidate Scanning Isolation:** During candidate scanning, each module verifies `!PlayerbotsLife::IsReserved(guid)` before borrowing a bot.
- **Shared Reservation Registry:** `src/LifeBotReservation.h` provides an atomic, mutex-guarded process-wide singleton that registers active modules and tracks ownership cleanly.
- **Independent Execution:** If only one module is installed, it runs completely standalone with zero dependencies or overhead.

---

## GM Commands Reference

All administrative commands use the `.citylife` root:

### Status & Diagnostics
- `.citylife status`: Displays total active residents, hub breakdowns, activity states, and borrowing budgets.
- `.citylife compat`: Reports detected active Life modules (`CityLife`, `PvPLife`, `Artisans`) and reservation pool statistics.
- `.citylife reload`: Reloads configuration and all hub/hotspot data from the database.

### Hub Management
- `.citylife hub list`: Lists all configured city hubs and their target populations.
- `.citylife hub add <name> <alliance|horde|any> <minLvl> <maxLvl> <pop> [priority]`: Creates a new hub at the GM's current position.
- `.citylife hub population <name> <count>`: Adjusts the default population for a hub.
- `.citylife hub enable <name>`: Enables a hub.
- `.citylife hub disable <name>`: Disables a hub.

### Hotspot Management
- `.citylife spot list <hub>`: Lists all hotspots defined for a hub.
- `.citylife spot add <hub> <name> <category> [weight] [radius]`: Creates or updates a hotspot at the GM's current position.
- `.citylife spot delete <hub> <name>`: Deletes a hotspot from a hub.

---

## Scope & Philosophy

City Life intentionally does not:
- Create auction listings or buy items from real players;
- Simulate banking or mailbox transactions;
- Create or force-login offline characters;
- Replace core Playerbots combat AI or talent trees;
- Teleport or interfere with real players and player-owned bots.

Its focus is singular and polished: bring Azeroth's cities to vibrant, believable life while preserving server performance and game integrity.

---

## Credits & Attribution

- **City Life Architecture & Implementation:** Created by **iCore**.
- **Micro-Behaviors & Waypoint Navigation:** Ported and expanded from `mod-city-bots`.
- **Cross-Module Reservation Protocol:** Designed for the Playerbots Life ecosystem (`mod-playerbots-city-life`, `mod-playerbots-pvp-life`, `mod-playerbots-artisans`).
