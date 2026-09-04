#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QtDebug>
#include <QVector>

#include "engine/bufferscalers/enginebufferscalelinear.h"
#include "engine/readaheadmanager.h"
#include "test/mixxxtest.h"
#include "util/math.h"
#include "util/sample.h"
#include "util/types.h"

using ::testing::StrictMock;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::_;

namespace {

class ReadAheadManagerMock : public ReadAheadManager {
  public:
    ReadAheadManagerMock()
            : ReadAheadManager(),
              m_pBuffer(NULL),
              m_iBufferSize(0),
              m_iReadPosition(0),
              m_iSamplesRead(0) {
    }

    SINT getNextSamplesFake(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount) {
        Q_UNUSED(dRate);
        Q_UNUSED(channelCount);
        bool hasBuffer = m_pBuffer != NULL;
        // You forgot to set the mock read buffer.
        EXPECT_TRUE(hasBuffer);

        for (SINT i = 0; i < requested_samples; ++i) {
            buffer[i] = hasBuffer ? m_pBuffer[m_iReadPosition++ % m_iBufferSize] : 0;
        }
        m_iSamplesRead += requested_samples;
        return requested_samples;
    }

    SINT getNextSamplesOneFrame(double dRate,
            CSAMPLE* buffer,
            SINT requested_samples,
            mixxx::audio::ChannelCount channelCount) {
        Q_UNUSED(dRate);
        bool hasBuffer = m_pBuffer != NULL;
        EXPECT_TRUE(hasBuffer);
        if (!hasBuffer) {
            return 0;
        }

        const SINT frame_samples = channelCount.value();
        EXPECT_GE(requested_samples, frame_samples);
        for (SINT i = 0; i < frame_samples; ++i) {
            buffer[i] = m_pBuffer[m_iReadPosition++ % m_iBufferSize];
        }
        m_iSamplesRead += frame_samples;
        ++m_iOneFrameReadCalls;
        return frame_samples;
    }

    void setReadBuffer(CSAMPLE* pBuffer, SINT iBufferSize) {
        m_pBuffer = pBuffer;
        m_iBufferSize = iBufferSize;
        m_iReadPosition = 0;
    }

    int getSamplesRead() {
        return m_iSamplesRead;
    }

    int getOneFrameReadCalls() {
        return m_iOneFrameReadCalls;
    }

    MOCK_METHOD4(getNextSamples,
            SINT(double dRate,
                    CSAMPLE* buffer,
                    SINT requested_samples,
                    mixxx::audio::ChannelCount channelCount));

    CSAMPLE* m_pBuffer;
    SINT m_iBufferSize;
    SINT m_iReadPosition;
    SINT m_iSamplesRead;
    int m_iOneFrameReadCalls = 0;
};

} // namespace

// Keep this fixture at file scope so FRIEND_TEST in the scaler header matches
// the generated test class.
class EngineBufferScaleLinearTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pReadAheadMock = new StrictMock<ReadAheadManagerMock>();
        m_pScaler = new EngineBufferScaleLinear(m_pReadAheadMock);
    }

    void TearDown() override {
        delete m_pScaler;
        delete m_pReadAheadMock;
    }

    void SetRate(double rate) {
        double tempoRatio = rate;
        double pitchRatio = rate;
        m_pScaler->setSignal(mixxx::audio::SampleRate(44100),
                mixxx::audio::ChannelCount::stereo());
        m_pScaler->setScaleParameters(
                1.0, &tempoRatio, &pitchRatio);
    }

    void SetRateNoLerp(double rate) {
        // Set it twice to prevent rate LERP'ing
        SetRate(rate);
        SetRate(rate);
    }

    void ClearBuffer(CSAMPLE* pBuffer, int length) {
        SampleUtil::clear(pBuffer, length);
    }

    void FillBuffer(CSAMPLE* pBuffer, CSAMPLE value, int length) {
        SampleUtil::fill(pBuffer, value, length);
    }

    void AssertWholeBufferEquals(const CSAMPLE* pBuffer, CSAMPLE value, int iBufferLen) {
        for (int i = 0; i < iBufferLen; ++i) {
            EXPECT_FLOAT_EQ(value, pBuffer[i]);
        }
    }

    void AssertBufferCycles(const CSAMPLE* pBuffer, int iBufferLen,
                            CSAMPLE* pCycleBuffer, int iCycleLength) {
        int cycleRead = 0;
        for (int i = 0; i < iBufferLen; ++i) {
            //qDebug() << "i" << i << pBuffer[i] << pCycleBuffer[cycleRead % iCycleLength];
            EXPECT_FLOAT_EQ(pCycleBuffer[cycleRead++ % iCycleLength], pBuffer[i]);
        }
    }

    void AssertBufferMonotonicallyProgresses(const CSAMPLE* pBuffer,
                                             CSAMPLE start, CSAMPLE finish,
                                             int iBufferLen) {
        CSAMPLE currentLimit = start;
        bool increasing = (finish - start) > 0;

        for (int i = 0; i < iBufferLen; ++i) {
            if (increasing) {
                //qDebug() << "i" << i << pBuffer[i] << currentLimit;
                EXPECT_GE(pBuffer[i], currentLimit);
                currentLimit = pBuffer[i];
            } else {
                EXPECT_LE(pBuffer[i], currentLimit);
                currentLimit = pBuffer[i];
            }
        }
    }

    StrictMock<ReadAheadManagerMock>* m_pReadAheadMock;
    EngineBufferScaleLinear* m_pScaler;
};

TEST_F(EngineBufferScaleLinearTest, ScaleConstant) {
    SetRateNoLerp(1.0);

    CSAMPLE readBuffer[1] = { 1.0f };
    m_pReadAheadMock->setReadBuffer(readBuffer, 1);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);
    // TODO(rryan) the LERP w/ the previous buffer causes samples 0 and 1 to be
    // 0, for now skip the first two.
    AssertWholeBufferEquals(pOutput+2, 1.0f, kiLinearScaleReadAheadLength - 2);

    // Check that the total samples read from the RAMAN is equal to the samples
    // we requested.
    ASSERT_EQ(kiLinearScaleReadAheadLength, m_pReadAheadMock->getSamplesRead());

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, UnityRateIsSamplePerfect) {
    SetRateNoLerp(1.0);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    QVector<CSAMPLE> readBuffer;
    for (int i = 0; i < 1000; ++i) {
        readBuffer.push_back(i);
    }
    m_pReadAheadMock->setReadBuffer(readBuffer.data(), readBuffer.size());

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);

    AssertBufferCycles(pOutput, kiLinearScaleReadAheadLength,
                       readBuffer.data(), readBuffer.size());

    // Check that the total samples read from the RAMAN is equal to the samples
    // we requested.
    ASSERT_EQ(kiLinearScaleReadAheadLength, m_pReadAheadMock->getSamplesRead());

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, TestRateLERPMonotonicallyProgresses) {
    // Starting from a rate of 0.0, we'll go to a rate of 1.0
    SetRate(0.0);
    SetRate(1.0);

    // Read all 1's
    CSAMPLE readBuffer[] = { 1.0f };
    m_pReadAheadMock->setReadBuffer(readBuffer, 1);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);

    AssertBufferMonotonicallyProgresses(pOutput, 0.0f, 1.0f, kiLinearScaleReadAheadLength);

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, TestDoubleSpeedSmoothlyHalvesSamples) {
    SetRateNoLerp(2.0);

    // To prove that the channels don't touch each other, we're using negative
    // values on the first channel and positive values on the second channel. If
    // a fraction of either channel were mixed into either, then we would see a
    // big shift in our desired values.
    CSAMPLE readBuffer[] = { 1.0, 1.0,
                             0.0, 0.0,
                             -1.0, -1.0,
                             0.0, 0.0 };
    m_pReadAheadMock->setReadBuffer(readBuffer, 8);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);

    CSAMPLE expectedResult[] = { 1.0, 1.0,
                                 -1.0, -1.0 };
    AssertBufferCycles(pOutput, kiLinearScaleReadAheadLength, expectedResult, 4);

    // Check that the total samples read from the RAMAN is double the samples
    // we requested.
    ASSERT_EQ(kiLinearScaleReadAheadLength * 2, m_pReadAheadMock->getSamplesRead());

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, TestHalfSpeedSmoothlyDoublesSamples) {
    SetRateNoLerp(0.5);

    // To prove that the channels don't touch each other, we're using negative
    // values on the first channel and positive values on the second channel. If
    // a fraction of either channel were mixed into either, then we would see a
    // big shift in our desired values.
    CSAMPLE readBuffer[] = { -101.0, 101.0,
                             -99.0, 99.0 };
    m_pReadAheadMock->setReadBuffer(readBuffer, 4);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);

    CSAMPLE expectedResult[] = { -101.0, 101.0,
                                 -100.0, 100.0,
                                 -99.0, 99.0,
                                 -100.0, 100.0 };
    AssertBufferCycles(pOutput, kiLinearScaleReadAheadLength, expectedResult, 8);

    // Check that the total samples read from the RAMAN is half the samples we
    // requested. TODO(XXX) the extra +2 in this seems very suspicious. We need
    // to find out why this happens.
    ASSERT_EQ(kiLinearScaleReadAheadLength / 2 + 2, m_pReadAheadMock->getSamplesRead());

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, TestRepeatedScaleCalls) {
    SetRateNoLerp(0.5);

    // To prove that the channels don't touch each other, we're using negative
    // values on the first channel and positive values on the second channel. If
    // a fraction of either channel were mixed into either, then we would see a
    // big shift in our desired values.
    CSAMPLE readBuffer[] = { -101.0, 101.0,
                             -99.0, 99.0 };
    m_pReadAheadMock->setReadBuffer(readBuffer, 4);

    // Tell the RAMAN mock to invoke getNextSamplesFake
    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillRepeatedly(Invoke(m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesFake));

    CSAMPLE expectedResult[] = { -101.0, 101.0,
                                 -100.0, 100.0,
                                 -99.0, 99.0,
                                 -100.0, 100.0 };

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);

    int samplesRemaining = kiLinearScaleReadAheadLength;
    while (samplesRemaining > 0) {
        int toRead = math_min(8, samplesRemaining);
        m_pScaler->scaleBuffer(pOutput, 8);
        samplesRemaining -= toRead;
        AssertBufferCycles(pOutput, toRead, expectedResult, toRead);
    }

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, RepeatedZeroRefillsAreBounded) {
    SetRateNoLerp(2.0);

    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .Times(2)
            .WillRepeatedly(Return(0));

    CSAMPLE* pOutput = SampleUtil::alloc(kiLinearScaleReadAheadLength);
    FillBuffer(pOutput, 1.0f, kiLinearScaleReadAheadLength);

    const double framesRead =
            m_pScaler->scaleBuffer(pOutput, kiLinearScaleReadAheadLength);

    EXPECT_GT(framesRead, 0.0);
    AssertWholeBufferEquals(pOutput, 0.0f, kiLinearScaleReadAheadLength);

    SampleUtil::free(pOutput);
}

TEST_F(EngineBufferScaleLinearTest, ZeroProgressRefillPreservesRebasedPosition) {
    constexpr SINT kInternalBufferFrames = 2;
    constexpr SINT kOutputFrames = 4;
    constexpr SINT kOutputSamples = kOutputFrames * 2;
    constexpr double kInitialNextFrame = 1.5;
    constexpr double kExpectedCurrentFrame = -0.5;

    SetRateNoLerp(2.0);

    // Start with a partial internal buffer and a fractional position that
    // needs the next frame from a refill. Two zero reads model the bounded
    // no-progress result returned when read-ahead capacity is full.
    m_pScaler->m_bufferIntSize = kInternalBufferFrames * 2;
    m_pScaler->m_dNextFrame = kInitialNextFrame;
    SampleUtil::fill(m_pScaler->m_bufferInt,
            1.0f,
            m_pScaler->m_bufferIntSize);

    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .Times(4)
            .WillRepeatedly(Return(0));

    for (const double rate : {2.0, -2.0}) {
        if (rate < 0) {
            SetRateNoLerp(rate);
            m_pScaler->m_bufferIntSize = kInternalBufferFrames * 2;
            m_pScaler->m_dNextFrame = kInitialNextFrame;
            SampleUtil::fill(m_pScaler->m_bufferInt,
                    1.0f,
                    m_pScaler->m_bufferIntSize);
        }

        CSAMPLE output[kOutputSamples];
        FillBuffer(output, 1.0f, kOutputSamples);

        const double framesRead = m_pScaler->scaleBuffer(
                output, kOutputSamples);

        EXPECT_DOUBLE_EQ(kOutputFrames * 2.0, framesRead);
        EXPECT_DOUBLE_EQ(kExpectedCurrentFrame, m_pScaler->m_dCurrentFrame);
        EXPECT_DOUBLE_EQ(0.0, m_pScaler->m_dNextFrame);
        EXPECT_EQ(0, m_pScaler->m_bufferIntSize);
        EXPECT_EQ(0, m_pReadAheadMock->getSamplesRead());
        AssertWholeBufferEquals(output, 0.0f, kOutputSamples);
    }
}

TEST_F(EngineBufferScaleLinearTest, EmptyRefillNormalizesPartialReadRecovery) {
    constexpr SINT kFallbackFrames = 1024;
    constexpr SINT kFallbackSamples = kFallbackFrames * 2;
    constexpr SINT kRecoveryFrames = 4;
    constexpr SINT kRecoverySamples = kRecoveryFrames * 2;
    constexpr CSAMPLE kStaleSample = 1234.0;

    SetRateNoLerp(1.25);
    // Distinct frames make skipped or duplicated recovery data observable.
    CSAMPLE readBuffer[] = {41.0, -41.0, 43.0, -43.0, 47.0, -47.0, 53.0, -53.0, 59.0, -59.0};
    m_pReadAheadMock->setReadBuffer(readBuffer, 10);
    m_pScaler->m_bufferIntSize = 4;
    m_pScaler->m_dNextFrame = 1.5;
    SampleUtil::fill(m_pScaler->m_bufferInt,
            99.0f,
            m_pScaler->m_bufferIntSize);

    EXPECT_CALL(*m_pReadAheadMock, getNextSamples(_, _, _, _))
            .WillOnce(Return(0))
            .WillOnce(Return(0))
            .WillRepeatedly(Invoke(
                    m_pReadAheadMock, &ReadAheadManagerMock::getNextSamplesOneFrame));

    CSAMPLE* pFallbackOutput = SampleUtil::alloc(kFallbackSamples);
    FillBuffer(pFallbackOutput, kStaleSample, kFallbackSamples);
    const double fallbackFrames =
            m_pScaler->scaleBuffer(pFallbackOutput, kFallbackSamples);

    EXPECT_DOUBLE_EQ(kFallbackFrames * 1.25, fallbackFrames);
    AssertWholeBufferEquals(pFallbackOutput, 0.0f, kFallbackSamples);
    EXPECT_DOUBLE_EQ(-0.5, m_pScaler->m_dCurrentFrame);
    EXPECT_DOUBLE_EQ(0.0, m_pScaler->m_dNextFrame);
    EXPECT_EQ(0, m_pScaler->m_bufferIntSize);
    SampleUtil::free(pFallbackOutput);

    CSAMPLE recoveryOutput[kRecoverySamples];
    FillBuffer(recoveryOutput, kStaleSample, kRecoverySamples);
    const double recoveryFrames =
            m_pScaler->scaleBuffer(recoveryOutput, kRecoverySamples);

    // A partial read is fresh source data, not an empty-buffer retry. The
    // normalized coordinate lets this four-frame recovery complete after the
    // five one-frame reads needed to establish interpolation provenance.
    EXPECT_DOUBLE_EQ(kRecoveryFrames * 1.25, recoveryFrames);
    EXPECT_EQ(kRecoveryFrames + 1, m_pReadAheadMock->getOneFrameReadCalls());
    EXPECT_EQ((kRecoveryFrames + 1) * 2, m_pReadAheadMock->getSamplesRead());
    EXPECT_FLOAT_EQ(0.0f, recoveryOutput[0]);
    EXPECT_FLOAT_EQ(0.0f, recoveryOutput[1]);
    EXPECT_FLOAT_EQ(11.75f, recoveryOutput[2]);
    EXPECT_FLOAT_EQ(-11.75f, recoveryOutput[3]);
    EXPECT_FLOAT_EQ(26.5f, recoveryOutput[4]);
    EXPECT_FLOAT_EQ(-26.5f, recoveryOutput[5]);
    EXPECT_FLOAT_EQ(44.25f, recoveryOutput[6]);
    EXPECT_FLOAT_EQ(-44.25f, recoveryOutput[7]);
    for (const CSAMPLE sample : recoveryOutput) {
        EXPECT_NE(kStaleSample, sample);
    }
}
