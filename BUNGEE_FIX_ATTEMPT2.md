# Bungee Playback Speed Fix - Attempt 2

## Problem
8.18x speedup regardless of playback speed setting.

## Root Cause Analysis

Based on analysis of Bungee library code:

From CommandLine.h line 349:
```cpp
size_t outputFrameCount = std::floor(inputFrameCount / std::fabs(request.speed) * sampleRates.output / sampleRates.input);
```

With sampleRates.input = sampleRates.output (both set to the same sample rate):
```cpp
outputFrameCount = inputFrameCount / speed
```

This means:
- speed = 1.0: outputFrameCount = inputFrameCount (normal playback)
- speed = 2.0: outputFrameCount = inputFrameCount / 2 (2x faster playback)
- speed = 0.5: outputFrameCount = inputFrameCount / 0.5 = 2 * inputFrameCount (2x slower playback)

## The Fix

Bungee's `speed` parameter represents the **time stretch ratio**, which is the **inverse** of the playback rate.

### Key Insight

Mixxx's tempo ratio (e.g., 2.0 for 2x faster) needs to be inverted when passed to Bungee:
- Mixxx tempo = 2.0 (2x faster) → Bungee speed = 0.5 (compress time by 0.5x)
- Mixxx tempo = 0.5 (half speed) → Bungee speed = 2.0 (stretch time by 2x)
- Mixxx tempo = 1.0 (normal) → Bungee speed = 1.0 (no change)

### Changes Made

**Line 151 (setScaleParameters)**:
```cpp
// Before:
m_request.speed = m_dBaseRate * m_dTempoRatio;

// After:
m_request.speed = 1.0 / (m_dBaseRate * m_dTempoRatio);
```

**Lines 223-230 (processGrain)**:
```cpp
// Calculate Bungee's speed parameter (time stretch ratio = inverse of playback rate)
double speed = 1.0 / (m_dBaseRate * m_dTempoRatio);
if (m_bBackwards) {
    speed = -speed;
}

// Calculate effective rate for ReadAheadManager (includes base_rate for sample rate conversion)
const double effectiveRate = (m_bBackwards ? -1.0 : 1.0) * m_dBaseRate * m_dTempoRatio;
```

**Lines 262-266 (processGrain)**:
```cpp
// ReadAheadManager gets effective rate, not Bungee's speed
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        effectiveRate,
        m_interleavedReadBuffer.data(),
        samplesNeeded,
        getOutputSignal().getChannelCount());
```

## Why This Fixes the 8.18x Speedup

With original code (speed = m_dBaseRate * m_dTempoRatio):
- At normal playback (m_dBaseRate=1.0, m_dTempoRatio=1.0): speed = 1.0 → output = input / 1.0 = input ✓
- At 2x playback (m_dBaseRate=1.0, m_dTempoRatio=2.0): speed = 2.0 → output = input / 2.0 = 0.5 * input ✓

Wait, this should work correctly...

**Actually**, if there's an 8.18x speedup, maybe the issue is that Bungee's speed should NOT include base_rate, AND should be inverted:

Let me reconsider...
