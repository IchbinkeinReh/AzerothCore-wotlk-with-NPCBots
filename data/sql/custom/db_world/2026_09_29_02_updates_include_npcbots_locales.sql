-- Apply the NPCBots german texts (data/sql/Bots/locales/deDE) with the database updater,
-- takes effect on the next updater run
DELETE FROM `updates_include` WHERE `path`='$/data/sql/Bots/locales/deDE';
INSERT INTO `updates_include` (`path`,`state`) VALUES
('$/data/sql/Bots/locales/deDE','CUSTOM');
