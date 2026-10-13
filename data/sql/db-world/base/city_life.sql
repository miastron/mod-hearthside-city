-- mod-hearthside-city: world database (city_life_hub, city_life_spot).
-- One idempotent file, applied by AzerothCore's updater and re-applied whenever it changes:
-- tables are created only if missing, missing columns are added to older installs, seed rows
-- are INSERT IGNORE so a hub or spot an operator has edited keeps its values, and the UPDATEs
-- below only touch values still at their seeded default. A spot removed with
-- `.citylife spot delete` is disabled rather than deleted, so a re-apply does not bring it back.
-- Folded from the upstream mod-playerbots-city-life manual + updates scripts, none of which had
-- been applied through the updater.

-- 1. Idempotent base table creation
CREATE TABLE IF NOT EXISTS `city_life_hub` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name` VARCHAR(64) NOT NULL,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `team` TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '0=Alliance,1=Horde,2=Any',
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `max_level` TINYINT UNSIGNED NOT NULL DEFAULT 80,
  `map_id` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `default_population` SMALLINT UNSIGNED NOT NULL DEFAULT 10,
  `priority` SMALLINT UNSIGNED NOT NULL DEFAULT 100,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_city_life_hub_name` (`name`),
  KEY `idx_city_life_hub_enabled_priority` (`enabled`,`priority`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `city_life_spot` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `hub_id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(64) NOT NULL,
  `category` VARCHAR(32) NOT NULL DEFAULT 'square',
  `poi_type` TINYINT UNSIGNED NOT NULL DEFAULT 7 COMMENT '0=Inn,1=AH,2=Mailbox,3=Tavern,4=Trainer,5=Dock,6=WallPatrol,7=Plaza,8=Vendor,9=DuelRing,10=InnBed,11=Bank',
  `district` VARCHAR(32) NOT NULL DEFAULT '',
  `partner_spot_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `x` FLOAT NOT NULL DEFAULT 0,
  `y` FLOAT NOT NULL DEFAULT 0,
  `z` FLOAT NOT NULL DEFAULT 0,
  `o` FLOAT NOT NULL DEFAULT 0,
  `radius` FLOAT UNSIGNED NOT NULL DEFAULT 4,
  `weight` SMALLINT UNSIGNED NOT NULL DEFAULT 100,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uq_city_life_spot_name` (`hub_id`,`name`),
  KEY `idx_city_life_spot_hub_enabled` (`hub_id`,`enabled`),
  CONSTRAINT `fk_city_life_spot_hub` FOREIGN KEY (`hub_id`) REFERENCES `city_life_hub` (`id`)
    ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 2. Idempotent column additions for pre-existing installations
DROP PROCEDURE IF EXISTS `AddColumnUnlessExists_CityLife`;
DELIMITER //
CREATE PROCEDURE `AddColumnUnlessExists_CityLife`(
    IN tableName VARCHAR(64),
    IN colName VARCHAR(64),
    IN colDef VARCHAR(255)
)
BEGIN
    IF NOT EXISTS (
        SELECT * FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
        AND TABLE_NAME = tableName
        AND COLUMN_NAME = colName
    ) THEN
        SET @sql = CONCAT('ALTER TABLE `', tableName, '` ADD COLUMN `', colName, '` ', colDef);
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
END //
DELIMITER ;

CALL AddColumnUnlessExists_CityLife('city_life_hub', 'zone_id', 'INT UNSIGNED NOT NULL DEFAULT 0 AFTER `map_id`');
CALL AddColumnUnlessExists_CityLife('city_life_spot', 'poi_type', 'TINYINT UNSIGNED NOT NULL DEFAULT 7 COMMENT \'0=Inn,1=AH,2=Mailbox,3=Tavern,4=Trainer,5=Dock,6=WallPatrol,7=Plaza,8=Vendor,9=DuelRing,10=InnBed,11=Bank\' AFTER `category`');
CALL AddColumnUnlessExists_CityLife('city_life_spot', 'district', 'VARCHAR(32) NOT NULL DEFAULT \'\' AFTER `poi_type`');
CALL AddColumnUnlessExists_CityLife('city_life_spot', 'partner_spot_id', 'INT UNSIGNED NOT NULL DEFAULT 0 AFTER `district`');

DROP PROCEDURE IF EXISTS `AddColumnUnlessExists_CityLife`;

-- 3. Re-applicable seed hubs. Config values override default_population at runtime.
INSERT IGNORE INTO `city_life_hub`
(`name`,`enabled`,`team`,`min_level`,`max_level`,`map_id`,`default_population`,`priority`)
VALUES
('Stormwind',1,0,1,80,0,25,220),
('Orgrimmar',1,1,1,80,1,25,220),
('Ironforge',1,0,1,80,0,18,190),
('Undercity',1,1,1,80,0,18,190),
('Darnassus',1,0,1,80,1,10,150),
('ThunderBluff',1,1,1,80,1,10,150),
('Exodar',1,0,1,80,530,5,120),
('Silvermoon',1,1,1,80,530,8,130),
('Shattrath',1,2,58,80,530,20,200),
('Dalaran',1,2,68,80,571,25,210),
('WintergraspAlliance',0,0,70,80,571,0,230),
('WintergraspHorde',0,1,70,80,571,0,230),
('Gadgetzan',1,2,35,80,1,8,110),
('Goldshire',1,0,1,30,0,8,100),
('HonorHold',1,0,58,80,530,6,100),
('Thrallmar',1,1,58,80,530,6,100);

-- 4. Insert bundled spots idempotently. Preserves existing custom spots and customized seed values.
-- Coordinates are based on AzerothCore 3.3.5 game_tele and service NPC/gameobject positions.
-- Use .citylife spot add in game to tune a point for a different navmesh or custom world database.
INSERT IGNORE INTO `city_life_spot`
(`hub_id`,`name`,`category`,`enabled`,`x`,`y`,`z`,`o`,`radius`,`weight`)
SELECT `hub`.`id`,`seed`.`spot_name`,`seed`.`category`,1,`seed`.`x`,`seed`.`y`,`seed`.`z`,`seed`.`o`,
       `seed`.`radius`,`seed`.`weight`
FROM `city_life_hub` AS `hub`
INNER JOIN
(
  SELECT 'Stormwind' AS `hub_name`,'MainSquare' AS `spot_name`,'square' AS `category`,
         -8833.38 AS `x`,628.628 AS `y`,94.0066 AS `z`,1.06535 AS `o`,8 AS `radius`,100 AS `weight`
  UNION ALL SELECT 'Stormwind','AuctionHouse','auction',-8821.53,659.886,97.4645,0.401426,5,150
  UNION ALL SELECT 'Stormwind','Bank','bank',-8931.23,605.801,99.606,0.488692,5,125
  UNION ALL SELECT 'Stormwind','Mailbox','mail',-8861.5,636.744,96.1785,1.91113,5,120
  UNION ALL SELECT 'Stormwind','FlightMaster','flight',-8835.76,490.084,109.699,4.04916,5,65

  UNION ALL SELECT 'Orgrimmar','MainSquare','square',1629.85,-4373.64,31.5573,3.69762,8,100
  UNION ALL SELECT 'Orgrimmar','AuctionHouse','auction',1592.8,-4397.05,7.46388,0.139626,5,150
  UNION ALL SELECT 'Orgrimmar','Bank','bank',1627.42,-4376.04,12.0548,3.68265,5,125
  UNION ALL SELECT 'Orgrimmar','Mailbox','mail',1607.49,-4374.26,9.76904,3.28996,5,120
  UNION ALL SELECT 'Orgrimmar','FlightMaster','flight',1676.25,-4313.45,61.8944,5.25344,5,65

  UNION ALL SELECT 'Ironforge','MainSquare','square',-4918.88,-940.406,501.564,5.42347,8,100
  UNION ALL SELECT 'Ironforge','AuctionHouse','auction',-4948.01,-901.528,505.172,3.80482,5,150
  UNION ALL SELECT 'Ironforge','Bank','bank',-4877.43,-990.034,504.024,2.30383,5,125
  UNION ALL SELECT 'Ironforge','Mailbox','mail',-4910.38,-976.212,501.408,2.26893,5,120
  UNION ALL SELECT 'Ironforge','FlightMaster','flight',-4821.13,-1152.4,502.295,4.31096,5,65

  UNION ALL SELECT 'Undercity','MainSquare','square',1584.14,240.308,-52.1534,0.041793,8,100
  UNION ALL SELECT 'Undercity','AuctionHouse','auction',1542.45,255.202,-56.7948,1.01229,5,150
  UNION ALL SELECT 'Undercity','Bank','bank',1591.49,240.328,-52.0596,3.03687,5,125
  UNION ALL SELECT 'Undercity','Mailbox','mail',1554.97,235.108,-43.201,0.252436,5,120
  UNION ALL SELECT 'Undercity','FlightMaster','flight',1567.12,266.345,-43.0194,0.820305,5,65

  UNION ALL SELECT 'Darnassus','MainSquare','square',9949.56,2284.21,1341.4,1.59587,8,100
  UNION ALL SELECT 'Darnassus','AuctionHouse','auction',9872.6,2341.73,1321.67,3.52556,5,150
  UNION ALL SELECT 'Darnassus','Bank','bank',9945.15,2518.39,1317.66,3.9968,5,125
  UNION ALL SELECT 'Darnassus','Mailbox','mail',9916.29,2348.2,1330.7,3.18527,5,120

  UNION ALL SELECT 'ThunderBluff','MainRise','square',-1277.37,124.804,131.287,5.22274,8,100
  UNION ALL SELECT 'ThunderBluff','AuctionHouse','auction',-1210.21,94.8587,134.535,2.98451,5,150
  UNION ALL SELECT 'ThunderBluff','Bank','bank',-1262.78,24.0055,128.27,0.10472,5,125
  UNION ALL SELECT 'ThunderBluff','Mailbox','mail',-1263.31,44.5451,127.545,4.72984,5,120
  UNION ALL SELECT 'ThunderBluff','FlightMaster','flight',-1196.75,26.0777,177.033,1.71042,5,65

  UNION ALL SELECT 'Exodar','MainSquare','square',-3965.7,-11653.6,-138.844,0.852154,8,100
  UNION ALL SELECT 'Exodar','AuctionHouse','auction',-4027.62,-11732.4,-151.822,0.436332,5,150
  UNION ALL SELECT 'Exodar','Bank','bank',-3923.77,-11544.5,-150.193,4.58778,5,125
  UNION ALL SELECT 'Exodar','Mailbox','mail',-3975.04,-11700,-139.258,3.14157,5,120
  UNION ALL SELECT 'Exodar','FlightMaster','flight',-4057.15,-11788.6,8.87662,5.59502,5,65

  UNION ALL SELECT 'Silvermoon','MainSquare','square',9487.69,-7279.2,14.2866,6.16478,8,100
  UNION ALL SELECT 'Silvermoon','AuctionHouse','auction',9641.82,-7135.55,16.8566,3.19395,5,150
  UNION ALL SELECT 'Silvermoon','Bank','bank',9515.41,-7221.62,16.2139,1.50098,5,125
  UNION ALL SELECT 'Silvermoon','Mailbox','mail',9515.66,-7262.47,14.1913,4.72984,5,120
  UNION ALL SELECT 'Silvermoon','FlightMaster','flight',9376.4,-7164.92,9.0194,3.1765,5,65

  UNION ALL SELECT 'Shattrath','TerraceOfLight','square',-1838.16,5301.79,-12.428,5.9517,10,130
  UNION ALL SELECT 'Shattrath','Bank','bank',-2000.66,5351.82,-9.26761,3.52556,6,125
  UNION ALL SELECT 'Shattrath','Mailbox','mail',-1773.68,5171.82,-40.2134,2.24275,6,120
  UNION ALL SELECT 'Shattrath','FlightMaster','flight',-1831.95,5298.3,-12.3448,1.76278,6,85

  UNION ALL SELECT 'Dalaran','MainSquare','square',5807.98,588.487,660.94,1.66594,10,130
  UNION ALL SELECT 'Dalaran','AuctionHouse','auction',5927.63,731.576,643.253,4.69494,5,150
  UNION ALL SELECT 'Dalaran','AllianceBank','bank',5766.59,734.037,620.134,5.07891,6,110
  UNION ALL SELECT 'Dalaran','HordeBank','bank',5981.66,599.832,651.223,2.75762,6,110
  UNION ALL SELECT 'Dalaran','Mailbox','mail',5797.91,558.436,650.719,4.36332,5,120
  UNION ALL SELECT 'Dalaran','FlightMaster','flight',5813.37,453.403,658.834,4.45059,6,80

  -- Faction-separated Wintergrasp starting graveyards from BattlefieldWG.
  UNION ALL SELECT 'WintergraspAlliance','AllianceStart','square',5140.79,2179.12,390.95,1.97222,24,100
  UNION ALL SELECT 'WintergraspHorde','HordeStart','square',5032.454,3711.382,372.468,3.971623,24,100

  UNION ALL SELECT 'Gadgetzan','MainSquare','square',-7177.15,-3785.34,8.36981,6.10237,8,110
  UNION ALL SELECT 'Gadgetzan','AuctionHouse','auction',-7239.1,-3803.89,0.813843,0.017453,5,145
  UNION ALL SELECT 'Gadgetzan','Bank','bank',-7205.06,-3827.29,8.6442,0.174533,5,120
  UNION ALL SELECT 'Gadgetzan','Mailbox','mail',-7154.4,-3829.52,8.75029,4.2237,5,115
  UNION ALL SELECT 'Gadgetzan','FlightMaster','flight',-7224.87,-3738.21,8.48369,1.18682,5,75

  UNION ALL SELECT 'Goldshire','InnSquare','square',-9448.55,68.236,56.3225,2.1115,8,135
  UNION ALL SELECT 'Goldshire','Mailbox','mail',-9455.99,45.8229,56.4395,1.40499,5,120
  UNION ALL SELECT 'Goldshire','Road','travel',-9465,64,56,0,8,80

  UNION ALL SELECT 'HonorHold','KeepSquare','square',-748.211,2681.52,100.35,5.7479,8,130
  UNION ALL SELECT 'HonorHold','Mailbox','mail',-706.554,2700.94,94.538,1.4943,5,115
  UNION ALL SELECT 'HonorHold','FlightMaster','flight',-665.804,2715.48,94.1981,3.35103,5,85

  UNION ALL SELECT 'Thrallmar','MainSquare','square',156.251,2673.45,85.1587,0.382074,8,130
  UNION ALL SELECT 'Thrallmar','Mailbox','mail',172.726,2623.06,86.8361,3.56983,5,115
  UNION ALL SELECT 'Thrallmar','FlightMaster','flight',233.137,2632.3,88.3007,2.67035,5,85
) AS `seed` ON `seed`.`hub_name`=`hub`.`name`;

-- 5. Fill in zone_ids the seed above leaves at 0 (an operator-set zone_id is kept)
UPDATE `city_life_hub` SET `zone_id` = 1519 WHERE `name` = 'Stormwind' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 1637 WHERE `name` = 'Orgrimmar' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 1537 WHERE `name` = 'Ironforge' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 1497 WHERE `name` = 'Undercity' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 1657 WHERE `name` = 'Darnassus' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 1638 WHERE `name` = 'ThunderBluff' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 3557 WHERE `name` = 'Exodar' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 3487 WHERE `name` = 'Silvermoon' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 3703 WHERE `name` = 'Shattrath' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 4395 WHERE `name` = 'Dalaran' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 4197 WHERE `name` IN ('WintergraspAlliance', 'WintergraspHorde') AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 440 WHERE `name` = 'Gadgetzan' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 12 WHERE `name` = 'Goldshire' AND `zone_id` = 0;
UPDATE `city_life_hub` SET `zone_id` = 3483 WHERE `name` IN ('HonorHold', 'Thrallmar') AND `zone_id` = 0;

-- 6. Seed the duel-ground hubs. The duels there are mod-hearthside-pvp's (its StormwindDuel,
-- OrgrimmarDuel and GoldshireDuel zones); City Life residents of these hubs are spectators.
INSERT IGNORE INTO `city_life_hub`
(`name`, `enabled`, `team`, `min_level`, `max_level`, `map_id`, `zone_id`, `default_population`, `priority`)
VALUES
('StormwindGate', 1, 0, 10, 80, 0, 12, 4, 215),
('OrgrimmarGate', 1, 1, 10, 80, 1, 14, 4, 215),
('GoldshireDuel', 1, 0, 1, 80, 0, 12, 3, 140);

-- 7. Insert curated POIs across all capitals and duel hubs

-- Stormwind POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Inn_Trade', 'inn', 1, -8867.0, 673.0, 97.9, 0.0, 4, 100, 0, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_AuctionHouse', 'auction', 1, -8813.0, 663.0, 95.0, 0.0, 5, 100, 1, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Mailbox_Trade', 'mailbox', 1, -8815.0, 667.0, 95.0, 0.0, 3, 100, 2, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Tavern_Trade', 'tavern', 1, -8865.0, 650.0, 96.0, 0.0, 4, 100, 3, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Plaza_Trade', 'square', 1, -8835.0, 625.0, 94.0, 0.0, 8, 100, 7, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Bank_Trade', 'bank', 1, -8918.0, 624.0, 99.5, 0.0, 5, 120, 11, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Docks_Fish1', 'dock', 1, -8420.0, 1320.0, 5.0, 0.0, 4, 100, 5, 'Harbor', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Docks_Fish2', 'dock', 1, -8426.0, 1325.0, 5.0, 0.0, 4, 100, 5, 'Harbor', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Walls_Patrol', 'wall', 1, -8430.0, 400.0, 120.0, 0.0, 6, 100, 6, 'Walls', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_OldTown_Square', 'square', 1, -8752.0, 780.0, 97.0, 0.0, 6, 90, 7, 'OldTown', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_MageQuarter_Square', 'square', 1, -9012.0, 873.0, 29.5, 0.0, 6, 90, 7, 'MageQuarter', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

-- Orgrimmar POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Inn_Valley', 'inn', 1, 1634.0, -4445.0, 15.0, 0.0, 4, 100, 0, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_AuctionHouse', 'auction', 1, 1679.0, -4455.0, 18.0, 0.0, 5, 100, 1, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Bank_Valley', 'bank', 1, 1627.0, -4378.0, 16.0, 0.0, 5, 120, 11, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Plaza_Center', 'square', 1, 1600.0, -4370.0, 10.0, 0.0, 8, 100, 7, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Docks_Fish', 'dock', 1, 1991.75, -4659.48, 27.38, 3.85, 4, 100, 5, 'ValleyOfHonor', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Cleft_Patrol', 'wall', 1, 1400.0, -4400.0, 55.0, 0.0, 6, 100, 6, 'Cleft', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

-- Goldshire POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_LionsPride_Inn', 'inn', 1, -9469.0, 62.0, 56.0, 0.0, 4, 100, 0, 'LionsPride', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_LionsPride_Tavern', 'tavern', 1, -9466.0, 58.0, 56.0, 0.0, 4, 100, 3, 'LionsPride', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_Square_Center', 'square', 1, -9458.0, 68.0, 56.0, 0.0, 6, 100, 7, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_Dance_FloorUp', 'inn', 1, -9472.58, 37.34, 64.69, 4.65, 3, 100, 0, 'LionsPrideUp', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_CrystalLake_Fish', 'dock', 1, -9432.98, 147.82, 56.40, 1.77, 4, 100, 5, 'CrystalLake', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

-- Duel Ring POIs (spectator stands)
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SWGate_DuelRing_1', 'duel', 1, -9126.38, 351.36, 93.62, 0.58, 6, 100, 9, 'GateDuelRing', 0 FROM `city_life_hub` h WHERE h.name='StormwindGate';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SWGate_DuelRing_2', 'duel', 1, -9108.68, 316.85, 93.39, 1.33, 6, 100, 9, 'GateDuelRing', 0 FROM `city_life_hub` h WHERE h.name='StormwindGate';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORGGate_DuelRing_1', 'duel', 1, 1276.26, -4400.97, 26.32, 0.41, 6, 100, 9, 'GateDuelRing', 0 FROM `city_life_hub` h WHERE h.name='OrgrimmarGate';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORGGate_DuelRing_2', 'duel', 1, 1283.00, -4394.50, 26.35, 0.41, 6, 100, 9, 'GateDuelRing', 0 FROM `city_life_hub` h WHERE h.name='OrgrimmarGate';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GSDuel_Ring_1', 'duel', 1, -9462.85, 75.32, 56.45, 1.25, 6, 100, 9, 'GoldshireDuelRing', 0 FROM `city_life_hub` h WHERE h.name='GoldshireDuel';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GSDuel_Ring_2', 'duel', 1, -9450.40, 85.10, 56.55, 3.80, 6, 100, 9, 'GoldshireDuelRing', 0 FROM `city_life_hub` h WHERE h.name='GoldshireDuel';

-- 8. Normalize category synonyms and POI types of spots still at the default poi_type (7)
UPDATE `city_life_spot` SET `category` = 'mailbox', `poi_type` = 2 WHERE `category` = 'mail' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 0 WHERE `category` = 'inn' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 1 WHERE `category` = 'auction' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 2 WHERE `category` = 'mailbox' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 3 WHERE `category` = 'tavern' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 4 WHERE `category` = 'trainer' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 5 WHERE `category` IN ('dock', 'fish', 'fishing') AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 6 WHERE `category` = 'wall' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 8 WHERE `category` = 'vendor' AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 9 WHERE `category` IN ('duel', 'duelring') AND `poi_type` = 7;
UPDATE `city_life_spot` SET `poi_type` = 11 WHERE `category` = 'bank' AND `poi_type` = 7;

-- 9. Spot fixes for older installs (each touches a row only while it still has the old seeded value)
UPDATE `city_life_hub` SET `enabled` = 0, `default_population` = 0
  WHERE `name` IN ('WintergraspAlliance', 'WintergraspHorde') AND `enabled` = 1 AND `default_population` = 40;
UPDATE `city_life_hub` SET `min_level` = 10, `default_population` = 4
  WHERE `name` IN ('StormwindGate', 'OrgrimmarGate') AND `min_level` = 80 AND `max_level` = 80 AND `default_population` = 22;
UPDATE `city_life_hub` SET `min_level` = 1
  WHERE `name` = 'GoldshireDuel' AND `min_level` = 80 AND `max_level` = 80;
UPDATE `city_life_spot` SET `poi_type` = 0 WHERE `name` = 'GS_Dance_FloorUp' AND `poi_type` = 10;

-- 10. Civic POIs from upstream (2026-10-12): class trainers, food vendors, taverns, inn beds,
-- banks, auction houses, mailboxes, plazas and docks across the capitals. Trainer spots carry
-- the district names CityLife's class-district matching looks for. Coordinates are approximate;
-- SnapTargetToGround and the destination checks handle small offsets, and an operator can move a
-- spot with `.citylife spot add` (same name) or disable it with `.citylife spot delete`.

-- Stormwind, Orgrimmar, Ironforge, Undercity, Dalaran: trainers, vendors, taverns, inn beds
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Trainer_OldTown', 'trainer', 1, -8765.0, 748.0, 97.5, 0.0, 5, 100, 4, 'OldTown', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Trainer_MageQuarter', 'trainer', 1, -9005.0, 865.0, 29.6, 0.0, 5, 100, 4, 'MageQuarter', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Vendor_TradeRations', 'vendor', 1, -8845.0, 655.0, 96.0, 0.0, 4, 100, 8, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Inn_TradeBed', 'bed', 1, -8864.0, 678.0, 101.5, 0.0, 3, 100, 10, 'TradeDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Trainer_Valley', 'trainer', 1, 1655.0, -4410.0, 16.0, 0.0, 5, 100, 4, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Vendor_Rations', 'vendor', 1, 1615.0, -4430.0, 15.0, 0.0, 4, 100, 8, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Inn_ValleyBed', 'bed', 1, 1638.0, -4440.0, 16.0, 0.0, 3, 100, 10, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Tavern_Military', 'tavern', 1, -4925.0, -925.0, 502.0, 0.0, 4, 100, 3, 'Commons', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Trainer_Commons', 'trainer', 1, -4890.0, -960.0, 502.0, 0.0, 5, 100, 4, 'Commons', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Vendor_Commons', 'vendor', 1, -4905.0, -930.0, 502.0, 0.0, 4, 100, 8, 'Commons', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Tavern_Center', 'tavern', 1, 1570.0, 250.0, -52.0, 0.0, 4, 100, 3, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Trainer_Apothecary', 'trainer', 1, 1590.0, 260.0, -52.0, 0.0, 5, 100, 4, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Vendor_Center', 'vendor', 1, 1575.0, 230.0, -52.0, 0.0, 4, 100, 8, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'Dala_Tavern_Legerdemain', 'tavern', 1, 5830.0, 625.0, 647.0, 0.0, 4, 100, 3, 'Legerdemain', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'Dala_Trainer_Center', 'trainer', 1, 5850.0, 650.0, 647.0, 0.0, 5, 100, 4, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'Dala_Vendor_Center', 'vendor', 1, 5810.0, 600.0, 647.0, 0.0, 4, 100, 8, 'Center', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

-- Ironforge, Undercity, Darnassus, Exodar, Thunder Bluff, Silvermoon, Shattrath, Dalaran: full set
-- Ironforge POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Inn_GreatForge', 'inn', 1, -4845.0, -880.0, 502.0, 0.0, 4, 100, 0, 'GreatForge', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Auction_GreatForge', 'auction', 1, -4960.0, -910.0, 503.0, 0.0, 5, 100, 1, 'GreatForge', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Tavern_GreatForge', 'tavern', 1, -4850.0, -850.0, 502.0, 0.0, 4, 100, 3, 'GreatForge', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_MysticWard', 'plaza', 1, -4920.0, -990.0, 501.0, 0.0, 5, 100, 7, 'MysticWard', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Dock_Cavern1', 'dock', 1, -4638.0, -1114.0, 501.0, 0.0, 4, 100, 5, 'ForlornCavern', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Dock_Cavern2', 'dock', 1, -4644.0, -1119.0, 501.0, 0.0, 4, 100, 5, 'ForlornCavern', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_Explorers', 'plaza', 1, -4675.0, -1230.0, 501.0, 0.0, 5, 80, 7, 'HallOfExplorers', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Trainer_Military', 'trainer', 1, -5010.0, -1250.0, 501.0, 0.0, 5, 80, 4, 'MilitaryWard', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Vendor_CommonsMarket', 'vendor', 1, -4925.0, -955.0, 501.0, 0.0, 4, 100, 8, 'Commons', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_TinkerTown', 'plaza', 1, -4830.0, -1260.0, 501.0, 0.0, 5, 90, 7, 'TinkerTown', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Trainer_Mystic', 'trainer', 1, -4650.0, -960.0, 501.0, 0.0, 5, 80, 4, 'MysticWard', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_Cavern', 'plaza', 1, -4625.0, -1100.0, 501.0, 0.0, 5, 80, 7, 'ForlornCavern', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_GreatForge', 'plaza', 1, -4780.0, -1000.0, 502.0, 0.0, 5, 100, 7, 'GreatForge', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Plaza_Gates', 'plaza', 1, -5035.0, -820.0, 495.0, 0.0, 5, 70, 7, 'Gates', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Vendor_TinkerTown', 'vendor', 1, -4865.0, -1170.0, 501.0, 0.0, 4, 80, 8, 'TinkerTown', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'IF_Bank_Commons', 'bank', 1, -4890.0, -985.0, 501.0, 0.0, 5, 120, 11, 'Commons', 0 FROM `city_life_hub` h WHERE h.name='Ironforge';

-- Undercity POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Inn_MagicQuarter', 'inn', 1, 1633.0, 240.0, -43.0, 0.0, 4, 100, 0, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Auction_MagicQuarter', 'auction', 1, 1600.0, 170.0, -56.0, 0.0, 5, 100, 1, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Tavern_MagicQuarter', 'tavern', 1, 1630.0, 200.0, -43.0, 0.0, 4, 100, 3, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_MagicQuarter', 'plaza', 1, 1580.0, 260.0, -43.0, 0.0, 5, 100, 7, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_TradeQuarter', 'plaza', 1, 1590.0, 330.0, -43.0, 0.0, 5, 90, 7, 'TradeQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Vendor_RoguesQuarter', 'vendor', 1, 1560.0, 430.0, -43.0, 0.0, 4, 80, 8, 'RoguesQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Trainer_WarQuarter', 'trainer', 1, 1700.0, 90.0, -62.0, 0.0, 5, 80, 4, 'WarQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_Apothecarium', 'plaza', 1, 1460.0, 170.0, -62.0, 0.0, 5, 80, 7, 'Apothecarium', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Vendor_MagicQuarter', 'vendor', 1, 1685.0, 300.0, -43.0, 0.0, 4, 80, 8, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_CanalRing', 'plaza', 1, 1580.0, 410.0, -43.0, 0.0, 5, 80, 7, 'CanalRing', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Trainer_MagicQuarter', 'trainer', 1, 1660.0, 210.0, -43.0, 0.0, 5, 80, 4, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_CentralRing', 'plaza', 1, 1520.0, 250.0, -43.0, 0.0, 5, 90, 7, 'CentralRing', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_TradeOuter', 'plaza', 1, 1640.0, 360.0, -43.0, 0.0, 5, 80, 7, 'TradeQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Vendor_Apothecarium', 'vendor', 1, 1510.0, 115.0, -62.0, 0.0, 4, 70, 8, 'Apothecarium', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Plaza_InnerRing', 'plaza', 1, 1715.0, 245.0, -43.0, 0.0, 5, 80, 7, 'InnerRing', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'UC_Bank_MagicQuarter', 'bank', 1, 1595.0, 230.0, -43.0, 0.0, 5, 110, 11, 'MagicQuarter', 0 FROM `city_life_hub` h WHERE h.name='Undercity';

-- Darnassus POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Inn_Craftsmen', 'inn', 1, 9947.0, 2483.0, 1316.0, 0.0, 4, 100, 0, 'Craftsmen', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Tavern_Craftsmen', 'tavern', 1, 9940.0, 2510.0, 1316.0, 0.0, 4, 100, 3, 'Craftsmen', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Plaza_Temple', 'plaza', 1, 9920.0, 2550.0, 1316.0, 0.0, 5, 100, 7, 'Temple', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Auction_Tradesmens', 'auction', 1, 9860.0, 2330.0, 1315.0, 0.0, 5, 100, 1, 'TradesmensTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Mailbox_Tradesmens', 'mailbox', 1, 9865.0, 2355.0, 1315.0, 0.0, 4, 90, 2, 'TradesmensTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_Warriors', 'trainer', 1, 10040.0, 2530.0, 1316.0, 0.0, 5, 80, 4, 'WarriorsTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Vendor_Warriors', 'vendor', 1, 10110.0, 2570.0, 1316.0, 0.0, 4, 80, 8, 'WarriorsTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Plaza_TempleGardens', 'plaza', 1, 9650.0, 2520.0, 1336.0, 0.0, 5, 80, 7, 'TempleGardens', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_TempleGardens', 'trainer', 1, 9705.0, 2510.0, 1335.0, 0.0, 5, 80, 4, 'TempleGardens', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Plaza_Bridge', 'plaza', 1, 9790.0, 2590.0, 1316.0, 0.0, 5, 70, 7, 'Bridge', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Vendor_Cenarion', 'vendor', 1, 9910.0, 2260.0, 1328.0, 0.0, 4, 80, 8, 'CenarionEnclave', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_Cenarion', 'trainer', 1, 10080.0, 2205.0, 1328.0, 0.0, 5, 80, 4, 'CenarionEnclave', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Plaza_Cenarion', 'plaza', 1, 9945.0, 2185.0, 1328.0, 0.0, 5, 80, 7, 'CenarionEnclave', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Plaza_Pool', 'plaza', 1, 9750.0, 2435.0, 1316.0, 0.0, 5, 70, 7, 'Pool', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Vendor_TerraceMarket', 'vendor', 1, 10010.0, 2425.0, 1316.0, 0.0, 4, 80, 8, 'TerraceMarket', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Bank_Tradesmens', 'bank', 1, 9865.0, 2325.0, 1315.0, 0.0, 5, 110, 11, 'TradesmensTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

-- Exodar POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Inn_Seat', 'inn', 1, -3965.0, -11653.0, -139.0, 0.0, 4, 100, 0, 'Seat', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Tavern_Seat', 'tavern', 1, -3950.0, -11680.0, -139.0, 0.0, 4, 100, 3, 'Seat', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Plaza_Vault', 'plaza', 1, -4000.0, -11800.0, -100.0, 0.0, 5, 100, 7, 'Vault', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Auction_CrystalHall', 'auction', 1, -4020.0, -11720.0, -138.0, 0.0, 5, 100, 1, 'CrystalHall', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Mailbox_CrystalHall', 'mailbox', 1, -4050.0, -11710.0, -138.0, 0.0, 4, 90, 2, 'CrystalHall', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Trainer_TradersTier', 'trainer', 1, -3860.0, -11695.0, -137.0, 0.0, 5, 80, 4, 'TradersTier', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Vendor_Vault', 'vendor', 1, -3905.0, -11870.0, -106.0, 0.0, 4, 80, 8, 'Vault', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Plaza_Naaru', 'plaza', 1, -4200.0, -11580.0, -130.0, 0.0, 5, 80, 7, 'SeatOfNaaru', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Trainer_Hall', 'trainer', 1, -4215.0, -11740.0, -139.0, 0.0, 5, 80, 4, 'Trainers', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Plaza_Mystics', 'plaza', 1, -3840.0, -11590.0, -137.0, 0.0, 5, 80, 7, 'HallOfMystics', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Vendor_CrystalHall', 'vendor', 1, -4090.0, -11620.0, -137.0, 0.0, 4, 80, 8, 'CrystalHall', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Plaza_SeatApproach', 'plaza', 1, -3950.0, -11570.0, -137.0, 0.0, 5, 80, 7, 'SeatApproach', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Trainer_Vault', 'trainer', 1, -4100.0, -11830.0, -106.0, 0.0, 5, 70, 4, 'Vault', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Plaza_TradersTier', 'plaza', 1, -3875.0, -11785.0, -138.0, 0.0, 5, 80, 7, 'TradersTier', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Vendor_SeatMarket', 'vendor', 1, -4015.0, -11505.0, -137.0, 0.0, 4, 80, 8, 'SeatMarket', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'EX_Bank_CrystalHall', 'bank', 1, -3985.0, -11730.0, -138.0, 0.0, 5, 110, 11, 'CrystalHall', 0 FROM `city_life_hub` h WHERE h.name='Exodar';

-- Thunder Bluff POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Inn_ElderRise', 'inn', 1, -1196.0, 28.0, 177.0, 0.0, 4, 100, 0, 'ElderRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Tavern_ElderRise', 'tavern', 1, -1210.0, 50.0, 177.0, 0.0, 4, 100, 3, 'ElderRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Plaza_Pools', 'plaza', 1, -1250.0, 80.0, 177.0, 0.0, 5, 100, 7, 'Pools', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Auction_LowerRise', 'auction', 1, -1265.0, 125.0, 132.0, 0.0, 5, 100, 1, 'LowerRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Mailbox_LowerRise', 'mailbox', 1, -1278.0, 118.0, 132.0, 0.0, 4, 90, 2, 'LowerRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_HunterRise', 'trainer', 1, -1050.0, 250.0, 135.0, 0.0, 5, 80, 4, 'HunterRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Vendor_HunterRise', 'vendor', 1, -1010.0, 210.0, 134.0, 0.0, 4, 80, 8, 'HunterRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Plaza_SpiritRise', 'plaza', 1, -1390.0, -80.0, 158.0, 0.0, 5, 80, 7, 'SpiritRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_SpiritRise', 'trainer', 1, -1425.0, -55.0, 158.0, 0.0, 5, 80, 4, 'SpiritRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Plaza_Bridge', 'plaza', 1, -1220.0, -160.0, 132.0, 0.0, 5, 70, 7, 'Bridge', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Vendor_CentralRise', 'vendor', 1, -1180.0, 140.0, 132.0, 0.0, 4, 80, 8, 'CentralRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Plaza_CentralRise', 'plaza', 1, -1295.0, -10.0, 132.0, 0.0, 5, 90, 7, 'CentralRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_CentralRise', 'trainer', 1, -1110.0, -35.0, 132.0, 0.0, 5, 80, 4, 'CentralRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Plaza_NorthBridge', 'plaza', 1, -1090.0, 95.0, 132.0, 0.0, 5, 70, 7, 'Bridge', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Vendor_Market', 'vendor', 1, -1330.0, 170.0, 132.0, 0.0, 4, 80, 8, 'Market', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Bank_LowerRise', 'bank', 1, -1255.0, 125.0, 132.0, 0.0, 5, 110, 11, 'LowerRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

-- Silvermoon POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Inn_Sunfury', 'inn', 1, 9480.0, -7279.0, 14.0, 0.0, 4, 100, 0, 'Sunfury', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Tavern_Sunfury', 'tavern', 1, 9460.0, -7300.0, 14.0, 0.0, 4, 100, 3, 'Sunfury', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Plaza_Court', 'plaza', 1, 9500.0, -7200.0, 14.0, 0.0, 5, 100, 7, 'Court', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Auction_Bazaar', 'auction', 1, 9680.0, -7500.0, 15.0, 0.0, 5, 100, 1, 'Bazaar', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Mailbox_Bazaar', 'mailbox', 1, 9650.0, -7475.0, 15.0, 0.0, 4, 90, 2, 'Bazaar', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_RoyalExchange', 'trainer', 1, 9605.0, -7380.0, 14.0, 0.0, 5, 80, 4, 'RoyalExchange', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Vendor_Court', 'vendor', 1, 9580.0, -7210.0, 14.0, 0.0, 4, 80, 8, 'Court', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Plaza_MurderRow', 'plaza', 1, 9850.0, -7285.0, 14.0, 0.0, 5, 80, 7, 'MurderRow', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_MurderRow', 'trainer', 1, 9800.0, -7335.0, 14.0, 0.0, 5, 80, 4, 'MurderRow', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Plaza_Elders', 'plaza', 1, 9525.0, -6860.0, 16.0, 0.0, 5, 80, 7, 'WalkOfElders', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Vendor_Gate', 'vendor', 1, 9410.0, -7100.0, 15.0, 0.0, 4, 70, 8, 'ShepherdsGate', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Plaza_Exchange', 'plaza', 1, 9750.0, -7050.0, 15.0, 0.0, 5, 90, 7, 'RoyalExchange', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_Farstriders', 'trainer', 1, 9900.0, -7420.0, 14.0, 0.0, 5, 80, 4, 'FarstridersSquare', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Plaza_Promenade', 'plaza', 1, 9680.0, -7150.0, 14.0, 0.0, 5, 80, 7, 'Promenade', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Vendor_Bazaar', 'vendor', 1, 9550.0, -7440.0, 14.0, 0.0, 4, 80, 8, 'Bazaar', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Bank_Bazaar', 'bank', 1, 9640.0, -7490.0, 15.0, 0.0, 5, 110, 11, 'Bazaar', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

-- Shattrath POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Inn_Aldor', 'inn', 1, -1838.0, 5301.0, -12.0, 0.0, 4, 100, 0, 'Aldor', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Tavern_Scryer', 'tavern', 1, -1900.0, 5450.0, -12.0, 0.0, 4, 100, 3, 'Scryer', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_Terrace', 'plaza', 1, -1850.0, 5400.0, -12.0, 0.0, 5, 100, 7, 'Terrace', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Mailbox_Terrace', 'mailbox', 1, -1865.0, 5410.0, -12.0, 0.0, 4, 90, 2, 'Terrace', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Vendor_LowerCity', 'vendor', 1, -1710.0, 5490.0, -12.0, 0.0, 4, 90, 8, 'LowerCity', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_LowerCity', 'plaza', 1, -1600.0, 5260.0, -39.0, 0.0, 5, 80, 7, 'LowerCity', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Trainer_ScryerRise', 'trainer', 1, -2000.0, 5350.0, -8.0, 0.0, 5, 80, 4, 'ScryerRise', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_ScryerRise', 'plaza', 1, -2100.0, 5295.0, -8.0, 0.0, 5, 80, 7, 'ScryerRise', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Trainer_AldorRise', 'trainer', 1, -1745.0, 5760.0, 130.0, 0.0, 5, 80, 4, 'AldorRise', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_AldorRise', 'plaza', 1, -1800.0, 5830.0, 130.0, 0.0, 5, 80, 7, 'AldorRise', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Vendor_Terrace', 'vendor', 1, -1885.0, 5200.0, -12.0, 0.0, 4, 80, 8, 'Terrace', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_Bridge', 'plaza', 1, -1980.0, 5560.0, -12.0, 0.0, 5, 70, 7, 'Bridge', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_LowerMarket', 'plaza', 1, -1685.0, 5350.0, -12.0, 0.0, 5, 80, 7, 'LowerCityMarket', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Vendor_TerraceMarket', 'vendor', 1, -1815.0, 5515.0, -12.0, 0.0, 4, 80, 8, 'TerraceMarket', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Plaza_Central', 'plaza', 1, -1915.0, 5310.0, -12.0, 0.0, 5, 90, 7, 'CentralTerrace', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Bank_Terrace', 'bank', 1, -1855.0, 5425.0, -12.0, 0.0, 5, 100, 11, 'Terrace', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

-- Dalaran POIs
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Inn_Sewers', 'inn', 1, 5807.0, 588.0, 661.0, 0.0, 4, 100, 0, 'Sewers', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Tavern_PurpleParlor', 'tavern', 1, 5850.0, 650.0, 647.0, 0.0, 4, 100, 3, 'PurpleParlor', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_Krasus', 'plaza', 1, 5750.0, 700.0, 655.0, 0.0, 5, 100, 7, 'Krasus', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Auction_MagusCommerce', 'auction', 1, 5890.0, 620.0, 650.0, 0.0, 5, 100, 1, 'MagusCommerce', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Mailbox_MagusCommerce', 'mailbox', 1, 5880.0, 642.0, 650.0, 0.0, 4, 90, 2, 'MagusCommerce', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Vendor_Runeweaver', 'vendor', 1, 5820.0, 480.0, 658.0, 0.0, 4, 80, 8, 'Runeweaver', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Trainer_Krasus', 'trainer', 1, 5775.0, 620.0, 650.0, 0.0, 5, 80, 4, 'Krasus', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_AllianceEnclave', 'plaza', 1, 5710.0, 520.0, 652.0, 0.0, 5, 80, 7, 'AllianceQuarter', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_Sunreavers', 'plaza', 1, 5905.0, 470.0, 652.0, 0.0, 5, 80, 7, 'HordeQuarter', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Trainer_Citadel', 'trainer', 1, 5810.0, 760.0, 640.0, 0.0, 5, 80, 4, 'VioletCitadel', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Vendor_Citadel', 'vendor', 1, 5730.0, 770.0, 640.0, 0.0, 4, 80, 8, 'VioletCitadel', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_Eventide', 'plaza', 1, 5680.0, 650.0, 647.0, 0.0, 5, 80, 7, 'Eventide', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_Landing', 'plaza', 1, 5780.0, 830.0, 680.0, 0.0, 5, 70, 7, 'Landing', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Vendor_Market', 'vendor', 1, 5855.0, 735.0, 640.0, 0.0, 4, 80, 8, 'Market', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Plaza_CentralLoop', 'plaza', 1, 5810.0, 690.0, 647.0, 0.0, 5, 90, 7, 'CentralLoop', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Bank_MagusCommerce', 'bank', 1, 5885.0, 608.0, 650.0, 0.0, 5, 110, 11, 'MagusCommerce', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

-- Class trainer districts and docks across the capitals
-- Stormwind
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Trainer_DwarvenDistrict', 'trainer', 1, -8425.0, 610.0, 95.0, 0.0, 5, 80, 4, 'DwarvenDistrict', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Trainer_Cathedral', 'trainer', 1, -8575.0, 880.0, 106.0, 0.0, 5, 80, 4, 'CathedralSquare', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SW_Dock_Harbor', 'dock', 1, -8642.0, 1336.0, 5.3, 3.14, 4, 80, 5, 'Harbor', 0 FROM `city_life_hub` h WHERE h.name='Stormwind';

-- Orgrimmar
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Trainer_Strength', 'trainer', 1, 1720.0, -4310.0, 33.0, 0.0, 5, 100, 4, 'ValleyOfStrength', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Trainer_Spirits', 'trainer', 1, 1450.0, -4185.0, 44.0, 0.0, 5, 80, 4, 'ValleyOfSpirits', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Trainer_Wisdom', 'trainer', 1, 1938.0, -4675.0, 32.0, 0.0, 5, 80, 4, 'ValleyOfWisdom', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Trainer_Shadow', 'trainer', 1, 1835.0, -4370.0, -16.0, 0.0, 5, 80, 4, 'CleftOfShadow', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'ORG_Dock_Spirits', 'dock', 1, 1960.0, -4180.0, 25.0, 0.0, 4, 80, 5, 'ValleyOfSpirits', 0 FROM `city_life_hub` h WHERE h.name='Orgrimmar';

-- Darnassus
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_Warriors', 'trainer', 1, 10040.0, 2530.0, 1316.0, 0.0, 5, 80, 4, 'WarriorTerrace', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_Temple', 'trainer', 1, 9705.0, 2510.0, 1335.0, 0.0, 5, 80, 4, 'TempleMoon', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Trainer_Enclave', 'trainer', 1, 10080.0, 2205.0, 1328.0, 0.0, 5, 80, 4, 'CenarionEnclave', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DAR_Dock_Ruttheran', 'dock', 1, 9870.0, 2580.0, 1316.0, 0.0, 4, 80, 5, 'RuttheranVillage', 0 FROM `city_life_hub` h WHERE h.name='Darnassus';

-- Thunder Bluff
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_Hunter', 'trainer', 1, -1050.0, 250.0, 135.0, 0.0, 5, 80, 4, 'HunterRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_Spirit', 'trainer', 1, -1425.0, -55.0, 158.0, 0.0, 5, 80, 4, 'SpiritRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Trainer_Central', 'trainer', 1, -1110.0, -35.0, 132.0, 0.0, 5, 80, 4, 'CentralMesa', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'TB_Dock_Lower', 'dock', 1, -1300.0, -20.0, 125.0, 0.0, 4, 80, 5, 'LowerRise', 0 FROM `city_life_hub` h WHERE h.name='ThunderBluff';

-- Silvermoon
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_Royal', 'trainer', 1, 9605.0, -7380.0, 14.0, 0.0, 5, 80, 4, 'RoyalExchange', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_Murder', 'trainer', 1, 9800.0, -7335.0, 14.0, 0.0, 5, 80, 4, 'MurderRow', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SMC_Trainer_Farstriders', 'trainer', 1, 9900.0, -7420.0, 14.0, 0.0, 5, 80, 4, 'FarstridersSquare', 0 FROM `city_life_hub` h WHERE h.name='Silvermoon';

-- Shattrath
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Trainer_Scryer', 'trainer', 1, -2000.0, 5350.0, -8.0, 0.0, 5, 80, 4, 'ScryersTier', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'SHAT_Trainer_Aldor', 'trainer', 1, -1745.0, 5760.0, 130.0, 0.0, 5, 80, 4, 'AldorRise', 0 FROM `city_life_hub` h WHERE h.name='Shattrath';

-- Dalaran
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Trainer_Krasus', 'trainer', 1, 5775.0, 620.0, 650.0, 0.0, 5, 80, 4, 'KrasusLanding', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'DALA_Trainer_Violet', 'trainer', 1, 5810.0, 760.0, 640.0, 0.0, 5, 80, 4, 'VioletHold', 0 FROM `city_life_hub` h WHERE h.name='Dalaran';
