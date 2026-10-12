-- mod-playerbots-city-life
-- Idempotent Bootstrap, Schema Repair & POI Normalization Migration
-- Targets AzerothCore WORLD Database (MySQL 8.0 / 8.4 compatible)

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

-- 3. Update authoritative zone_ids for core hubs
UPDATE `city_life_hub` SET `zone_id` = 1519 WHERE `name` = 'Stormwind';
UPDATE `city_life_hub` SET `zone_id` = 1637 WHERE `name` = 'Orgrimmar';
UPDATE `city_life_hub` SET `zone_id` = 1537 WHERE `name` = 'Ironforge';
UPDATE `city_life_hub` SET `zone_id` = 1497 WHERE `name` = 'Undercity';
UPDATE `city_life_hub` SET `zone_id` = 1657 WHERE `name` = 'Darnassus';
UPDATE `city_life_hub` SET `zone_id` = 1638 WHERE `name` = 'ThunderBluff';
UPDATE `city_life_hub` SET `zone_id` = 3557 WHERE `name` = 'Exodar';
UPDATE `city_life_hub` SET `zone_id` = 3487 WHERE `name` = 'Silvermoon';
UPDATE `city_life_hub` SET `zone_id` = 3703 WHERE `name` = 'Shattrath';
UPDATE `city_life_hub` SET `zone_id` = 4395 WHERE `name` = 'Dalaran';
UPDATE `city_life_hub` SET `zone_id` = 4197 WHERE `name` IN ('WintergraspAlliance', 'WintergraspHorde');
UPDATE `city_life_hub` SET `zone_id` = 440 WHERE `name` = 'Gadgetzan';
UPDATE `city_life_hub` SET `zone_id` = 12 WHERE `name` = 'Goldshire';
UPDATE `city_life_hub` SET `zone_id` = 3483 WHERE `name` IN ('HonorHold', 'Thrallmar');

-- 4. Upsert duel hubs
INSERT IGNORE INTO `city_life_hub`
(`name`, `enabled`, `team`, `min_level`, `max_level`, `map_id`, `zone_id`, `default_population`, `priority`)
VALUES
('StormwindGate', 1, 0, 80, 80, 0, 12, 22, 215),
('OrgrimmarGate', 1, 1, 80, 80, 1, 14, 22, 215),
('GoldshireDuel', 1, 0, 80, 80, 0, 12, 4, 140);

-- 5. Normalize category synonyms and POI types (preserving customized coordinates)
UPDATE `city_life_spot` SET `category` = 'mailbox', `poi_type` = 2 WHERE `category` = 'mail';
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

-- 6. Upsert Goldshire duel spots without touching user spots
INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GSDuel_Ring_1', 'duel', 1, -9462.85, 75.32, 56.45, 1.25, 6, 100, 9, 'GoldshireDuelRing', 0
FROM `city_life_hub` h WHERE h.name='GoldshireDuel';

INSERT IGNORE INTO `city_life_spot` (`hub_id`, `name`, `category`, `enabled`, `x`, `y`, `z`, `o`, `radius`, `weight`, `poi_type`, `district`, `partner_spot_id`)
SELECT h.id, 'GSDuel_Ring_2', 'duel', 1, -9450.40, 85.10, 56.55, 3.80, 6, 100, 9, 'GoldshireDuelRing', 0
FROM `city_life_hub` h WHERE h.name='GoldshireDuel';
