#include "library/rekordbox/rekordboxanlzimporter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "library/rekordbox/rekordboxconstants.h"
#include "track/track.h"
#include "util/sandbox.h"

namespace mixxx::rekordbox {
namespace {

QStringList annotationIdentities(const AnlzCue& cue) {
    if (cue.hotSlot) {
        return {QStringLiteral("hot:%1").arg(*cue.hotSlot)};
    }
    QStringList identities;
    for (const auto& provenance : cue.provenance) {
        identities.append(QStringLiteral("memory:%1:%2:%3:%4")
                        .arg(QString::fromStdString(provenance.sourceIdentity),
                                QString::fromStdString(provenance.section))
                        .arg(provenance.sectionIndex)
                        .arg(provenance.ordinal));
    }
    return identities;
}

bool isLoop(const AnlzCue& cue) {
    return cue.role == AnlzCueRole::MemoryLoop || cue.role == AnlzCueRole::HotLoop;
}

RgbColor::optional_t cueColor(const AnlzCue& cue) {
    if ((cue.paletteId && *cue.paletteId == 0) ||
            (cue.hotColorCode && *cue.hotColorCode == 0 && cue.colorRgb && *cue.colorRgb == 0)) {
        return RgbColor::optional(PredefinedColorPalettes::kDefaultCueColor);
    }
    if (cue.colorRgb && RgbColor::isValidCode(*cue.colorRgb)) {
        return RgbColor::optional(*cue.colorRgb);
    }
    if (cue.paletteId) {
        return anlzPaletteColor(*cue.paletteId);
    }
    return RgbColor::nullopt();
}

bool matches(const CuePointer& cue,
        CueType type,
        int slot,
        audio::FramePos start,
        audio::FramePos end,
        const AnlzCue& source,
        RgbColor::optional_t color) {
    return cue->getType() == type && cue->getHotCue() == slot &&
            cue->getPosition() == start && cue->getEndPosition() == end &&
            (!source.labelUtf8 || cue->getLabel() == QString::fromStdString(*source.labelUtf8)) &&
            (!color || cue->getColor() == *color) &&
            (!source.activeLoop || cue->getActiveLoopIntent() == source.activeLoop);
}

} // namespace

AnlzSourceSnapshot readAnlzSources(const QString& anlzPath, const AnlzSourceSnapshot* cached) {
    using namespace mixxx::rekordbox;
    std::array<QByteArray, 3> bytes;
    const int suffixOffset = anlzPath.lastIndexOf(QLatin1Char('.'));
    const bool hasSuffix = suffixOffset > anlzPath.lastIndexOf(QLatin1Char('/'));
    const QString stem = anlzPath.left(suffixOffset + 1);
    const std::array<QString, 3> paths{
            anlzPath, stem + QStringLiteral("EXT"), stem + QStringLiteral("2EX")};
    const std::array<AnlzSource, 3> kinds{AnlzSource::Dat, AnlzSource::Ext, AnlzSource::TwoEx};
    std::vector<AnlzInput> inputs;
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    AnlzDecodeOptions options;
    options.preserveRawSections = false;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0 && !hasSuffix) {
            continue;
        }
        mixxx::FileInfo fileInfo(paths[i]);
        const auto securityToken = Sandbox::openSecurityToken(&fileInfo, true);
        QFile file(paths[i]);
        AnlzStatus availability = AnlzStatus::Success;
        if (!file.open(QIODevice::ReadOnly)) {
            availability = file.error() == QFileDevice::PermissionsError
                    ? AnlzStatus::Denied
                    : file.exists() ? AnlzStatus::Failed
                                    : AnlzStatus::Unavailable;
        } else if (file.size() > static_cast<qint64>(options.limits.maxFileBytes)) {
            availability = AnlzStatus::Invalid;
        } else {
            bytes[i] = file.read(static_cast<qint64>(options.limits.maxFileBytes) + 1);
            if (file.error() != QFileDevice::NoError) {
                availability = AnlzStatus::Failed;
            }
        }
        fingerprint.addData(QByteArray::number(paths[i].toUtf8().size()).append(':'));
        fingerprint.addData(paths[i].toUtf8());
        fingerprint.addData(QByteArray::number(static_cast<int>(availability)).append(':'));
        fingerprint.addData(QByteArray::number(bytes[i].size()).append(':'));
        fingerprint.addData(bytes[i]);
        if (i == 0 || availability != AnlzStatus::Unavailable) {
            inputs.push_back({kinds[i],
                    paths[i].toStdString(),
                    std::string_view(bytes[i].constData(), bytes[i].size()),
                    availability});
        }
    }
    const auto digest = fingerprint.result();
    if (cached && cached->fingerprint == digest) {
        return *cached;
    }
    return {decodeAnlzCompanions(inputs, options), digest};
}

RgbColor::optional_t anlzPaletteColor(std::uint8_t paletteId) {
    constexpr std::array<std::uint32_t, 9> codes{0x000000,
            0xF870F8,
            0xF870900,
            0xF8A030,
            0xF8E331,
            0x1EE000,
            0x16C0F8,
            0x0150F8,
            0x9808F8};
    if (paletteId == 0 || paletteId >= codes.size() || !RgbColor::isValidCode(codes[paletteId])) {
        return RgbColor::nullopt();
    }
    return RgbColor::optional(codes[paletteId]);
}

AnlzApplyResult applyAnlzTrackAnnotations(const TrackPointer& track,
        audio::SampleRate sampleRate,
        double timingCorrectionMilliseconds,
        const AnlzData& data,
        const library::MainCuePromotionPolicy& promotionPolicy,
        AnlzImportState* state) {
    AnlzApplyResult result;
    if (!track || !sampleRate.isValid() ||
            !std::isfinite(anlzMillisecondsToFrames(
                    0, sampleRate.value(), timingCorrectionMilliseconds)) ||
            !state) {
        result.complete = false;
        result.diagnostics << QStringLiteral("Invalid analysis application target or timing");
        return result;
    }
    if (state->track.lock() != track) {
        *state = AnlzImportState();
        state->track = track;
    }
    state->sampleRate = sampleRate;
    state->timingCorrectionMilliseconds = timingCorrectionMilliseconds;
    state->promoteFirstMemoryCue = promotionPolicy.promoteFirstMemoryCue;

    if (!data.beats.empty()) {
        QVector<audio::FramePos> positions;
        positions.reserve(data.beats.size());
        for (const auto& beat : data.beats) {
            positions.append(audio::FramePos(anlzMillisecondsToFrames(
                    beat.timeMilliseconds, sampleRate.value(), timingCorrectionMilliseconds)));
        }
        const auto beatInBar = data.beats.front().beatInBar;
        const int phase = beatInBar >= 1 && beatInBar <= 4 ? (5 - beatInBar) % 4 : 0;
        BeatsPointer beats;
        if (positions.size() == 1) {
            const Bpm bpm(data.beats.front().tempoHundredths / 100.0);
            if (positions.front().isValid() && bpm.isValid()) {
                beats = Beats::fromConstTempo(sampleRate,
                        positions.front().toLowerFrameBoundary(),
                        bpm,
                        rekordboxconstants::beatsSubversion,
                        phase);
            }
        } else {
            beats = Beats::fromBeatPositions(sampleRate,
                    positions,
                    rekordboxconstants::beatsSubversion,
                    phase);
        }
        if (!beats) {
            result.complete = false;
            result.diagnostics << QStringLiteral("Analysis beat grid has invalid timing or tempo");
        } else {
            const auto current = track->getBeats();
            const bool changedLocally = (!current && state->beats) ||
                    (current && (!state->beats || *current != *state->beats) && *current != *beats);
            if (!changedLocally &&
                    ((current && *current == *beats) || track->trySetBeats(beats))) {
                state->beats = beats;
            } else {
                result.complete = false;
                result.diagnostics << QStringLiteral(
                        "Preserved an existing or BPM-locked beat grid");
            }
        }
    }

    bool promoted = false;
    QSet<const Cue*> matched;
    for (const auto& source : data.cues) {
        const bool hot = source.role == AnlzCueRole::HotCue || source.role == AnlzCueRole::HotLoop;
        if ((hot &&
                    (!source.hotSlot ||
                            *source.hotSlot >
                                    std::numeric_limits<int>::max())) ||
                (isLoop(source) &&
                        (!source.endMilliseconds ||
                                *source.endMilliseconds <=
                                        source.startMilliseconds))) {
            result.complete = false;
            result.diagnostics << QStringLiteral("Invalid cue interval or hot-cue slot");
            continue;
        }
        const int slot = hot ? static_cast<int>(*source.hotSlot) : Cue::kNoHotCue;
        const CueType type = hot ? (isLoop(source) ? CueType::Loop : CueType::HotCue)
                                 : CueType::Memory;
        const auto start = audio::FramePos(anlzMillisecondsToFrames(
                source.startMilliseconds, sampleRate.value(), timingCorrectionMilliseconds));
        const auto end = isLoop(source)
                ? audio::FramePos(
                          anlzMillisecondsToFrames(*source.endMilliseconds,
                                  sampleRate.value(),
                                  timingCorrectionMilliseconds))
                : audio::kInvalidFramePos;
        const auto color = cueColor(source);
        if (source.paletteId && *source.paletteId != 0 && !source.colorRgb && !color) {
            result.complete = false;
            result.diagnostics
                    << QStringLiteral("Preserved unresolved palette ID %1")
                               .arg(*source.paletteId);
        }
        const bool promoteThisCue = !hot && promotionPolicy.shouldPromote(promoted, isLoop(source));
        promoted = promoted || promoteThisCue;

        const auto identities = annotationIdentities(source);
        std::shared_ptr<AnlzAppliedCue> existing;
        bool conflictingAliases = false;
        for (const auto& identity : identities) {
            if (state->cues.contains(identity)) {
                const auto candidate = state->cues.value(identity);
                if (existing && existing->cue != candidate->cue) {
                    conflictingAliases = true;
                    break;
                }
                existing = candidate;
            }
        }
        if (conflictingAliases) {
            result.complete = false;
            result.diagnostics << QStringLiteral(
                    "Preserved annotations with conflicting source identities");
            continue;
        }
        const QList<CuePointer> currentCues = track->getCuePoints();
        CuePointer cue;
        bool owned = false;
        if (existing) {
            if (!currentCues.contains(existing->cue) ||
                    existing->cue->getCueInfo(existing->sampleRate) != existing->snapshot) {
                result.complete = false;
                result.diagnostics << QStringLiteral(
                        "Preserved a locally edited or deleted annotation");
                continue;
            }
            if (!hot && matched.contains(existing->cue.get())) {
                existing.reset();
            }
        }
        if (existing) {
            cue = existing->cue;
            owned = true;
        } else if (hot) {
            cue = track->findHotcueByIndex(slot);
            if (cue && !matches(cue, type, slot, start, end, source, color)) {
                result.complete = false;
                result.diagnostics
                        << QStringLiteral("Preserved occupied hot-cue slot %1")
                                   .arg(slot);
                continue;
            }
        } else {
            for (const auto& candidate : currentCues) {
                if (!matched.contains(candidate.get()) &&
                        matches(candidate, type, slot, start, end, source, color)) {
                    cue = candidate;
                    break;
                }
            }
        }
        if (!cue) {
            cue = track->createAndAddCue(type, slot, start, end);
            owned = true;
        }
        matched.insert(cue.get());
        if (owned) {
            cue->setType(type);
            cue->setStartAndEndPosition(start, end);
            if (source.labelUtf8) {
                cue->setLabel(QString::fromStdString(*source.labelUtf8));
            }
            if (color) {
                cue->setColor(*color);
            }
            if (source.activeLoop || !isLoop(source)) {
                cue->setActiveLoopIntent(isLoop(source) ? source.activeLoop : std::nullopt);
            }
            if (!existing) {
                existing = std::make_shared<AnlzAppliedCue>();
            }
            *existing = {cue, cue->getCueInfo(sampleRate), sampleRate};
            for (const auto& identity : identities) {
                state->cues.insert(identity, existing);
            }
        }

        if (promoteThisCue) {
            const auto mainCue = track->findCueByType(CueType::MainCue);
            if (state->promotedMainCue &&
                    (!mainCue ||
                            mainCue->getCueInfo(
                                    state->promotedMainCueSampleRate) !=
                                    *state->promotedMainCue)) {
                result.complete = false;
                result.diagnostics << QStringLiteral(
                        "Preserved a locally edited or deleted MainCue");
            } else if (!mainCue || !mainCue->getPosition().isValid() ||
                    state->promotedMainCue) {
                track->setMainCuePosition(start);
                auto promotedCue = track->findCueByType(CueType::MainCue);
                if (!promotedCue) {
                    promotedCue = track->createAndAddCue(CueType::MainCue,
                            Cue::kNoHotCue,
                            start,
                            audio::kInvalidFramePos);
                }
                if (promotedCue) {
                    if (source.labelUtf8) {
                        promotedCue->setLabel(QString::fromStdString(*source.labelUtf8));
                    }
                    if (color) {
                        promotedCue->setColor(*color);
                    }
                    state->promotedMainCue = promotedCue->getCueInfo(sampleRate);
                    state->promotedMainCueSampleRate = sampleRate;
                }
            }
        }
    }
    return result;
}

AnlzApplyResult importAnlzSources(const TrackPointer& track,
        audio::SampleRate sampleRate,
        double timingCorrectionMilliseconds,
        const QString& anlzPath,
        const library::MainCuePromotionPolicy& promotionPolicy,
        AnlzImportState* state) {
    AnlzApplyResult result;
    if (!track || !sampleRate.isValid() ||
            !std::isfinite(anlzMillisecondsToFrames(
                    0, sampleRate.value(), timingCorrectionMilliseconds)) ||
            anlzPath.isEmpty() || !state) {
        result.complete = false;
        result.diagnostics << QStringLiteral("Invalid analysis source, target or timing");
        return result;
    }
    const auto source = readAnlzSources(anlzPath,
            state->decodedSource.decoded.usable() ? &state->decodedSource : nullptr);
    for (const auto& diagnostic : source.decoded.diagnostics) {
        if (diagnostic.status != AnlzStatus::Unsupported) {
            const auto message = QString::fromStdString(diagnostic.message);
            result.diagnostics << (diagnostic.sourceIdentity.empty()
                            ? message
                            : QString::fromStdString(
                                      diagnostic.sourceIdentity) +
                                    QStringLiteral(": ") + message);
        }
    }
    const bool onlyUnsupportedCapabilities =
            std::all_of(source.decoded.diagnostics.cbegin(),
                    source.decoded.diagnostics.cend(),
                    [](const auto& diagnostic) {
                        return diagnostic.status == AnlzStatus::Unsupported;
                    });
    result.complete = source.decoded.usable() && onlyUnsupportedCapabilities;
    if (source.decoded.usable() &&
            (state->track.lock() != track ||
                    state->fingerprint != source.fingerprint ||
                    state->sampleRate != sampleRate ||
                    state->timingCorrectionMilliseconds !=
                            timingCorrectionMilliseconds ||
                    state->promoteFirstMemoryCue !=
                            promotionPolicy.promoteFirstMemoryCue)) {
        state->fingerprint.clear();
        const auto applied = applyAnlzTrackAnnotations(track,
                sampleRate,
                timingCorrectionMilliseconds,
                *source.decoded.data,
                promotionPolicy,
                state);
        result.complete = result.complete && applied.complete;
        result.diagnostics.append(applied.diagnostics);
        if (result.complete) {
            state->fingerprint = source.fingerprint;
        }
    }
    state->decodedSource = source;
    return result;
}

} // namespace mixxx::rekordbox
