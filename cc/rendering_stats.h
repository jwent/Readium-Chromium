// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CCRenderingStats_h
#define CCRenderingStats_h

namespace cc {

struct RenderingStats {
    // FIXME: Rename these to animationFrameCount and screenFrameCount, crbug.com/138641.
    int numAnimationFrames;
    int numFramesSentToScreen;
    int droppedFrameCount;
    double totalPaintTimeInSeconds;
    double totalRasterizeTimeInSeconds;
    double totalCommitTimeInSeconds;
    size_t totalCommitCount;
    size_t numImplThreadScrolls;
    size_t numMainThreadScrolls;

    RenderingStats()
        : numAnimationFrames(0)
        , numFramesSentToScreen(0)
        , droppedFrameCount(0)
        , totalPaintTimeInSeconds(0)
        , totalRasterizeTimeInSeconds(0)
        , totalCommitTimeInSeconds(0)
        , totalCommitCount(0)
        , numImplThreadScrolls(0)
        , numMainThreadScrolls(0)
    {
    }
};

}

#endif
