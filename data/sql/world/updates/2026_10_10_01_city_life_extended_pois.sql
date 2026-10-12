-- mod-playerbots-city-life
-- Extended POIs and Schema Migration (Ported from mod-city-bots)
-- Idempotent script for AzerothCore WORLD database.

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

-- 1. Extend city_life_spot with POI classification, district, and scene pairing (MySQL compatible)
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

CALL AddColumnUnlessExists_CityLife('city_life_spot', 'poi_type', 'TINYINT UNSIGNED NOT NULL DEFAULT 7 COMMENT \'0=Inn,1=AH,2=Mailbox,3=Tavern,4=Trainer,5=Dock,6=WallPatrol,7=Plaza,8=Vendor,9=DuelRing,10=InnBed,11=Bank\' AFTER `category`');
CALL AddColumnUnlessExists_CityLife('city_life_spot', 'district', 'VARCHAR(32) NOT NULL DEFAULT \'\' AFTER `poi_type`');
CALL AddColumnUnlessExists_CityLife('city_life_spot', 'partner_spot_id', 'INT UNSIGNED NOT NULL DEFAULT 0 AFTER `district`');

DROP PROCEDURE IF EXISTS `AddColumnUnlessExists_CityLife`;

-- 2. Ensure duel sub-hubs exist in city_life_hub
INSERT IGNORE INTO `city_life_hub`
(`name`, `enabled`, `team`, `min_level`, `max_level`, `map_id`, `default_population`, `priority`)
VALUES
('StormwindGate', 1, 0, 80, 80, 0, 22, 215),
('OrgrimmarGate', 1, 1, 80, 80, 1, 22, 215),
('GoldshireDuel', 1, 0, 80, 80, 0, 4, 140);

-- 3. Insert curated POIs across all capitals and duel hubs

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
SELECT h.id, 'GS_Dance_FloorUp', 'inn', 1, -9472.58, 37.34, 64.69, 4.65, 3, 100, 10, 'LionsPrideUp', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GS_CrystalLake_Fish', 'dock', 1, -9432.98, 147.82, 56.40, 1.77, 4, 100, 5, 'CrystalLake', 0 FROM `city_life_hub` h WHERE h.name='Goldshire';

-- Duel Ring POIs
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
