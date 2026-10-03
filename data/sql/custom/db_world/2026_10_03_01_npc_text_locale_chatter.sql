-- NPCBots chatter texts 71201-71300, deDE, see src/server/game/AI/NpcBots/bottext.h
-- slot N here translates slot N of the same npc_text ID
SET @CHATTER2_TEXTS_START = 71201;
SET @CHATTER2_TEXTS_END   = 71300;

DELETE FROM `npc_text_locale` WHERE `Locale`='deDE' AND `ID` BETWEEN @CHATTER2_TEXTS_START AND @CHATTER2_TEXTS_END;
INSERT INTO `npc_text_locale` (`ID`,`Locale`,`Text0_0`,`Text1_0`,`Text2_0`,`Text3_0`,`Text4_0`,`Text5_0`,`Text6_0`,`Text7_0`) VALUES
(@CHATTER2_TEXTS_START+0,'deDE','Schönes %item, %target! Wo hast du das her?','Ist das %item? Steht dir gut, %target.','Gegen %item hätte ich auch nichts einzuwenden.',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+1,'deDE','Ist das etwa %item? Beeindruckend!','Bei den Göttern, %target, %item! Dafür musst du hart gekämpft haben.','Seht mal alle! %target hat %item!',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+2,'deDE','Beim Licht... %item! Ich hätte nie gedacht, so etwas mit eigenen Augen zu sehen.','Ist das... %item? Legenden wandeln unter uns!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+3,'deDE','Was für ein prächtiges %item, %target!','Wo hast du %item her? Ich will auch so eins!','Das nenne ich ein Reittier! %item, oder?',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+4,'deDE','Glückwunsch zu "%achievement", %target!','Ich habe gehört, du hast "%achievement" geschafft. Gut gemacht!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+5,'deDE','Ist mein %pet nicht entzückend?','Mein %pet folgt mir überallhin.','Vorsicht, mein %pet beißt. Kleiner Scherz.',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+6,'deDE','Habt ihr mein %mount gesehen? Gibt es nicht oft.','Ich habe ewig gebraucht, um dieses %mount zu bekommen.',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+7,'deDE','Man sagt, %enemy wurde bei %place gesehen.','Ein Freund erzählte mir, dass %enemy bei %place herumstreift.','Passt bei %place auf, %enemy treibt sich dort herum.',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+8,'deDE','Ein Wettrennen nach %place! Sammelt euch hier, Start in 30 Sekunden. Keine Reittiere!','Wer ist am schnellsten? Rennen nach %place, Start hier in 30 Sekunden, zu Fuß!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+9,'deDE','Auf die Plätze... fertig... LOS!','Drei, zwei, eins... LAUFT!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+10,'deDE','%winner gewinnt das Rennen nach %place!','Und der Sieger des Rennens nach %place ist %winner!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+11,'deDE','Na, das hat wohl nicht geklappt. Nächstes Mal!','Niemand? Schade.',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+12,'deDE','Trinkwettbewerb! Wer verträgt am meisten? /jubeln zum Mitmachen, wir starten in 30 Sekunden!',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+13,'deDE','Noch eine Runde!','Ex und hopp!','Weiter, weiter!',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+14,'deDE','*hicks* Ich bin... fertig.','Nichts mehr... der Raum dreht sich.','Ich muss mich... hinsetzen.',NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+15,'deDE','%winner hat alle unter den Tisch getrunken!','Als Letzter noch auf den Beinen: %winner!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+16,'deDE','Armdrücken! Wer wagt es, mich herauszufordern? /jubeln zum Mitmachen, wir starten in 30 Sekunden!',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+17,'deDE','%winner gewinnt das Armdrücken!','Was für eine Kraft! %winner gewinnt!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+18,'deDE','Hier ist dein Preis: %price. Gut gemacht!',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+19,'deDE','Keine Reittiere bei diesem Rennen, %target! Du bist raus.',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+20,'deDE','Du verträgst keinen Tropfen mehr und scheidest aus dem Wettbewerb aus.',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+21,'deDE','%cult verliert an Boden. %villain %title wird sich bald zeigen müssen!','Ein weiterer Schlag gegen %cult! %villain kann sich nicht ewig verstecken.',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+22,'deDE','%villain %title wurde bei %place gesichtet! Alle sammeln!','%villain %title zeigt sich bei %place! Zu den Waffen!',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+23,'deDE','%villain %title ist gefallen! Ruhm den Helden!',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+24,'deDE','%villain %title ist entkommen... vorerst.','Verflucht! %villain ist wieder entwischt.',NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+25,'deDE','%player hat %subject bei %place besiegt!',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+26,'deDE','%player hat %subject bei %place gewonnen.',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+27,'deDE','ein Wettrennen',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+28,'deDE','einen Trinkwettbewerb',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+29,'deDE','ein Armdrücken',NULL,NULL,NULL,NULL,NULL,NULL,NULL),
(@CHATTER2_TEXTS_START+30,'deDE','Hnngh...!','Noch... nicht...!','Du bist stärker, als du aussiehst!',NULL,NULL,NULL,NULL,NULL);
