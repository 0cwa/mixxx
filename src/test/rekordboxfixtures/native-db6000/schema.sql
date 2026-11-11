-- Prepared by ChatGPT for human review.
-- Synthetic parser fixture. No rekordbox application or audio export is claimed.
CREATE TABLE djmdProperty (DBID TEXT, DBVersion TEXT, DeviceID TEXT);
INSERT INTO djmdProperty VALUES ('fixture-native-dbid', '6000', 'fixture-device');
CREATE TABLE djmdArtist (ID TEXT, Name TEXT, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdAlbum (ID TEXT, Name TEXT, AlbumArtistID TEXT, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdGenre (ID TEXT, Name TEXT, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdKey (ID TEXT, ScaleName TEXT, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdLabel (ID TEXT, Name TEXT, rb_local_deleted INTEGER DEFAULT 0);
-- codespell:ignore-next-line commnt
CREATE TABLE djmdColor (ID TEXT, ColorCode TEXT, Commnt TEXT, rb_local_deleted INTEGER DEFAULT 0);
INSERT INTO djmdArtist VALUES ('artist', 'Synthetic Artist', 0), ('empty-artist', '', 0);
INSERT INTO djmdAlbum VALUES ('album', 'Synthetic Album', 'artist', 0);
INSERT INTO djmdGenre VALUES ('genre', 'Synthetic Genre', 0);
INSERT INTO djmdKey VALUES ('key', 'F# minor', 0);
INSERT INTO djmdLabel VALUES ('label', 'Synthetic Label', 0);
INSERT INTO djmdColor VALUES ('5', NULL, 'Green', 0);
CREATE TABLE djmdContent (
    ID TEXT, FolderPath TEXT, Title TEXT, ArtistID TEXT, AlbumID TEXT,
    GenreID TEXT, KeyID TEXT, LabelID TEXT, RemixerID TEXT, ComposerID TEXT,
    BPM INTEGER, Length INTEGER, TrackNo INTEGER, DiscNo INTEGER,
    BitRate INTEGER, SampleRate INTEGER, Rating INTEGER, ReleaseYear INTEGER,
    -- codespell:ignore-next-line commnt
    DJPlayCount TEXT, Commnt TEXT, StockDate TEXT, updated_at TEXT,
    AnalysisDataPath TEXT, ImagePath TEXT, FileType INTEGER, ServiceID INTEGER,
    ColorID TEXT, CueUpdated TEXT, AnalysisUpdated TEXT, rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdContent VALUES (
    '7', 'música/日本語.wav', 'Ångström 日本語', 'artist', 'album',
    'genre', 'key', 'label', 'artist', 'artist', 12850, 10, 2, 1,
    1536, 48000, 5, 2026, '12', '', '2026-11-11', '2026-11-11 00:00:00',
    '/PIONEER/USBANLZ/test/ANLZ0000.DAT', 'artwork/cover.png', 11, 0,
    '5', '3', '1', 0
);
INSERT INTO djmdContent (ID, FolderPath, Title, ArtistID, FileType, ServiceID)
VALUES ('4294967296', 'missing.wav', NULL, 'empty-artist', 11, 0);
INSERT INTO djmdContent (ID, FolderPath, Title, FileType, ServiceID)
VALUES ('stream', 'service://track/123', 'Streaming Track', 1, 3);
INSERT INTO djmdContent (ID, FolderPath, Title, FileType, ServiceID)
VALUES ('empty', '', '', 0, 0);
INSERT INTO djmdContent (ID, FolderPath, Title, FileType, ServiceID, rb_local_deleted)
VALUES ('deleted', 'removed.wav', 'Removed Track', 11, 0, 1);
CREATE TABLE djmdPlaylist (
    ID TEXT, Seq INTEGER, Name TEXT, Attribute INTEGER, ParentID TEXT,
    SmartList TEXT, rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdPlaylist VALUES
    ('folder', 1, 'Folder 日本語', 1, 'root', NULL, 0),
    ('7', 1, 'Repeated Tracks', 0, 'folder', NULL, 0),
    ('smart', 2, 'Smart Definition', 4, 'root', '<NODE/>', 0),
    ('100000', 3, 'Internal Sentinel', -128, 'root', NULL, 0);
CREATE TABLE djmdSongPlaylist (
    ID TEXT, PlaylistID TEXT, ContentID TEXT, TrackNo INTEGER,
    rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdSongPlaylist VALUES
    ('membership-c', '7', '7', 3, 0),
    ('membership-a', '7', '7', 1, 0),
    ('membership-b', '7', '4294967296', 2, 0);
CREATE TABLE djmdHistory (
    ID TEXT, Seq INTEGER, Name TEXT, Attribute INTEGER, ParentID TEXT,
    rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdHistory VALUES ('7', 1, 'History', 0, 'root', 0);
CREATE TABLE djmdSongHistory (
    ID TEXT, HistoryID TEXT, ContentID TEXT, TrackNo INTEGER,
    rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdSongHistory VALUES ('history-b', '7', '7', 2, 0), ('history-a', '7', '7', 1, 0);
CREATE TABLE djmdCue (
    ID TEXT, ContentID TEXT, InMsec INTEGER, OutMsec INTEGER, Kind INTEGER,
    Color INTEGER, ColorTableIndex INTEGER, ActiveLoop INTEGER, Comment TEXT,
    InFrame INTEGER, OutFrame INTEGER, CueMicrosec INTEGER,
    rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdCue VALUES
    ('memory-a', '7', 1500, -1, 0, -1, 0, 0, 'Memory A', 225, 0, NULL, 0),
    ('memory-b', '7', 1500, -1, 0, -1, 0, 0, NULL, 225, 0, NULL, 0),
    ('hot-loop', '7', 2000, 4000, 1, 0, 0, 1, '', 300, 600, NULL, 0),
    ('hot-point', '7', 2501, -1, 2, 7, 1, NULL, 'Hot B', 375, 0, NULL, 0),
    ('memory-loop', '7', 5000, 6000, 0, 7, 1, 0, 'Memory Loop', 750, 900, NULL, 0);
CREATE TABLE contentCue (
    ID TEXT, ContentID TEXT, Cues TEXT, rb_local_deleted INTEGER DEFAULT 0
);
CREATE TABLE djmdActiveCensor (
    ID TEXT, ContentID TEXT, InMsec INTEGER, OutMsec INTEGER,
    ParameterList TEXT, rb_local_deleted INTEGER DEFAULT 0
);
INSERT INTO djmdActiveCensor VALUES ('censor', '7', 8000, 9000, 'opaque-effect-parameters', 0);
-- End of fixture prepared by ChatGPT for human review and submission.
