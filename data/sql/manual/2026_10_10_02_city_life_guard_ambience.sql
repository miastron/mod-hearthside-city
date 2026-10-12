-- mod-playerbots-city-life
-- Optional Guard Ambiance SmartAI Scripts
-- Apply to AzerothCore WORLD database for optional guard flavor.
-- Entries 68, 1976 and 3296 already run SmartAI in the stock world database (their own rows use
-- ids below 100); these rows add a repeating out-of-combat timer (event 1, not flagged
-- NOT_REPEATABLE) linked to SMART_ACTION_PLAY_EMOTE (5): 66 = salute, 2 = bow.

-- Stormwind Guards (entry 68) & Patrollers (entry 1976)
DELETE FROM `smart_scripts` WHERE `entryorguid` IN (68, 1976) AND `source_type` = 0 AND `id` BETWEEN 100 AND 102;

INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, `event_chance`, `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`, `action_param6`, `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`, `target_y`, `target_z`, `target_o`, `comment`) VALUES
(68, 0, 100, 101, 1, 0, 100, 0, 30000, 90000, 120000, 240000, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Stormwind Guard - random timer -> link 101'),
(68, 0, 101, 0, 61, 0, 100, 0, 0, 0, 0, 0, 0, 5, 66, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Stormwind Guard - emote salute'),

(1976, 0, 100, 101, 1, 0, 100, 0, 45000, 120000, 150000, 300000, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Stormwind Patroller - random timer -> link 101'),
(1976, 0, 101, 0, 61, 0, 100, 0, 0, 0, 0, 0, 0, 5, 2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Stormwind Patroller - emote bow');

-- Orgrimmar Grunts (entry 3296)
DELETE FROM `smart_scripts` WHERE `entryorguid` = 3296 AND `source_type` = 0 AND `id` BETWEEN 100 AND 102;

INSERT INTO `smart_scripts` (`entryorguid`, `source_type`, `id`, `link`, `event_type`, `event_phase_mask`, `event_chance`, `event_flags`, `event_param1`, `event_param2`, `event_param3`, `event_param4`, `event_param5`, `action_type`, `action_param1`, `action_param2`, `action_param3`, `action_param4`, `action_param5`, `action_param6`, `target_type`, `target_param1`, `target_param2`, `target_param3`, `target_param4`, `target_x`, `target_y`, `target_z`, `target_o`, `comment`) VALUES
(3296, 0, 100, 101, 1, 0, 100, 0, 30000, 90000, 120000, 240000, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Orgrimmar Grunt - random timer -> link 101'),
(3296, 0, 101, 0, 61, 0, 100, 0, 0, 0, 0, 0, 0, 5, 66, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'CityLife: Orgrimmar Grunt - emote salute');
