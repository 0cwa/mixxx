-- Prepared by ChatGPT for human review.
-- Synthetic schema-profile fixture; no vendor export or audio is included.
CREATE TABLE content(content_id INTEGER PRIMARY KEY, title TEXT, titleForSearch TEXT,
    subtitle TEXT, bpmx100 INTEGER, length INTEGER, trackNo INTEGER, discNo INTEGER,
    artist_id_artist INTEGER, artist_id_remixer INTEGER, artist_id_originalArtist INTEGER,
    artist_id_composer INTEGER, artist_id_lyricist INTEGER, album_id INTEGER,
    genre_id INTEGER, label_id INTEGER, key_id INTEGER, color_id INTEGER, image_id INTEGER,
    djComment TEXT, rating INTEGER, releaseYear INTEGER, releaseDate TEXT,
    dateCreated TEXT, dateAdded TEXT, path TEXT, fileName TEXT, fileSize INTEGER,
    fileType INTEGER, bitrate INTEGER, bitDepth INTEGER, samplingRate INTEGER, isrc TEXT,
    djPlayCount INTEGER, isHotCueAutoLoadOn INTEGER, isKuvoDeliverStatusOn INTEGER,
    kuvoDeliveryComment TEXT, masterDbId INTEGER, masterContentId INTEGER,
    analysisDataFilePath TEXT, analysedBits INTEGER, contentLink INTEGER, hasModified INTEGER,
    cueUpdateCount INTEGER, analysisDataUpdateCount INTEGER, informationUpdateCount INTEGER);
CREATE TABLE artist(artist_id INTEGER PRIMARY KEY, name TEXT, nameForSearch TEXT);
CREATE TABLE album(album_id INTEGER PRIMARY KEY, name TEXT, artist_id INTEGER,
    image_id INTEGER, isComplation INTEGER, nameForSearch TEXT);
CREATE TABLE genre(genre_id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE label(label_id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE "key"(key_id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE color(color_id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE image(image_id INTEGER PRIMARY KEY, path TEXT);
CREATE TABLE playlist(playlist_id INTEGER PRIMARY KEY, sequenceNo INTEGER, name TEXT,
    image_id INTEGER, attribute INTEGER, playlist_id_parent INTEGER);
CREATE TABLE playlist_content(playlist_id INTEGER, content_id INTEGER, sequenceNo INTEGER);
CREATE INDEX index_playlist_content_playlist_id ON playlist_content(playlist_id);
CREATE TABLE property(deviceName TEXT, dbVersion TEXT, numberOfContents INTEGER,
    createdDate TEXT, backGroundColorType INTEGER, myTagMasterDBID INTEGER);
CREATE TABLE cue(cue_id INTEGER PRIMARY KEY, content_id INTEGER, kind INTEGER,
    colorTableIndex INTEGER, cueComment TEXT, isActiveLoop INTEGER,
    beatLoopNumerator INTEGER, beatLoopDenominator INTEGER, inUsec INTEGER,
    outUsec INTEGER, in150FramePerSec INTEGER, out150FramePerSec INTEGER);
CREATE TABLE history(history_id INTEGER PRIMARY KEY, sequenceNo INTEGER, name TEXT,
    attribute INTEGER, history_id_parent INTEGER);
CREATE TABLE history_content(history_id INTEGER, content_id INTEGER, sequenceNo INTEGER);
CREATE TABLE hotCueBankList(hotCueBankList_id INTEGER PRIMARY KEY, sequenceNo INTEGER,
    name TEXT, image_id INTEGER, attribute INTEGER, hotCueBankList_id_parent INTEGER);
CREATE TABLE hotCueBankList_cue(hotCueBankList_id INTEGER, cue_id INTEGER, sequenceNo INTEGER);
CREATE INDEX index_hotCueBankList_cue_hotCueBankList_id ON hotCueBankList_cue(hotCueBankList_id);
CREATE TABLE myTag(myTag_id INTEGER PRIMARY KEY, sequenceNo INTEGER, name TEXT,
    attribute INTEGER, myTag_id_parent INTEGER);
CREATE TABLE myTag_content(myTag_id INTEGER, content_id INTEGER);
CREATE INDEX index_myTag_content_myTag_id ON myTag_content(myTag_id);
CREATE INDEX index_myTag_content_content_id ON myTag_content(content_id);
CREATE TABLE menuItem(menuItem_id INTEGER PRIMARY KEY, kind INTEGER, name TEXT);
CREATE TABLE category(category_id INTEGER PRIMARY KEY, menuItem_id INTEGER, sequenceNo INTEGER, isVisible INTEGER);
CREATE TABLE sort(sort_id INTEGER PRIMARY KEY, menuItem_id INTEGER, sequenceNo INTEGER,
    isVisible INTEGER, isSelectedAsSubColumn INTEGER);
CREATE TABLE recommendedLike(content_id_1 INTEGER, content_id_2 INTEGER, rating INTEGER, createdDate INTEGER);

INSERT INTO property VALUES('Synthetic device', '1000', 2, '2026-11-11', 0, 123);
INSERT INTO artist VALUES(1, 'Artist Æ', NULL), (2, '', NULL);
INSERT INTO album VALUES(1, 'Night', 2, NULL, 0, NULL);
INSERT INTO genre VALUES(1, 'House');
INSERT INTO label VALUES(1, '');
INSERT INTO "key" VALUES(1, '8A');
INSERT INTO color VALUES(1, 'Pink');
INSERT INTO image VALUES(1, '/PIONEER/Artwork/cover.jpg');
INSERT INTO content(content_id, title, bpmx100, length, trackNo, discNo, artist_id_artist,
    artist_id_composer, artist_id_remixer, album_id, genre_id, label_id, key_id, color_id,
    image_id, djComment, rating, releaseYear, dateAdded, path, bitrate, samplingRate,
    djPlayCount, masterDbId, masterContentId, analysisDataFilePath)
VALUES(7, 'Höfn / 東京', 12850, 161, 4, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, NULL, 5,
    2024, '2026-11-11', '/Contents/Höfn.wav', 1411, 44100, 8, 99, 700,
    '/PIONEER/USBANLZ/ABC/ANLZ0000.DAT');
INSERT INTO content(content_id, title, bpmx100, length, artist_id_artist, djComment, rating, path)
VALUES(42, '', NULL, 0, 2, '', 0, '/Contents/second.wav');
INSERT INTO playlist VALUES(100, 2, 'Folder', NULL, 1, 0),
    (101, 1, 'Repeated', NULL, 0, 100), (102, 1, 'Empty', NULL, 0, NULL);
INSERT INTO playlist_content VALUES(101, 7, 1), (101, 42, 2), (101, 7, 3);
INSERT INTO cue VALUES(9, 7, 1, 0, 'Opaque cue', 0, NULL, NULL, 250000, -1, 37, -1);
-- End of synthetic fixture prepared by ChatGPT for human review and submission.
