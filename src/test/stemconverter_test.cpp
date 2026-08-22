#include "stems/stemconverter.h"

#include <gtest/gtest.h>

#include <QTemporaryFile>
#include <vector>

TEST(StemConverterTest, ConvertsStereoInterleavedAudioToPlanarAndBack) {
    const std::vector<float> interleaved{1.0f, 10.0f, 2.0f, 20.0f, 3.0f, 30.0f};
    const std::vector<float> expectedPlanar{1.0f, 2.0f, 3.0f, 10.0f, 20.0f, 30.0f};
    std::vector<float> planar;

    ASSERT_TRUE(StemConverter::interleavedToPlanar(interleaved, 2, 3, &planar));
    EXPECT_EQ(planar, expectedPlanar);

    std::vector<float> roundTrip;
    ASSERT_TRUE(StemConverter::planarToInterleaved(planar, 2, 3, &roundTrip));
    EXPECT_EQ(roundTrip, interleaved);
}

TEST(StemConverterTest, RejectsInvalidAudioLayout) {
    const std::vector<float> samples{1.0f, 2.0f, 3.0f};
    std::vector<float> output;

    EXPECT_FALSE(StemConverter::interleavedToPlanar(samples, 0, 3, &output));
    EXPECT_FALSE(StemConverter::interleavedToPlanar(samples, 2, 3, &output));
    EXPECT_FALSE(StemConverter::planarToInterleaved(samples, 2, 3, &output));
    EXPECT_FALSE(StemConverter::planarToInterleaved(samples, 2, 0, &output));
}

TEST(StemConverterTest, UsesPathAsIdentityForTemporaryTrack) {
    QTemporaryFile inputFile;
    ASSERT_TRUE(inputFile.open());

    const auto pTrack = Track::newTemporary(inputFile.fileName());
    ASSERT_TRUE(pTrack);
    EXPECT_FALSE(pTrack->getId().isValid());
    EXPECT_TRUE(StemConverter::isValidConversionInput(pTrack));
}

TEST(StemConverterTest, DetectsAudioThatNeedsModelFormatConversion) {
    EXPECT_FALSE(StemConverter::requiresAudioFormatConversion(44100, 2));
    EXPECT_TRUE(StemConverter::requiresAudioFormatConversion(48000, 2));
    EXPECT_TRUE(StemConverter::requiresAudioFormatConversion(44100, 1));
}
