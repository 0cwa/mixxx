#pragma once

#include <QByteArray>

namespace mixxx::rekordbox::test {

// These bytes are a deterministic, synthetic subset of the checked-in
// rekordbox_anlz.ksy contract. They are not an authentic Rekordbox export and
// do not claim full-device compatibility. The fixture contains a PMAI file
// header followed by one PQTZ section. The section has a 12-byte header and a
// beat-grid body with two reserved u32 values, a beat count, and two beats
// (beat number, tempo * 100, time in milliseconds).
inline void appendU16Be(QByteArray* bytes, quint16 value) {
    bytes->append(static_cast<char>((value >> 8) & 0xff));
    bytes->append(static_cast<char>(value & 0xff));
}

inline void appendU32Be(QByteArray* bytes, quint32 value) {
    bytes->append(static_cast<char>((value >> 24) & 0xff));
    bytes->append(static_cast<char>((value >> 16) & 0xff));
    bytes->append(static_cast<char>((value >> 8) & 0xff));
    bytes->append(static_cast<char>(value & 0xff));
}

inline QByteArray makeAnlzBeatGridFixture() {
    QByteArray beatGridBody;
    appendU32Be(&beatGridBody, 0);
    appendU32Be(&beatGridBody, 0x00080000);
    appendU32Be(&beatGridBody, 2);
    appendU16Be(&beatGridBody, 1);
    appendU16Be(&beatGridBody, 12000);
    appendU32Be(&beatGridBody, 1000);
    appendU16Be(&beatGridBody, 2);
    appendU16Be(&beatGridBody, 12000);
    appendU32Be(&beatGridBody, 1500);

    QByteArray pqtz;
    pqtz.append("PQTZ", 4);
    appendU32Be(&pqtz, 12);
    appendU32Be(&pqtz, static_cast<quint32>(12 + beatGridBody.size()));
    pqtz.append(beatGridBody);

    QByteArray anlz;
    anlz.append("PMAI", 4);
    appendU32Be(&anlz, 12);
    appendU32Be(&anlz, static_cast<quint32>(12 + pqtz.size()));
    anlz.append(pqtz);
    return anlz;
}

} // namespace mixxx::rekordbox::test
