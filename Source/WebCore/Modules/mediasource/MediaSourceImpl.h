/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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

#pragma once

#include "config.h"
#include "MediaSourceImpl.h"

#if ENABLE(MEDIA_SOURCE)

#include "HTMLMediaElementEnums.h"
#include "MediaSource.h"
#include "MediaSourcePrivateClient.h"
#include "ScriptExecutionContext.h"
#include "SourceBufferList.h"
#include <wtf/IsoMalloc.h>
#include <wtf/WorkQueue.h>

namespace WebCore {

class ContentType;
class HTMLMediaElement;
class ScriptExecutionContext;
class SourceBuffer;
class SourceBufferImpl;
class TimeRanges;
struct FourCC;

class MediaSourceImpl : public MediaSourcePrivateClient {
    WTF_MAKE_ISO_ALLOCATED(MediaSourceImpl);
public:
    MediaSourceImpl(MediaSource&, ScriptExecutionContext&);
    virtual ~MediaSourceImpl();

    static bool isTypeSupported(const String& type,
                                bool needsVP9FullRangeQuirk,
                                const Vector<ContentType>& contentTypesRequiringHardwareSupport,
                                const std::optional<Vector<String>>& allowedMediaCodecTypes,
                                const std::optional<Vector<String>>& allowedMediaContainerTypes,
                                const std::optional<Vector<FourCC>>& allowedMediaVideoCodecIDs,
                                const std::optional<Vector<FourCC>>& allowedMediaAudioCodecIDs,
                                const std::optional<Vector<FourCC>>& allowedMediaCaptionFormatTypes);

    using ReadyState = MediaSource::ReadyState;
    using EndOfStreamError = MediaSource::EndOfStreamError;

    // These methods are thread-safe
    WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData> mediaElement() const;
    void setMediaElement(WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData>&&);
    MediaTime currentTime() const;
    MediaSource::ReadyState readyState() const;
    bool isOpen() const;
    bool isClosed() const;
    bool isEnded() const;
    SourceBufferList& sourceBuffers() const;
    SourceBufferList& activeSourceBuffers() const;
    void invalidateDuration();

    // These methods must be called on workQueue()
    ExceptionOr<void> setDuration(double duration);
    ExceptionOr<void> setDurationInternal(const MediaTime& duration);
    std::unique_ptr<PlatformTimeRanges> buffered() const;
    Ref<TimeRanges> seekable();
    ExceptionOr<void> setLiveSeekableRange(double start, double end);
    ExceptionOr<void> clearLiveSeekableRange();
    ExceptionOr<void> endOfStream(std::optional<EndOfStreamError> error);
    void streamEndedWithError(std::optional<EndOfStreamError> error);
    ExceptionOr<Ref<SourceBufferPrivate>> createSourceBufferPrivate(const String& type);

    void openIfInEndedState();

    bool attachToElement(HTMLMediaElement& element);
    void detachFromElement(HTMLMediaElement& element);

    void stop();

    void regenerateActiveSourceBuffers();
    void updateBufferedIfNeeded();

    void didReceiveInitializationSegment(bool activeTrackFlag);
    static bool contentTypeShouldGenerateTimestamps(const ContentType& contentType);

    // MediaSourcePrivateClient
    WorkQueue& workQueue() const final { return m_workQueue; }
    MediaTime duration() const final; // Thread-safe
#if USE(GSTREAMER)
    void monitorSourceBuffers() final;
#else
    void monitorSourceBuffers();
#endif

    void dispatchWorkQueueTask(Function<void()>&& task)
    {
        m_workQueue->dispatch(CancellableTask(m_taskGroup, WTFMove(task)));
    }

    template <typename C, typename R = std::invoke_result_t<C>>
    R dispatchWorkQueueTaskSync(C&& task)
    {
        std::optional<R> returnValue;
        m_workQueue->dispatchSync([&] () mutable {
            returnValue = task();
        });
        return returnValue.value();
    }

    void forEachSourceBuffer(Function<void(SourceBuffer&)>&& task);
    void forEachSourceBufferInternal(Function<void(SourceBufferImpl&)>&& task);
    void forEachActiveSourceBuffer(Function<void(SourceBuffer&)>&& task);
    bool anySourceBuffer(Function<bool(SourceBuffer&)>&& task);
    bool everyActiveSourceBuffer(Function<bool(SourceBuffer&)>&& task);

    void dispatchSourceBufferTaskSync(Function<void()>&& task);
    void dispatchMediaElementTask(Function<void(HTMLMediaElement&)>&& task);

private:
    void setBuffered(UniqueRef<PlatformTimeRanges>&& buffered);
    void completeSeek();
    static const MediaTime& currentTimeFudgeFactor();
    bool hasBufferedTime(const MediaTime& time);
    bool hasFutureTimeAfterCurrentTime(const MediaTime& currentTime);
    void monitorSourceBuffersWithCurrentTimeAndReadyState(const MediaTime& currentTime, HTMLMediaElementEnums::ReadyState);
    void setReadyState(ReadyState);
    void onReadyStateChange(ReadyState oldState, ReadyState newState);
    Vector<PlatformTimeRanges> activeRanges() const;
    void scheduleEvent(const AtomString& eventName);

    // MediaSourcePrivateClient
    void setPrivateAndOpen(Ref<MediaSourcePrivate>&&) final;
    void seekToTime(const MediaTime&) final;
#if !RELEASE_LOG_DISABLED
    void setLogIdentifier(const void*) final;
#endif
    void failedToCreateRenderer(RendererType) final;

    template <typename C,
        typename R = std::invoke_result_t<C>,
        typename std::enable_if_t<std::is_void_v<R>, bool> = true>
    void accessIvarSync(C&& callable)
    {
        m_ivarWorkQueue->dispatchSync([&] { callable(); });
    }

    template <typename C,
        typename R = std::invoke_result_t<C>,
        typename std::enable_if_t<std::is_void_v<R>, bool> = true>
    void accessIvarSync(C&& callable) const
    {
        m_ivarWorkQueue->dispatchSync([&] { callable(); });
    }

    template <typename C,
        typename R = std::invoke_result_t<C>,
        typename std::enable_if_t<!std::is_void_v<R>, bool> = true>
    R accessIvarSync(C&& callable)
    {
        R returnValue;
        m_ivarWorkQueue->dispatchSync([&] () mutable {
            returnValue = callable();
        });
        return returnValue;
    }

    template <typename C,
        typename R = std::invoke_result_t<C>,
        typename std::enable_if_t<!std::is_void_v<R>, bool> = true>
    R accessIvarSync(C&& callable) const
    {
        R returnValue;
        m_ivarWorkQueue->dispatchSync([&] () mutable {
            returnValue = callable();
        });
        return returnValue;
    }

    void dispatchContextTask(Function<void(ScriptExecutionContext&)>&&);

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const { return m_logger.get(); }
    const void* logIdentifier() const { return m_logIdentifier; }
    const char* logClassName() const { return "MediaSourceImpl"; }
    WTFLogChannel& logChannel() const;
#endif

    MediaSource& m_mediaSource;
    Ref<WorkQueue> m_workQueue;
    Ref<WorkQueue> m_sourceBufferWorkQueue;
    Ref<WorkQueue> m_ivarWorkQueue;
    TaskCancellationGroup m_taskGroup;
    RefPtr<MediaSourcePrivate> m_private;
    Ref<SourceBufferList> m_sourceBuffers;
    Ref<SourceBufferList> m_activeSourceBuffers;
    UniqueRef<PlatformTimeRanges> m_buffered;
    std::unique_ptr<PlatformTimeRanges> m_liveSeekable;
    WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData> m_mediaElement;
    MediaTime m_duration;
    MediaTime m_pendingSeekTime;
    ReadyState m_readyState { ReadyState::Closed };

    bool m_webmParserEnabled { false };
    bool m_needsVP9FullRangeFlagQuirk { false };
    Vector<ContentType> m_contentTypesRequiringHardwareSupport;
    std::optional<Vector<String>> m_allowedMediaCodecTypes;
    std::optional<Vector<String>> m_allowedMediaContainerTypes;
    std::optional<Vector<FourCC>> m_allowedMediaVideoCodecIDs;
    std::optional<Vector<FourCC>> m_allowedMediaAudioCodecIDs;
    std::optional<Vector<FourCC>> m_allowedMediaCaptionFormatTypes;

#if !RELEASE_LOG_DISABLED
    Ref<const Logger> m_logger;
    const void* m_logIdentifier { nullptr };
#endif
};

}

#endif
