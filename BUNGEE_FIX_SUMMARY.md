# Bungee Playback Speed Fix - Final Solution

## Problem
Audio playing at approximately 8x speed regardless of playback speed setting.

## Root Cause
Bungee can determine speed from position deltas between grains (Bungee.h line 45):
```cpp
speed = position_delta / frames_consumed
```

### The Bug
When processing grains at 2x playback (tempo=2.0):
- Consume 100 input frames, produce 50 output frames
- Need: position_delta / 100 = 2.0
- So position_delta must be 200 (100 * 2.0)

**Bug was advancing position by 100 instead of 200**, causing Bungee to calculate speed = 1.0 instead of 2.0.

This incorrect speed calculation accumulated across grains, resulting in approximately 8x speedup.

## The Fix

### 1. Line 151 (setScaleParameters)
Use only tempo ratio for Bungee's speed parameter:
```cpp
m_request.speed = m_dTempoRatio;  // Not m_dBaseRate * m_dTempoRatio
```
Bungee handles sample rate conversion internally via `resampleMode`.

### 2. Lines 224-231 (processGrain)
Separate speed calculations:
```cpp
// For Bungee's request (time stretch ratio only)
double speed = m_dTempoRatio;
if (m_bBackwards) {
    speed = -speed;
}

// For ReadAheadManager (includes sample rate conversion)
const double effectiveRate = m_dBaseRate * m_dTempoRatio;
```

### 3. Lines 235-250 (processGrain) - KEY FIX
Fix position advancement for Bungee's speed calculation:
```cpp
// IMPORTANT: Bungee can determine speed from position deltas between grains.
// It calculates speed as: speed = position_delta / frames_consumed
// To get the correct tempo ratio, position must advance by: frames_consumed * tempo_ratio
if (!std::isnan(m_request.position)) {
    const SINT framesConsumed = m_currentInputChunk.end - m_currentInputChunk.begin;
    if (framesConsumed > 0) {
        // Position must advance by frames_consumed * tempo_ratio for Bungee
        // to calculate the correct speed from position deltas.
        const double positionDelta = static_cast<double>(framesConsumed) * m_dTempoRatio;
        m_grainPosition += (m_bBackwards ? -positionDelta : positionDelta);
    }
}
```

### 4. Line 264 (processGrain)
Pass effective rate to ReadAheadManager:
```cpp
const SINT availableSamples = m_pReadAheadManager->getNextSamples(
        effectiveRate,  // Not speed
        m_interleavedReadBuffer.data(),
        samplesNeeded,
        getOutputSignal().getChannelCount());
```

## Why This Fixes the Issue

### Before Fix
At 2x playback (tempo=2.0):
- Grain 1: consume 100 frames, advance position by 100
- Bungee calculates: speed = 100 / 100 = 1.0 (wrong! should be 2.0)
- Grain 2: consume 100 frames, advance position by 100
- Bungee calculates: speed = 100 / 100 = 1.0 (wrong! should be 2.0)
- Result: Bungee thinks speed is 1.0, plays at ~8x actual speed

### After Fix
At 2x playback (tempo=2.0):
- Grain 1: consume 100 frames, advance position by 200 (100 * 2.0)
- Bungee calculates: speed = 200 / 100 = 2.0 ✓
- Grain 2: consume 100 frames, advance position by 200
- Bungee calculates: speed = 200 / 100 = 2.0 ✓
- Result: Bungee correctly detects 2x speed, plays correctly

## Technical Details

1. **Bungee's speed parameter**: Represents input/output frame ratio (time stretch ratio)
   - speed = 1.0: outputFrameCount = inputFrameCount (normal playback)
   - speed = 2.0: outputFrameCount = inputFrameCount / 2 (2x faster)
   - Bungee handles sample rate conversion internally via `resampleMode`

2. **ReadAheadManager's rate parameter**: Needs effective playback rate (base_rate × tempo_ratio)
   - For proper position tracking in audio stream
   - Includes sample rate conversion

3. **Position tracking**: Critical for Bungee's speed calculation from deltas
   - Position must advance by: frames_consumed * tempo_ratio
   - This allows Bungee to calculate: speed = position_delta / frames_consumed = tempo_ratio ✓

This fix ensures Bungee correctly calculates speed from position deltas, preventing approximately 8x speedup issue.
