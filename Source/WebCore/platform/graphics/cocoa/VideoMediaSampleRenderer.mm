/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "VideoMediaSampleRenderer.h"

#import "MediaSample.h"
#import "WebCoreDecompressionSession.h"
#import "WebSampleBufferVideoRendering.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CMFormatDescription.h>
#import <pal/avfoundation/MediaTimeAVFoundation.h>
#import <pal/spi/cocoa/AVFoundationSPI.h>

#pragma mark - Soft Linking

#import "VideoToolboxSoftLink.h"
#import <pal/cf/CoreMediaSoftLink.h>
#import <pal/cocoa/AVFoundationSoftLink.h>

@interface AVSampleBufferDisplayLayer (WebCoreAVSampleBufferDisplayLayerQueueManagementPrivate)
- (void)expectMinimumUpcomingSampleBufferPresentationTime:(CMTime)minimumUpcomingPresentationTime;
- (void)resetUpcomingSampleBufferPresentationTimeExpectations;
@end

namespace WebCore {

VideoMediaSampleRenderer::VideoMediaSampleRenderer(WebSampleBufferVideoRendering *renderer)
    : m_workQueue(WorkQueue::create("VideoMediaSampleRenderer Queue"_s))
    , m_renderer(renderer)
{
}

VideoMediaSampleRenderer::~VideoMediaSampleRenderer()
{
    m_workQueue->dispatchSync([&] {
        setTimebase(nullptr);

        [m_renderer flush];
        [m_renderer stopRequestingMediaData];
        m_renderer = nil;

        if (m_decompressionSession) {
            m_decompressionSession->invalidate();
            m_decompressionSession = nullptr;
        }

        m_hasDecompressionSession = false;
    });
}

bool VideoMediaSampleRenderer::isReadyForMoreMediaData() const
{
    bool ready = false;
    m_workQueue->dispatchSync([&] {
        ready = (!m_decompressionSession || m_decompressionSession->isReadyForMoreMediaData()) && [m_renderer isReadyForMoreMediaData];
    });
    return ready;
}

void VideoMediaSampleRenderer::stopRequestingMediaData()
{
    m_workQueue->dispatch([weakThis = ThreadSafeWeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis.get())
            [protectedThis->m_renderer stopRequestingMediaData];
    });
}

void VideoMediaSampleRenderer::setPrefersDecompressionSession(bool prefers)
{
    m_workQueue->dispatch([weakThis = ThreadSafeWeakPtr { *this }, prefers = prefers] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->setPrefersDecompressionSessionInternal(prefers);
    });
}

void VideoMediaSampleRenderer::setPrefersDecompressionSessionInternal(bool prefers)
{
    if (m_prefersDecompressionSession == prefers)
        return;

    m_prefersDecompressionSession = prefers;
    if (!m_prefersDecompressionSession && m_decompressionSession) {
        m_decompressionSession->invalidate();
        m_decompressionSession = nullptr;
    }
}

void VideoMediaSampleRenderer::setTimebase(RetainPtr<CMTimebaseRef>&& timebase)
{
    m_workQueue->dispatch([weakThis = ThreadSafeWeakPtr { *this }, timebase = WTFMove(timebase)] () mutable {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->setTimebaseInternal(WTFMove(timebase));
    });
}

void VideoMediaSampleRenderer::setTimebaseInternal(RetainPtr<CMTimebaseRef>&& timebase)
{
    if (m_timebase) {
        PAL::CMTimebaseRemoveTimerDispatchSource(m_timebase.get(), m_timerSource.get());
        dispatch_source_cancel(m_timerSource.get());
        m_timerSource = nullptr;

        // cancel any timers
    }

    m_timebase = WTFMove(timebase);
    m_hasTimebase = !!m_timebase;

    if (m_timebase) {
        m_timerSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, m_workQueue->dispatchQueue()));
        dispatch_source_set_event_handler(m_timerSource.get(), [this] {
            purgeDecodedSampleQueue();
        });
        dispatch_activate(m_timerSource.get());
        PAL::CMTimebaseAddTimerDispatchSource(m_timebase.get(), m_timerSource.get());
    }
}

void VideoMediaSampleRenderer::enqueueSample(const MediaSample& sample)
{
    ASSERT(sample.platformSampleType() == PlatformSample::Type::CMSampleBufferType);
    if (sample.platformSampleType() != PlatformSample::Type::CMSampleBufferType)
        return;

    auto cmSampleBuffer = sample.platformSample().sample.cmSampleBuffer;

#if PLATFORM(IOS_FAMILY)
    if (!m_hasDecompressionSession && !m_currentCodec) {
        // Only use a decompression session for vp8 or vp9 when software decoded.
        CMVideoFormatDescriptionRef videoFormatDescription = PAL::CMSampleBufferGetFormatDescription(cmSampleBuffer);
        auto fourCC = PAL::CMFormatDescriptionGetMediaSubType(videoFormatDescription);
        if (fourCC == 'vp08' || (fourCC == 'vp09' && !(canLoad_VideoToolbox_VTIsHardwareDecodeSupported() && VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9))))
            initializeDecompressionSession();
        m_currentCodec = fourCC;
    }
#endif

    if (!m_hasDecompressionSession && prefersDecompressionSession() && !sample.isProtected())
        initializeDecompressionSession();

    if (!m_decompressionSession) {
        [m_renderer enqueueSampleBuffer:cmSampleBuffer];
        return;
    }

    bool displaying = !sample.isNonDisplaying();
    auto decodePromise = m_decompressionSession->decodeSample(cmSampleBuffer, displaying);
    decodePromise->whenSettled(m_workQueue, [weakThis = ThreadSafeWeakPtr { *this }] (auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        if (!result) {
            ensureOnMainThread([protectedThis = WTFMove(protectedThis), status = result.error()] {
                assertIsMainThread();

                // Simulate AVSBDL decoding error.
                RetainPtr error = [NSError errorWithDomain:@"com.apple.WebKit" code:status userInfo:nil];
                NSDictionary *userInfoDict = @{ (__bridge NSString *)AVSampleBufferDisplayLayerFailedToDecodeNotificationErrorKey: (__bridge NSError *)error.get() };
                [NSNotificationCenter.defaultCenter postNotificationName:AVSampleBufferDisplayLayerFailedToDecodeNotification object:protectedThis->m_renderer.get() userInfo:userInfoDict];
                [NSNotificationCenter.defaultCenter postNotificationName:AVSampleBufferVideoRendererDidFailToDecodeNotification object:protectedThis->m_renderer.get() userInfo:userInfoDict];
            });
            return;
        }

        if (!*result)
            return;

        RetainPtr<CMSampleBufferRef> retainedResult = *result;
        protectedThis->decodedFrameAvailable(WTFMove(retainedResult));
    });
}

void VideoMediaSampleRenderer::initializeDecompressionSession()
{
    m_workQueue->dispatchSync([&] {
        if (m_decompressionSession)
            m_decompressionSession->invalidate();

        m_decompressionSession = WebCoreDecompressionSession::createOpenGL();
        m_decompressionSession->setTimebase([m_renderer timebase]);
        m_decompressionSession->setResourceOwner(m_resourceOwner);

        m_hasDecompressionSession = true;
        resetReadyForMoreSample();
    });
}

void VideoMediaSampleRenderer::decodedFrameAvailable(RetainPtr<CMSampleBufferRef>&& sample)
{
    assertIsCurrent(m_workQueue);
    if (m_renderer)
        [m_renderer enqueueSampleBuffer:sample.get()];

    if (!m_timebase)
        return;

    purgeDecodedSampleQueue();
    PAL::CMBufferQueueEnqueue(ensureDecodedSampleQueue(), sample.get());
}

void VideoMediaSampleRenderer::purgeDecodedSampleQueue()
{
    assertIsCurrent(m_workQueue);
    if (!m_decodedSampleQueue)
        return;

    if (!m_timebase)
        return;

    CMTime currentTime = PAL::CMTimebaseGetTime(m_timebase.get());
    CMTime nextPurgeTime = PAL::kCMTimeInvalid;

    while (auto nextSample = (CMSampleBufferRef)const_cast<void*>(PAL::CMBufferQueueGetHead(m_decodedSampleQueue.get()))) {
        CMTime presentationTime = PAL::CMSampleBufferGetOutputPresentationTimeStamp(nextSample);
        CMTime duration = PAL::CMSampleBufferGetOutputDuration(nextSample);
        CMTime presentationEndTime = PAL::CMTimeAdd(presentationTime, duration);
        if (PAL::CMTimeCompare(presentationEndTime, currentTime) >= 0) {
            nextPurgeTime = presentationEndTime;
            break;
        }

        RetainPtr sampleToBePurged = adoptCF(PAL::CMBufferQueueDequeueAndRetain(m_decodedSampleQueue.get()));
        sampleToBePurged = nil;
    }

    if (!CMTIME_IS_VALID(nextPurgeTime))
        return;

    PAL::CMTimebaseSetTimerDispatchSourceNextFireTime(m_timebase.get(), m_timerSource.get(), nextPurgeTime, 0);
}

CMBufferQueueRef VideoMediaSampleRenderer::ensureDecodedSampleQueue()
{
    assertIsCurrent(m_workQueue);
    if (!m_decodedSampleQueue)
        m_decodedSampleQueue = WebCoreDecompressionSession::createBufferQueue();
    return m_decodedSampleQueue.get();
}

void VideoMediaSampleRenderer::flush()
{
    m_workQueue->dispatchSync([&] {
        [m_renderer flush];
        if (m_decompressionSession)
            m_decompressionSession->flush();
    });
}

void VideoMediaSampleRenderer::requestMediaDataWhenReady(Function<void()>&& function)
{
    m_readyForMoreSampleFunction = WTFMove(function);
    resetReadyForMoreSample();
}

void VideoMediaSampleRenderer::resetReadyForMoreSample()
{
    if (m_renderer) {
        ThreadSafeWeakPtr weakThis { *this };
        [m_renderer requestMediaDataWhenReadyOnQueue:dispatch_get_main_queue() usingBlock:^{
            if (RefPtr protectedThis = weakThis.get(); protectedThis && protectedThis->m_readyForMoreSampleFunction)
                protectedThis->m_readyForMoreSampleFunction();
        }];
        return;
    }

    if (!m_hasDecompressionSession)
        return;

    if (!m_hasTimebase)
        return;
}

void VideoMediaSampleRenderer::expectMinimumUpcomingSampleBufferPresentationTime(const MediaTime& time)
{
    if (![PAL::getAVSampleBufferDisplayLayerClass() instancesRespondToSelector:@selector(expectMinimumUpcomingSampleBufferPresentationTime:)])
        return;
    [m_renderer expectMinimumUpcomingSampleBufferPresentationTime:PAL::toCMTime(time)];
}

void VideoMediaSampleRenderer::resetUpcomingSampleBufferPresentationTimeExpectations()
{
    if (![PAL::getAVSampleBufferDisplayLayerClass() instancesRespondToSelector:@selector(resetUpcomingSampleBufferPresentationTimeExpectations)])
        return;
    [m_renderer resetUpcomingSampleBufferPresentationTimeExpectations];
    m_minimumUpcomingPresentationTime.reset();
}

AVSampleBufferDisplayLayer *VideoMediaSampleRenderer::displayLayer() const
{
    return dynamic_objc_cast<AVSampleBufferDisplayLayer>(m_renderer.get(), PAL::getAVSampleBufferDisplayLayerClass());
}

RetainPtr<CVPixelBufferRef> VideoMediaSampleRenderer::copyDisplayedPixelBuffer()
{
    RetainPtr<CVPixelBufferRef> imageBuffer;
    m_workQueue->dispatchSync([&] {
#if HAVE(AVSAMPLEBUFFERDISPLAYLAYER_COPYDISPLAYEDPIXELBUFFER)
        if (auto buffer = adoptCF([m_renderer copyDisplayedPixelBuffer])) {
            imageBuffer = buffer;
            return;
        }
#endif

        if (!m_timebase)
            return;

        purgeDecodedSampleQueue();

        CMTime currentTime = PAL::CMTimebaseGetTime(m_timebase.get());
        auto nextSample = (CMSampleBufferRef)const_cast<void*>(PAL::CMBufferQueueGetHead(m_decodedSampleQueue.get()));
        CMTime presentationTime = PAL::CMSampleBufferGetOutputPresentationTimeStamp(nextSample);

        if (PAL::CMTimeCompare(presentationTime, currentTime) > 0)
            return;

        RetainPtr sampleToBePurged = adoptCF((CMSampleBufferRef)const_cast<void*>(PAL::CMBufferQueueDequeueAndRetain(m_decodedSampleQueue.get())));
        imageBuffer = (CVPixelBufferRef)PAL::CMSampleBufferGetImageBuffer(sampleToBePurged.get());
    });

    if (!imageBuffer)
        return nullptr;

    ASSERT(CFGetTypeID(imageBuffer.get()) == CVPixelBufferGetTypeID());
    if (CFGetTypeID(imageBuffer.get()) != CVPixelBufferGetTypeID())
        return nullptr;
    return imageBuffer;
}

CGRect VideoMediaSampleRenderer::bounds() const
{
    return [displayLayer() bounds];
}

unsigned VideoMediaSampleRenderer::totalVideoFrames() const
{
    if (m_decompressionSession)
        return m_decompressionSession->totalVideoFrames();
    return [m_renderer videoPerformanceMetrics].totalNumberOfVideoFrames;
}

unsigned VideoMediaSampleRenderer::droppedVideoFrames() const
{
    if (m_decompressionSession)
        return m_decompressionSession->droppedVideoFrames();
    return [m_renderer videoPerformanceMetrics].numberOfDroppedVideoFrames;
}

unsigned VideoMediaSampleRenderer::corruptedVideoFrames() const
{
    if (m_decompressionSession)
        return m_decompressionSession->corruptedVideoFrames();
    return [m_renderer videoPerformanceMetrics].numberOfCorruptedVideoFrames;
}

MediaTime VideoMediaSampleRenderer::totalFrameDelay() const
{
    if (m_decompressionSession)
        return m_decompressionSession->totalFrameDelay();
    return MediaTime::createWithDouble([m_renderer videoPerformanceMetrics].totalFrameDelay);
}

void VideoMediaSampleRenderer::setResourceOwner(const ProcessIdentity& resourceOwner)
{
    m_workQueue->dispatchSync([&] {
        if (resourceOwner == m_resourceOwner)
            return;

        m_resourceOwner = resourceOwner;

        if (m_decompressionSession)
            m_decompressionSession->setResourceOwner(m_resourceOwner);
    });
}

} // namespace WebCore
