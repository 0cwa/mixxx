# Bungee Playback Speed Fix

## Problem
The Bungee buffer scaler was causing 8.18x speedup regardless of playback speed setting. A 3-minute song was playing in 22 seconds.

## Root Cause
The Bungee library's `speed` parameter represents the **input-to-output frame ratio**, not the effective playback rate. The code was incorrectly multiplying `base_rate` into this speed parameter.

### Bungee's Speed Semantics
From Bungee's library documentation (Stream.h line 144):
```cpp
request.speed = inputFrameCount / outputFrameCount;
```

This means:
- `speed = 1.0`: No time stretch (normal playback)
- `speed = 2.0`: Output is 2x faster than input (compress time)
- `speed = 0.5`: Output is 2x slower than input (stretch time)

Bungee handles sample rate conversion internally via the `resampleMode` parameter, so `base_rate` should NOT be mixed into the speed parameter.

## The Fix

### Changed Lines

**Line 148-151 (setScaleParameters)**:
```cpp
// Before:
m_request.speed = m_dBaseRate * m_dTempoRatio;

// After:
// Bungee's speed parameter is the input/output frame ratio.
// Use only the tempo ratio (playback speed) without base_rate.
// Bungee handles sample rate conversion internally via resampleMode.
m_request.speed = m_dTempoRatio;
```

**Lines 222-230 (processGrain)**:
```cpp
// Before:
double speed = m_dBaseRate * m_dTempoRatio;
if (m_bBackwards) {
    speed = -speed;
}

// After:
// For Bungee's request, use only the tempo ratio (input/output frame ratio)
double speed = m_dTempoRatio;
if (m_bBackwards) {
    speed = -speed;
}

// Calculate effective rate for ReadAheadManager (includes base_rate for sample rate conversion)
const double effectiveRate = m_dBaseRate * m_dTempoRatio;
```

**Lines 262-266 (processGrain)**:
```cpp
// Before:
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        speed,
        m_interleavedReadBuffer.data(),
        samplesNeeded,
        getOutputSignal().getChannelCount());

// After:
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        effectiveRate,
        m_interleavedReadBuffer.data(),
        samplesNeeded,
        getOutputSignal().getChannelCount());
```

**Lines 391-398 (scaleBuffer flush path)**:
```cpp
// Before:
const SINT samplesToRead = getOutputSignal().frames2samples(kMaxGrainFrames);
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        (m_bBackwards ? -1.0 : 1.0) * m_dBaseRate * m_dTempoRatio,
        m_interleavedReadBuffer.data(),
        samplesToRead,
        getOutputSignal().getChannelCount());

// After:
const SINT samplesToRead = getOutputSignal().frames2samples(kMaxGrainFrames);
const double effectiveRate = (m_bBackwards ? -1.0 : 1.0) * m_dBaseRate * m_dTempoRatio;
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        effectiveRate,
        m_interleavedReadBuffer.data(),
        samplesToRead,
        getOutputSignal().getChannelCount());
```

## Why This Fixes the Issue

1. **Bungee's speed parameter** now receives only the tempo ratio (playback speed), not including base_rate
2. **ReadAheadManager** receives the effective rate (base_rate * tempo_ratio) for proper position tracking
3. This ensures Bungee's time stretching calculations are correct while the ReadAheadManager still tracks the correct position

## Testing

Expected behavior after fix:
- 1.0x speed: Normal playback
- 0.5x speed: Half-speed playback with correct pitch
- 2.0x speed: 2x faster playback with correct pitch
- 0.1x speed: 10% speed playback with correct pitch (should not have super-deep pitch issue)

## Comparison with Other Scalers

This fix aligns with how RubberBand handles time ratios:
- RubberBand uses `setTimeRatio(1.0 / (base_rate * tempo_ratio))` - it calculates the inverse
- Bungee uses `speed = tempo_ratio` directly - it uses the ratio directly

The key difference is that RubberBand's `timeRatio` is stretched/unstretched duration ratio, while Bungee's `speed` is input/output frame ratio.
