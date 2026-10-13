#pragma once

#include <QtGui/qrgb.h>

#include <QByteArray>
#include <QString>

namespace mixxx::rekordbox::test::hotloop {

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

// Adapted from PR75's synthetic PCOB/PCO2 fixture, not a device export.
inline QByteArray makeAnlzCueFixture(bool extended,
        quint32 listType,
        quint32 hotCueNumber,
        quint8 cueType = 1,
        quint32 time = 1000,
        quint32 loopTime = 2000,
        const QString& comment = {},
        QRgb color = qRgb(0, 0, 0)) {
    QByteArray commentBytes;
    for (const QChar codeUnit : comment) {
        appendU16Be(&commentBytes, codeUnit.unicode());
    }
    if (!comment.isEmpty()) {
        appendU16Be(&commentBytes, 0);
    }
    QByteArray entry;
    entry.append(extended ? "PCP2" : "PCPT", 4);
    appendU32Be(&entry, 0x20);
    appendU32Be(&entry, extended ? 48 + commentBytes.size() : 56);
    appendU32Be(&entry, hotCueNumber);
    if (!extended) {
        appendU32Be(&entry, 1);
        appendU32Be(&entry, 0x00010000);
        appendU16Be(&entry, 0xffff);
        appendU16Be(&entry, 0);
    }
    entry.append(static_cast<char>(cueType));
    entry.append(QByteArray(3, '\0'));
    appendU32Be(&entry, time);
    appendU32Be(&entry, loopTime);
    if (extended) {
        entry.append(QByteArray(8, '\0'));
        appendU16Be(&entry, 0);
        appendU16Be(&entry, 0);
        appendU32Be(&entry, commentBytes.size());
        entry.append(commentBytes);
        entry.append('\0');
        entry.append(static_cast<char>(qRed(color)));
        entry.append(static_cast<char>(qGreen(color)));
        entry.append(static_cast<char>(qBlue(color)));
    } else {
        entry.append(QByteArray(16, '\0'));
    }

    QByteArray body;
    appendU32Be(&body, listType);
    if (extended) {
        appendU16Be(&body, 1);
        appendU16Be(&body, 0);
    } else {
        appendU16Be(&body, 0);
        appendU16Be(&body, 1);
        appendU32Be(&body, 0);
    }
    body.append(entry);
    QByteArray section;
    section.append(extended ? "PCO2" : "PCOB", 4);
    appendU32Be(&section, 12);
    appendU32Be(&section, 12 + body.size());
    section.append(body);
    QByteArray file;
    file.append("PMAI", 4);
    appendU32Be(&file, 12);
    appendU32Be(&file, 12 + section.size());
    file.append(section);
    return file;
}

} // namespace mixxx::rekordbox::test::hotloop
