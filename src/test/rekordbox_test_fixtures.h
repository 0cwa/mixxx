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

inline QByteArray makeAnlzCueFixture(
        bool extended,
        quint32 listType,
        quint32 hotCueNumber,
        quint8 cueType = 1,
        bool includeSecondLoop = false) {
    QByteArray cueBody;
    appendU32Be(&cueBody, listType);
    const quint16 cueCount = includeSecondLoop ? 2 : 1;
    if (extended) {
        appendU16Be(&cueBody, cueCount);
        appendU16Be(&cueBody, 0);
    } else {
        appendU16Be(&cueBody, 0);
        appendU16Be(&cueBody, cueCount);
        appendU32Be(&cueBody, 0);
    }

    const auto appendCueEntry = [&](quint32 entryHotCueNumber,
                                        quint8 entryCueType,
                                        quint32 time,
                                        quint32 loopTime) {
        cueBody.append(extended ? "PCP2" : "PCPT", 4);
        appendU32Be(&cueBody, 0x20);

        if (extended) {
            appendU32Be(&cueBody, 44);
            appendU32Be(&cueBody, entryHotCueNumber);
            cueBody.append(static_cast<char>(entryCueType));
            cueBody.append(QByteArray(3, '\0'));
            appendU32Be(&cueBody, time);
            appendU32Be(&cueBody, loopTime);
            cueBody.append('\0');
            cueBody.append(QByteArray(7, '\0'));
            appendU16Be(&cueBody, 0);
            appendU16Be(&cueBody, 0);
            appendU32Be(&cueBody, 0);
        } else {
            appendU32Be(&cueBody, 56);
            appendU32Be(&cueBody, entryHotCueNumber);
            appendU32Be(&cueBody, 1);
            appendU32Be(&cueBody, 0x00010000);
            appendU16Be(&cueBody, 0xffff);
            appendU16Be(&cueBody, 0);
            cueBody.append(static_cast<char>(entryCueType));
            cueBody.append(QByteArray(3, '\0'));
            appendU32Be(&cueBody, time);
            appendU32Be(&cueBody, loopTime);
            cueBody.append(QByteArray(16, '\0'));
        }
    };

    appendCueEntry(hotCueNumber, cueType, 1000, 2000);
    if (includeSecondLoop) {
        appendCueEntry(0, 2, 2000, 3000);
    }

    QByteArray cueSection;
    cueSection.append(extended ? "PCO2" : "PCOB", 4);
    appendU32Be(&cueSection, 12);
    appendU32Be(&cueSection, static_cast<quint32>(12 + cueBody.size()));
    cueSection.append(cueBody);

    QByteArray anlz;
    anlz.append("PMAI", 4);
    appendU32Be(&anlz, 12);
    appendU32Be(&anlz, static_cast<quint32>(12 + cueSection.size()));
    anlz.append(cueSection);
    return anlz;
}

} // namespace mixxx::rekordbox::test
