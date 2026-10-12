/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CITY_LIFE_MGR_H
#define CITY_LIFE_MGR_H

#include "CityLifeTypes.h"
#include "AsyncCallbackProcessor.h"
#include "QueryCallback.h"
#include "Transaction.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChatHandler;
class Field;
class Player;

namespace CityLife
{
    class Manager
    {
    public:
        static Manager& Instance();

        void LoadConfig();
        void Update(uint32 diff);
        void Shutdown();
        bool HandleCommand(ChatHandler* handler, std::string const& text);

        // Interaction & Player Event Callbacks
        void OnPlayerZoneUpdate(Player* player, uint32 zoneId);
        void OnPlayerLogin(Player* player);
        void OnPlayerLogout(Player* player);
        void DisengageResident(uint32 guidLow);
        bool IsResident(uint32 guidLow) const;

        // Activity Helper Methods
        Spot const* FindSpot(uint32 hubId, uint32 spotId) const;
        Player* FindCrowdChatPartner(uint32 hubId, Player* bot);
        void BeginCrowdChat(Player* bot, Player* partner, Resident& resident, uint32 now);
        void ClearResidentChatPartner(uint32 partnerGuidLow, uint32 expectedGuidLow);
        Player* FindDuelOpponent(Hub const& hub, Player* bot, float centerX, float centerY, float radius);
        Resident* FindResident(uint32 guidLow);

        // Config Accessors
        bool IsInteractionsEnabled() const { return _enableInteractions; }
        bool IsPlayerTradingEnabled() const { return _enablePlayerTrading; }
        bool IsBlockAchievementsEnabled() const { return _blockAchievements; }
        void RecordSuppressedAchievement() { ++_suppressedAchievements; }
        void RecordSuppressedCriteria() { ++_suppressedCriteria; }
        bool IsResidentInSyntheticActivity(uint32 guidLow) const;
        bool ValidateResidentAction(Player* bot, Resident const& resident) const;
        bool IsHumanControlled(Player* player) const;

    private:
        Manager() = default;

        bool VerifyDatabase();
        bool LoadData();
        void ImportPlayerbotDefaults();
        bool CheckCoexistence();

        TeamSide TeamForRace(uint8 race) const;
        Player* FindPlayer(uint32 guidLow) const;
        bool IsConfiguredBotAccount(uint32 accountId);
        void RefreshAccountCache();
        bool IsCandidate(Player* player);
        bool IsResidentUsable(Player* player, bool allowOwnedDuel = false) const;
        uint32 GetLiveWorldBotCount(bool forceRefresh = false) const;
        std::vector<BotCandidate> LoadCandidates(Hub const& hub, uint32 poolCount = 0);

        Hub* FindHub(uint32 hubId);
        Hub* FindHub(std::string const& name);
        Hub const* FindHubByZone(uint32 zoneId) const;
        bool IsStaticServiceSpot(Hub const& hub, Spot const& spot) const;
        Spot const* ChooseSpot(Hub const& hub, uint32 ignoredSpotId = 0, bool filterByDistrict = false, bool targetHubDistrict = false, Player* botForPathCheck = nullptr) const;
        bool UsesAlternatingHubRoute(std::string const& hubName) const;
        bool IsHubDistrict(std::string const& hubName, std::string const& district) const;
        bool IsOutlyingDistrict(std::string const& hubName, std::string const& district) const;
        CitizenRole DetermineRole(Hub const& hub, Spot const& spot, BotCandidate const& candidate) const;
        CitizenActivity ChooseInitialActivity(CitizenRole role, Spot const& spot) const;

        uint32 CountResidents(uint32 hubId) const;
        uint32 TargetPopulation(Hub const& hub) const;
        uint32 CurrentTimeMultiplier() const;
        uint32 CalculateTotalBudget(uint32 eligiblePoolCount) const;

        bool AddResident(Hub const& hub, uint32 poolCount = 0);
        bool AdmitResident(Hub const& hub, BotCandidate const& bot, PlayerbotsLife::LeaseToken token);
        void ProcessAdmissionGrants();
        void CancelAdmissions();
        void ReleaseResident(Resident const& resident, bool returnHome);
        void ProcessReleasingResidents(bool isShutdown = false);
        void ReleaseAll(bool returnHome);
        void QueueMutation(std::string const& sql, uint32 gmGuidLow = 0, std::string const& successMsg = "");
        void RemoveUnavailableResidents();
        void MaintainPopulation();
        void MaintainResidents();
        void RelocateResident(Resident& resident, Hub const& hub);
        void AssignSpot(Resident& resident, Hub const& hub, Spot const& spot, bool allowStationary);

        void ApplyCityStrategies(Player* player, Resident& resident) const;
        void SetStayPosition(Player* player, Resident& resident) const;
        void RestoreResidentState(Player* player, Resident const& resident) const;

        // Command Handlers
        void PrintStatus(ChatHandler* handler) const;
        void PrintCompat(ChatHandler* handler) const;
        void PrintHelp(ChatHandler* handler) const;
        void PrintDebugStatus(ChatHandler* handler) const;
        bool HandleHubCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args);

        bool _enable = true;
        bool _databaseReady = false;
        bool _debug = false;
        uint32 _startupDelaySeconds = 90;
        uint32 _tickSeconds = 10;
        uint32 _startupElapsedMs = 0;
        uint32 _timerMs = 0;

        // Population Budget & Demand Profiles
        BudgetMode _budgetMode = BudgetMode::Percentage;
        uint32 _fixedCount = 100;
        uint32 _borrowPercent = 10;
        uint32 _maxTotalPopulation = 120;
        DemandMode _demandMode = DemandMode::Hybrid;
        DistributionMode _distributionMode = DistributionMode::Scaled;
        uint32 _baselinePercent = 20;
        uint32 _burstGraceSeconds = 180;
        uint32 _maxChangesPerTick = 3;

        // Compatibility & Anti-Stealing Barrier
        bool _compatPvPLifeHarmony = true;
        bool _compatPvPLifeRespectReservations = true;

        // Candidate Filters & Safety
        bool _usePlayerbotConfig = true;
        std::string _botAccountPrefix = "auto";
        uint32 _botAccountMin = 0;
        uint32 _botAccountMax = 0;
        uint32 _botQueryLimit = 500;
        bool _skipGroupedBots = true;
        bool _respectPlayerbotActivity = false;
        bool _returnBots = true;

        // Time Scaling
        bool _timeScaling = true;
        uint32 _morningMultiplier = 60;
        uint32 _dayMultiplier = 80;
        uint32 _eveningMultiplier = 100;
        uint32 _nightMultiplier = 25;

        // Behavior & Relocation
        bool _roaming = true;
        uint32 _relocateMinSeconds = 180;
        uint32 _relocateMaxSeconds = 600;
        uint32 _leashRadius = 35;
        bool _emotes = true;
        uint32 _emoteChance = 35;
        uint32 _emoteMinSeconds = 120;
        uint32 _emoteMaxSeconds = 360;
        uint32 _serviceSpotWeightMultiplier = 250;
        uint32 _staticServiceChance = 80;
        std::unordered_set<std::string> _staticServiceHubs;
        std::unordered_set<std::string> _staticServiceCategories;
        std::string _nonCombatStrategies = "+stay,-follow,-passive,-grind,-rpg,-travel,-duel,-pvp";

        // Activity Switches
        bool _actLeaveInn = true;
        bool _actReturnToInn = true;
        bool _actRoaming = true;
        bool _actWallWalking = true;
        bool _actTavernSitting = true;
        bool _actTavernDrinking = true;
        bool _actRandomEmotes = true;
        bool _actAuctionHouse = true;
        bool _actMailbox = true;
        bool _actTrainer = true;
        bool _actFoodVendor = true;
        bool _actDuelWatching = true;
        bool _actFishing = true;
        bool _actDuels = true;
        bool _actInnDancing = true;
        bool _actPlazaSocial = true;
        bool _actCrowdScenes = true;
        bool _actMountedTravel = true;

        // Living Crowd Settings
        bool _crowdEnable = true;
        float _crowdPlayerRadius = 60.0f;
        uint32 _crowdChatCooldownSec = 90;
        uint32 _crowdAfkMinSec = 60;
        uint32 _crowdAfkMaxSec = 180;
        bool _activitiesAfkEnable = true;

        // Duels Settings
        uint32 _duelCooldownSec = 45;
        bool _duelAllowPlayerChallenge = true;
        bool _duelBotChallengeRealPlayer = true;
        float _duelMaxDistance = 40.0f;

        // Movement & Sink Settings
        uint32 _mountBetweenPoiChance = 90;
        bool _sinkForensics = true;
        uint32 _maxPathRefuseStreak = 3;

        // Chat & Interaction Settings
        bool _chatScriptedConversations = true;
        bool _chatGreetings = true;
        bool _chatUnrestricted = false;
        uint32 _chatRandomSayChance = 0;
        uint32 _chatSayCooldownSec = 30;
        bool _enableInteractions = false;
        bool _enablePlayerTrading = false;
        bool _blockAchievements = false;
        uint32 _suppressedAchievements = 0;
        uint32 _suppressedCriteria = 0;

        // Advanced Micro-Behaviors (Ported from mod-city-bots)
        bool _alternatingDistricts = true;
        bool _duelMeetingPoint = true;
        bool _duelTacticalRange = true;
        bool _waterAvoidance = true;

        std::vector<Hub> _hubs;
        struct ReleasingResident
        {
            Resident Actor;
            bool ReturnHome = false;
            uint32 ReleaseStartedAt = 0;
            uint32 Attempts = 0;
        };
        std::vector<ReleasingResident> _releasingResidents;
        std::vector<Resident> _residents;
        std::unordered_map<uint32, uint32> _hubResidentCounts;
        std::unordered_map<uint32, bool> _botAccountCache;
        mutable std::mutex _hubRealPlayerMutex;
        mutable std::unordered_map<uint32, uint32> _hubLastRealPlayerTime; // HubId -> timestamp
        mutable std::unordered_map<uint32, std::vector<uint32>> _zoneToHubIds; // ZoneId -> HubIds (thread-safe lookup)
        mutable uint32 _cachedLiveBotCount = 0;
        mutable uint32 _lastCensusTimeMs = 0;
        mutable std::vector<uint32> _cachedOnlineGuids;
        mutable uint32 _lastCandidateScanMs = 0;
        uint64 _nextAssignmentId = 1;
        QueryCallbackProcessor _queryProcessor;
        AsyncCallbackProcessor<TransactionCallback> _transactionProcessor;
        uint32 _pendingDbWrites = 0;
        uint32 _releasingTimerMs = 0;
        bool _releasingDirty = false;
        uint64 _dataGeneration = 0;
        struct PendingAdmission
        {
            uint64 RequestId = 0;
            uint32 HubId = 0;
            uint32 Deadline = 0;
            BotCandidate Bot;
            PlayerbotsLife::LeaseToken Token;
        };
        std::vector<PendingAdmission> _pendingAdmissions;
        std::unordered_map<uint32, uint32> _hubTargets;
        uint32 _changesThisTick = 0;
    };
}

#define sCityLifeMgr CityLife::Manager::Instance()

#endif
