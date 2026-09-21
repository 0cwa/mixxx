#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace mixxx::rekordbox::test {

// This is a deterministic, synthetic subset of the checked-in
// rekordbox_pdb.ksy contract. It is not an authentic Rekordbox export and
// does not claim full-device compatibility. The layout follows the generated
// rekordbox_pdb.cpp reader and the schema-derived B38 fixture recipe.

constexpr qsizetype kPdbPageSize = 0x200;
constexpr qsizetype kPdbPageCount = 3;
constexpr qsizetype kPdbFileSize = kPdbPageSize * kPdbPageCount;

inline void setByte(QByteArray* bytes, qsizetype offset, quint8 value) {
    Q_ASSERT(bytes != nullptr);
    Q_ASSERT(offset >= 0 && offset < bytes->size());
    (*bytes)[offset] = static_cast<char>(value);
}

inline void setU16Le(QByteArray* bytes, qsizetype offset, quint16 value) {
    setByte(bytes, offset, static_cast<quint8>(value & 0xff));
    setByte(bytes, offset + 1, static_cast<quint8>((value >> 8) & 0xff));
}

inline void setU24Le(QByteArray* bytes, qsizetype offset, quint32 value) {
    setByte(bytes, offset, static_cast<quint8>(value & 0xff));
    setByte(bytes, offset + 1, static_cast<quint8>((value >> 8) & 0xff));
    setByte(bytes, offset + 2, static_cast<quint8>((value >> 16) & 0xff));
}

inline void setU32Le(QByteArray* bytes, qsizetype offset, quint32 value) {
    setByte(bytes, offset, static_cast<quint8>(value & 0xff));
    setByte(bytes, offset + 1, static_cast<quint8>((value >> 8) & 0xff));
    setByte(bytes, offset + 2, static_cast<quint8>((value >> 16) & 0xff));
    setByte(bytes, offset + 3, static_cast<quint8>((value >> 24) & 0xff));
}

inline void setShortAscii(
        QByteArray* bytes, qsizetype offset, const QByteArray& text) {
    // device_sql_short_ascii stores the actual length, incremented, doubled,
    // and incremented again as its length-and-kind marker.
    Q_ASSERT(text.size() <= 126);
    setByte(bytes, offset, static_cast<quint8>(2 * (text.size() + 1) + 1));
    for (qsizetype index = 0; index < text.size(); ++index) {
        setByte(bytes, offset + 1 + index, static_cast<quint8>(text.at(index)));
    }
}

inline void setPageHeader(
        QByteArray* bytes,
        qsizetype pageOffset,
        quint32 pageIndex,
        quint32 pageType,
        quint16 freeSize,
        quint16 usedSize) {
    // page::seq in rekordbox_pdb.ksy, including the 13-bit/11-bit packed
    // num_rows/num_rows_valid field.
    setU32Le(bytes, pageOffset + 0x00, 0);
    setU32Le(bytes, pageOffset + 0x04, pageIndex);
    setU32Le(bytes, pageOffset + 0x08, pageType);
    setU32Le(bytes, pageOffset + 0x0c, kPdbPageCount);
    setU32Le(bytes, pageOffset + 0x10, 0);
    setU32Le(bytes, pageOffset + 0x14, 0);
    setU24Le(bytes, pageOffset + 0x18, 0x2001);
    setByte(bytes, pageOffset + 0x1b, 0);
    setU16Le(bytes, pageOffset + 0x1c, freeSize);
    setU16Le(bytes, pageOffset + 0x1e, usedSize);
    setU16Le(bytes, pageOffset + 0x20, 0);
    setU16Le(bytes, pageOffset + 0x22, 0);
    setU16Le(bytes, pageOffset + 0x24, 0);
    setU16Le(bytes, pageOffset + 0x26, 0);
}

inline void setPageFooter(QByteArray* bytes, qsizetype pageOffset) {
    // row_group(0): 16 u16 row offsets, followed by the u16 presence mask.
    // The generated reader addresses these from pageOffset + 0x200.
    constexpr qsizetype kRowGroupBase = kPdbPageSize;
    for (qsizetype rowIndex = 0; rowIndex < 16; ++rowIndex) {
        setU16Le(bytes,
                pageOffset + kRowGroupBase - 6 - (2 * rowIndex),
                0);
    }
    setU16Le(bytes, pageOffset + kRowGroupBase - 4, 0x0001);
}

inline void setTrackRow(QByteArray* bytes, qsizetype rowOffset) {
    // track_row in rekordbox_pdb.ksy: the fixed row ends at offset 0x88.
    setU16Le(bytes, rowOffset + 0x00, 0x0024);
    setU16Le(bytes, rowOffset + 0x02, 0);
    setU32Le(bytes, rowOffset + 0x04, 0);
    setU32Le(bytes, rowOffset + 0x08, 44100);
    setU32Le(bytes, rowOffset + 0x0c, 0);
    setU32Le(bytes, rowOffset + 0x10, 0);
    setU32Le(bytes, rowOffset + 0x14, 0);
    setU16Le(bytes, rowOffset + 0x18, 0);
    setU16Le(bytes, rowOffset + 0x1a, 0);
    setU32Le(bytes, rowOffset + 0x1c, 0);
    setU32Le(bytes, rowOffset + 0x20, 42); // KEYS row id
    setU32Le(bytes, rowOffset + 0x24, 0);
    setU32Le(bytes, rowOffset + 0x28, 0);
    setU32Le(bytes, rowOffset + 0x2c, 0);
    setU32Le(bytes, rowOffset + 0x30, 128000);
    setU32Le(bytes, rowOffset + 0x34, 1);
    setU32Le(bytes, rowOffset + 0x38, 12800); // 128 BPM * 100
    setU32Le(bytes, rowOffset + 0x3c, 0);
    setU32Le(bytes, rowOffset + 0x40, 0);
    setU32Le(bytes, rowOffset + 0x44, 0);
    setU32Le(bytes, rowOffset + 0x48, 100); // track id
    setU16Le(bytes, rowOffset + 0x4c, 1);
    setU16Le(bytes, rowOffset + 0x4e, 0);
    setU16Le(bytes, rowOffset + 0x50, 2024);
    setU16Le(bytes, rowOffset + 0x52, 16);
    setU16Le(bytes, rowOffset + 0x54, 180);
    setU16Le(bytes, rowOffset + 0x56, 0);
    setByte(bytes, rowOffset + 0x58, 0);
    setByte(bytes, rowOffset + 0x59, 0);
    setU16Le(bytes, rowOffset + 0x5a, 0);
    setU16Le(bytes, rowOffset + 0x5c, 0);

    // ofs_strings[0..20], in the exact order declared by track_row.
    constexpr quint16 kEmpty = 136;
    constexpr quint16 kTitle = 137;
    constexpr quint16 kComment = 139;
    constexpr quint16 kAnalyzePath = 141;
    constexpr quint16 kFilePath = 147;
    constexpr quint16 kStringOffsets[] = {
            kEmpty,       // isrc
            kEmpty,       // texter
            kEmpty,       // unknown_string_2
            kEmpty,       // unknown_string_3
            kEmpty,       // unknown_string_4
            kEmpty,       // message
            kEmpty,       // kuvo_public
            kEmpty,       // autoload_hot_cues
            kEmpty,       // unknown_string_5
            kEmpty,       // unknown_string_6
            kEmpty,       // date_added
            kEmpty,       // release_date
            kEmpty,       // mix_name
            kEmpty,       // unknown_string_7
            kAnalyzePath, // analyze_path
            kEmpty,       // analyze_date
            kComment,     // comment
            kTitle,       // title
            kEmpty,       // unknown_string_8
            kFilePath,    // filename
            kFilePath,    // file_path
    };
    for (qsizetype index = 0;
            index < static_cast<qsizetype>(sizeof(kStringOffsets) / sizeof(quint16));
            ++index) {
        setU16Le(bytes, rowOffset + 0x5e + (2 * index), kStringOffsets[index]);
    }

    // Five short-ASCII device_sql_string records, all within this row.
    setShortAscii(bytes, rowOffset + kEmpty, QByteArray());
    setShortAscii(bytes, rowOffset + kTitle, QByteArrayLiteral("T"));
    setShortAscii(bytes, rowOffset + kComment, QByteArrayLiteral("C"));
    setShortAscii(bytes, rowOffset + kAnalyzePath, QByteArrayLiteral("A.DAT"));
    setShortAscii(bytes, rowOffset + kFilePath, QByteArrayLiteral("F.MP3"));
}

inline QByteArray makePdbKeyImportFixture() {
    // Three fixed pages: root, KEYS, and TRACKS.
    QByteArray bytes(kPdbFileSize, '\0');

    // rekordbox_pdb.ksy root header.
    setU32Le(&bytes, 0x00, 0);
    setU32Le(&bytes, 0x04, kPdbPageSize);
    setU32Le(&bytes, 0x08, 2);
    setU32Le(&bytes, 0x0c, kPdbPageCount);
    setU32Le(&bytes, 0x10, 0);
    setU32Le(&bytes, 0x14, 1);
    setU32Le(&bytes, 0x18, 0);

    // table: KEYS (page 1), then TRACKS (page 2).
    setU32Le(&bytes, 0x1c, 5);
    setU32Le(&bytes, 0x20, 0);
    setU32Le(&bytes, 0x24, 1);
    setU32Le(&bytes, 0x28, 1);
    setU32Le(&bytes, 0x2c, 0);
    setU32Le(&bytes, 0x30, 0);
    setU32Le(&bytes, 0x34, 2);
    setU32Le(&bytes, 0x38, 2);

    constexpr qsizetype kHeapOffset = 0x28;
    const qsizetype keysPage = kPdbPageSize;
    const qsizetype tracksPage = 2 * kPdbPageSize;
    setPageHeader(&bytes, keysPage, 1, 5, 426, 10);
    setU32Le(&bytes, keysPage + kHeapOffset, 42);
    setU32Le(&bytes, keysPage + kHeapOffset + 4, 42);
    setShortAscii(&bytes, keysPage + kHeapOffset + 8, QByteArrayLiteral("C"));
    setPageFooter(&bytes, keysPage);

    setPageHeader(&bytes, tracksPage, 2, 0, 283, 153);
    setTrackRow(&bytes, tracksPage + kHeapOffset);
    setPageFooter(&bytes, tracksPage);

    return bytes;
}

} // namespace mixxx::rekordbox::test
