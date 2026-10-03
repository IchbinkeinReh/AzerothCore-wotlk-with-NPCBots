-- NPCBots chatter texts 71201-71300, see src/server/game/AI/NpcBots/bottext.h
-- each ID holds up to 8 random variants (text0_0 - text7_0)
SET @CHATTER2_TEXTS_START = 71201;
SET @CHATTER2_TEXTS_END   = 71300;

DELETE FROM `npc_text` WHERE `ID` BETWEEN @CHATTER2_TEXTS_START AND @CHATTER2_TEXTS_END;
INSERT INTO `npc_text` (`ID`,`text0_0`,`text1_0`,`text2_0`,`text3_0`,`text4_0`,`text5_0`,`text6_0`,`text7_0`,`VerifiedBuild`) VALUES
(@CHATTER2_TEXTS_START+0,'Nice %item, %target! Where did you get that?','Is that %item? Looks good on you, %target.','I would not mind having %item myself.',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+1,'Is that %item? Impressive!','By the gods, %target, %item! You must have fought hard for that.','Everyone, look! %target has %item!',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+2,'By the Light... %item! I never thought I would see one with my own eyes.','Is that... %item? Legends walk among us!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+3,'What a beautiful %item, %target!','Where did you find %item? I want one too!','Now that is a mount! %item, right?',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+4,'Congratulations on "%achievement", %target!','I heard you earned "%achievement". Well done!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+5,'Isn\'t my %pet adorable?','My %pet follows me everywhere.','Careful, my %pet bites. Just kidding.',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+6,'Have you seen my %mount? Rare as they come.','Took me ages to get this %mount.',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+7,'They say %enemy has been seen near %place.','A friend told me %enemy is prowling around %place.','Watch out near %place, %enemy is about.',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+8,'A race to %place! Gather here, we start in 30 seconds. No mounts!','Who is the fastest? Race to %place, start here in 30 seconds, on foot!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+9,'Ready... set... GO!','Three, two, one... RUN!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+10,'%winner wins the race to %place!','And the winner of the race to %place is %winner!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+11,'Well, that did not work out. Next time!','Nobody? Pity.',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+12,'Drinking contest! Who can hold their ale? /cheer to join, we start in 30 seconds!',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+13,'Another round!','Bottoms up!','Keep them coming!',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+14,'*hic* I am... done.','No more... the room is spinning.','I need... to sit down.',NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+15,'%winner drank everyone under the table!','The last one standing: %winner!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+16,'Arm wrestling! Who dares to challenge me? /cheer to join, we start in 30 seconds!',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+17,'%winner wins the arm wrestling!','What strength! %winner wins!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+18,'Here is your prize: %price. Well done!',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+19,'No mounts in this race, %target! You are out.',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+20,'You cannot take another drop and drop out of the contest.',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+21,'%cult is losing ground. %villain %title will have to show up soon!','Another blow against %cult! %villain cannot hide forever.',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+22,'%villain %title has been sighted at %place! Everyone, rally!','%villain %title shows itself at %place! To arms!',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+23,'%villain %title has fallen! Glory to the heroes!',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+24,'%villain %title escaped... for now.','Curses! %villain got away again.',NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+25,'%player defeated %subject at %place!',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+26,'%player won %subject at %place.',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+27,'a race',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+28,'a drinking contest',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+29,'an arm wrestling match',NULL,NULL,NULL,NULL,NULL,NULL,NULL,'-1'),
(@CHATTER2_TEXTS_START+30,'Hnngh...!','Not... yet...!','You are stronger than you look!',NULL,NULL,NULL,NULL,NULL,'-1');
