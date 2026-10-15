#include <gtest/gtest.h>

#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

#include "sources/soundsourceffmpeg.h"
#include "sources/soundsourceproxy.cpp"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/samplebuffer.h"

using namespace mixxx;

namespace {

const std::vector<std::string> supportedCodecs = {
#if !defined(Q_OS_WIN)
        "AAC_256kbps_VBR",
#endif
        "ALAC_24bit"};

const QList<QString> kStemFiles = {
        "01-drum.wav",
        "02-bass.wav",
        "03-melody.wav",
        "04-vocal.wav",
};

// This could probably also be done with test Params like supportedCodecs
struct StemFileInfo {
    QString dir;
    QString title;
};

static const std::array<StemFileInfo, 2> kStemFileInfos = {
        StemFileInfo{QStringLiteral("stem01"), QStringLiteral("sin")},
        StemFileInfo{QStringLiteral("stem02"), QStringLiteral("trance")}};

// AI-generated sample-oracle correction begins.
// Every finite sample is compared. References below account for codec loss
// through their source, rather than widening an observed error tolerance.

constexpr CSAMPLE kReferenceUnreadSentinel = -12345.5f;
constexpr CSAMPLE kActualUnreadSentinel = 23456.75f;

testing::AssertionResult samplesWereWritten(
        const SampleBuffer& samples, CSAMPLE sentinel) {
    for (SINT sample = 0; sample < samples.size(); ++sample) {
        if (!std::isfinite(samples.data()[sample]) || samples.data()[sample] == sentinel) {
            return testing::AssertionFailure() << "Unwritten or non-finite sample at " << sample;
        }
    }
    return testing::AssertionSuccess();
}

testing::AssertionResult compareStemSamples(
        const SampleBuffer& expected,
        const SampleBuffer& actual) {
    if (expected.size() != actual.size() || expected.size() == 0) {
        return testing::AssertionFailure() << "Invalid sample counts: "
                                           << expected.size() << " / " << actual.size();
    }
    double maximumDifference = 0.0;
    double squaredDifferences = 0.0;
    SINT largestDifferenceIndex = 0;
    for (SINT sample = 0; sample < expected.size(); ++sample) {
        if (!std::isfinite(expected.data()[sample]) ||
                !std::isfinite(actual.data()[sample])) {
            return testing::AssertionFailure() << "Non-finite sample at " << sample;
        }
        const double difference = std::abs(
                double(expected.data()[sample]) - double(actual.data()[sample]));
        squaredDifferences += difference * difference;
        if (difference > maximumDifference) {
            maximumDifference = difference;
            largestDifferenceIndex = sample;
        }
    }
    const double rmsDifference = std::sqrt(squaredDifferences / expected.size());
    qInfo() << "STEM sample comparison" << "samples" << expected.size()
            << "maximumDifference" << maximumDifference
            << "rmsDifference" << rmsDifference;
    if (maximumDifference != 0.0) {
        return testing::AssertionFailure()
                << "Sample " << largestDifferenceIndex << ": expected "
                << expected.data()[largestDifferenceIndex] << ", actual "
                << actual.data()[largestDifferenceIndex]
                << ", maximum difference " << maximumDifference
                << ", RMS difference " << rmsDifference;
    }
    return testing::AssertionSuccess();
}

TEST(StemSampleComparisonTest, RejectsLastSampleMismatch) {
    SampleBuffer expected(1024), actual(1024);
    std::fill_n(expected.data(), expected.size(), 0.0f);
    std::fill_n(actual.data(), actual.size(), 0.0f);
    actual.data()[actual.size() - 1] = 0.125f;
    EXPECT_FALSE(compareStemSamples(expected, actual));
}
// End AI-generated sample-oracle correction.

// must be a std::tuple for std::combine in INSTANTIATE_TEST_SUITE_P
using StemParam = std::tuple<std::string, StemFileInfo>;

class StemFixture : public MixxxTest, public ::testing::WithParamInterface<StemParam> {
  protected:
    void SetUp() override {
        ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported("stem.mp4") ||
                SoundSourceProxy::registerProviders());
    }

    // AI-generated encoded reference selection begins.
    // Independently decode the same encoded payload with matching configuration
    // and read history. This gives an exact numerical reference even for AAC;
    // it does not claim equality with a WAV before lossy encoding/mastering.
    std::unique_ptr<SoundSourceFFmpeg> CreateReferenceStem(int stemIndex) {
        return std::make_unique<SoundSourceFFmpeg>(
                QUrl::fromLocalFile(GetStemFilePath()), stemIndex + 1);
    }
    // End AI-generated encoded reference selection.

    QString GetStemFilePath() {
        const auto& [codec, info] = GetParam();
        return getTestDir().filePath(getTestDir()
                        .filePath("stems/%1/%2_%3.stem.mp4")
                        .arg(info.dir,
                                info.title,
                                QString::fromStdString(codec)));
    }
};

TEST_P(StemFixture, FetchStemInfo) {
    auto sourceStemPath = GetStemFilePath();
    TrackPointer pTrack(Track::newTemporary(sourceStemPath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));

    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    ASSERT_EQ(stemInfo.at(0), StemInfo("Drums", QColor(0xfd, 0x4a, 0x4a)));  // #fd4a4a
    ASSERT_EQ(stemInfo.at(1), StemInfo("Bass", QColor(0xff, 0xff, 0x00)));   // #ffff00
    ASSERT_EQ(stemInfo.at(2), StemInfo("Synths", QColor(0x00, 0xe8, 0xe8))); // #00e8e8
    ASSERT_EQ(stemInfo.at(3), StemInfo("Vox", QColor(0xad, 0x65, 0xff)));    // #ad65ff
}

TEST_P(StemFixture, FetchStemEmptyInfo) {
    TrackPointer pTrack(Track::newTemporary(
            getTestDir().filePath("stems/stem01/test_missing_stem_details.stem.mp4")));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));

    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    ASSERT_EQ(stemInfo.at(0), StemInfo("Stem #1", QColor(0x00, 0x9E, 0x73)));
    ASSERT_EQ(stemInfo.at(1), StemInfo("Stem #2", QColor(0xD5, 0x5E, 0x00)));
    ASSERT_EQ(stemInfo.at(2), StemInfo("Stem #3", QColor(0xCC, 0x79, 0xA7)));
    ASSERT_EQ(stemInfo.at(3), StemInfo("Stem #4", QColor(0x56, 0xB4, 0xE9)));
}

// AI-generated decoded-stem sample coverage begins.
TEST_P(StemFixture, ReadMainMix) {
    const auto& [codec, info] = GetParam();
    ASSERT_TRUE(codec == "ALAC_24bit" || codec == "AAC_256kbps_VBR");
    SCOPED_TRACE(codec + " / " + info.dir.toStdString());
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(GetStemFilePath()));
    AudioSource::OpenParams config;
    config.setChannelCount(audio::ChannelCount::stereo());
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    // The provider deliberately skips the premastered stream. Its stereo
    // output is the unclipped sum of the four independently decoded stems.
    const auto requestedRange = IndexRange::between(0, 512);
    SampleBuffer reference(1024), actual(1024);
    std::fill_n(reference.data(), reference.size(), 0.0f);
    std::fill_n(actual.data(), actual.size(), kActualUnreadSentinel);
    for (int stemIndex = 0; stemIndex < kStemFiles.size(); ++stemIndex) {
        SCOPED_TRACE(stemIndex);
        auto sourceReference = CreateReferenceStem(stemIndex);
        ASSERT_EQ(sourceReference->open(AudioSource::OpenMode::Strict, config),
                AudioSource::OpenResult::Succeeded);
        ASSERT_EQ(sourceReference->getSignalInfo(), sourceStem.getSignalInfo());
        SampleBuffer decodedStem(reference.size());
        std::fill_n(decodedStem.data(), decodedStem.size(), kReferenceUnreadSentinel);
        const auto decoded = sourceReference->readSampleFrames(
                WritableSampleFrames(requestedRange,
                        SampleBuffer::WritableSlice(
                                decodedStem.data(), decodedStem.size())));
        ASSERT_EQ(decoded.frameIndexRange(), requestedRange);
        ASSERT_EQ(decoded.readableLength(), decodedStem.size());
        ASSERT_TRUE(samplesWereWritten(decodedStem, kReferenceUnreadSentinel));
        for (SINT sample = 0; sample < reference.size(); ++sample) {
            reference.data()[sample] += decodedStem.data()[sample];
        }
        qInfo() << "STEM reference decoded" << QString::fromStdString(codec)
                << info.dir << "stream" << stemIndex + 1 << "samples" << decodedStem.size();
    }
    const auto mixed = sourceStem.readSampleFrames(WritableSampleFrames(
            requestedRange, SampleBuffer::WritableSlice(actual.data(), actual.size())));
    ASSERT_EQ(mixed.frameIndexRange(), requestedRange);
    ASSERT_EQ(mixed.readableLength(), actual.size());
    ASSERT_TRUE(samplesWereWritten(actual, kActualUnreadSentinel));
    qInfo() << "STEM main mix read" << QString::fromStdString(codec)
            << info.dir << "samples" << actual.size();
    EXPECT_TRUE(compareStemSamples(reference, actual));
}

TEST_P(StemFixture, ReadEachStem) {
    const auto& [codec, info] = GetParam();
    ASSERT_TRUE(codec == "ALAC_24bit" || codec == "AAC_256kbps_VBR");
    SCOPED_TRACE(codec + " / " + info.dir.toStdString());
    const auto requestedRange = IndexRange::between(0, 512);
    for (int stemIndex = 0; stemIndex < kStemFiles.size(); ++stemIndex) {
        SCOPED_TRACE(stemIndex);
        auto sourceReference = CreateReferenceStem(stemIndex);
        SoundSourceSTEM sourceStem(QUrl::fromLocalFile(GetStemFilePath()));
        AudioSource::OpenParams referenceConfig;
        referenceConfig.setChannelCount(audio::ChannelCount::stereo());
        ASSERT_EQ(sourceReference->open(AudioSource::OpenMode::Strict, referenceConfig),
                AudioSource::OpenResult::Succeeded);
        auto selectedStemConfig = referenceConfig;
        selectedStemConfig.setStemMask(StemChannelSelection::fromInt(1U << stemIndex));
        ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, selectedStemConfig),
                AudioSource::OpenResult::Succeeded);
        ASSERT_EQ(sourceReference->getSignalInfo(), sourceStem.getSignalInfo());
        SampleBuffer reference(1024), actual(1024);
        std::fill_n(reference.data(), reference.size(), kReferenceUnreadSentinel);
        std::fill_n(actual.data(), actual.size(), kActualUnreadSentinel);
        const auto decodedReference = sourceReference->readSampleFrames(WritableSampleFrames(
                requestedRange, SampleBuffer::WritableSlice(reference.data(), reference.size())));
        ASSERT_EQ(decodedReference.frameIndexRange(), requestedRange);
        ASSERT_EQ(decodedReference.readableLength(), reference.size());
        ASSERT_TRUE(samplesWereWritten(reference, kReferenceUnreadSentinel));
        const auto decodedActual = sourceStem.readSampleFrames(WritableSampleFrames(
                requestedRange, SampleBuffer::WritableSlice(actual.data(), actual.size())));
        ASSERT_EQ(decodedActual.frameIndexRange(), requestedRange);
        ASSERT_EQ(decodedActual.readableLength(), actual.size());
        ASSERT_TRUE(samplesWereWritten(actual, kActualUnreadSentinel));
        qInfo() << "STEM selected stem read" << QString::fromStdString(codec)
                << info.dir << "stream" << stemIndex + 1 << "samples" << actual.size();
        EXPECT_TRUE(compareStemSamples(reference, actual));
    }
}
// End AI-generated decoded-stem sample coverage.

TEST_P(StemFixture, OpenStem) {
    auto sourceStemPath = GetStemFilePath();
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(sourceStemPath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(8));
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    ASSERT_EQ(mixxx::audio::SignalInfo(mixxx::audio::ChannelCount::stem(),
                      mixxx::audio::SampleRate(44100)),
            sourceStem.getSignalInfo());
}

INSTANTIATE_TEST_SUITE_P(
        StemTest,
        StemFixture,
        ::testing::Combine(
                ::testing::ValuesIn(supportedCodecs),
                ::testing::ValuesIn(kStemFileInfos)),
        [](const testing::TestParamInfo<StemFixture::ParamType>& info) {
            return std::get<0>(info.param) + "_" +
                    std::get<1>(info.param).title.toStdString();
        });

} // namespace
