#pragma once

#include <QByteArray>

namespace mixxx::rekordbox::test {

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

// A minimal synthetic ANLZ cue fixture. Extended entries include a zero-length
// comment and explicit color bytes required by the PCP2 schema.
inline QByteArray makeAnlzCueFixture(
        bool extended,
        quint32 listType,
        quint32 hotCueNumber,
        quint8 cueType = 1,
        quint32 time = 1000,
        quint32 loopTime = 2000) {
    QByteArray cueBody;
    appendU32Be(&cueBody, listType);
    if (extended) {
        appendU16Be(&cueBody, 1);
        appendU16Be(&cueBody, 0);
    } else {
        appendU16Be(&cueBody, 0);
        appendU16Be(&cueBody, 1);
        appendU32Be(&cueBody, 0);
    }

    cueBody.append(extended ? "PCP2" : "PCPT", 4);
    appendU32Be(&cueBody, 0x20);
    if (extended) {
        appendU32Be(&cueBody, 48);
        appendU32Be(&cueBody, hotCueNumber);
        cueBody.append(static_cast<char>(cueType));
        cueBody.append(QByteArray(3, '\0'));
        appendU32Be(&cueBody, time);
        appendU32Be(&cueBody, loopTime);
        cueBody.append('\0');
        cueBody.append(QByteArray(7, '\0'));
        appendU16Be(&cueBody, 0);
        appendU16Be(&cueBody, 0);
        appendU32Be(&cueBody, 0);
        cueBody.append(QByteArray(4, '\0'));
    } else {
        appendU32Be(&cueBody, 56);
        appendU32Be(&cueBody, hotCueNumber);
        appendU32Be(&cueBody, 1);
        appendU32Be(&cueBody, 0x00010000);
        appendU16Be(&cueBody, 0xffff);
        appendU16Be(&cueBody, 0);
        cueBody.append(static_cast<char>(cueType));
        cueBody.append(QByteArray(3, '\0'));
        appendU32Be(&cueBody, time);
        appendU32Be(&cueBody, loopTime);
        cueBody.append(QByteArray(16, '\0'));
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
