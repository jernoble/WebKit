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

#if ENABLE(MEDIA_SOURCE)

#include "ActiveDOMObject.h"
#include "EventTarget.h"
#include "ExceptionOr.h"
#include "WebCoreOpaqueRoot.h"
#include <wtf/LoggerHelper.h>
#include <wtf/Observer.h>

namespace WTF {
class MediaTime;
}

namespace WebCore {

class AudioTrack;
class AudioTrackList;
class BufferSource;
class MediaSource;
class SourceBufferImpl;
class SourceBufferPrivate;
class TextTrack;
class TextTrackList;
class TimeRanges;
class VideoTrack;
class VideoTrackList;

class SourceBuffer final
    : public ThreadSafeRefCounted<SourceBuffer>
    , public ActiveDOMObject
    , public EventTarget
{
    WTF_MAKE_ISO_ALLOCATED(SourceBuffer);
public:
    static Ref<SourceBuffer> create(Ref<SourceBufferPrivate>&&, MediaSource&);
    virtual ~SourceBuffer();

    enum class AppendMode { Segments, Sequence };
    AppendMode mode() const;
    ExceptionOr<void> setMode(AppendMode);

    bool updating() const;
    ExceptionOr<Ref<TimeRanges>> buffered() const;
    double timestampOffset() const;
    ExceptionOr<void> setTimestampOffset(double);
    bool hasVideo() const;
    void abortIfUpdating();
    void removedFromMediaSource();

    VideoTrackList& videoTracks();
    VideoTrackList* videoTracksIfExists() const;
    AudioTrackList& audioTracks();
    AudioTrackList* audioTracksIfExists() const;
    TextTrackList& textTracks();
    TextTrackList* textTracksIfExists() const;

    void addAudioTrack(Ref<AudioTrack>&&);
    void addVideoTrack(Ref<VideoTrack>&&);
    void addTextTrack(Ref<TextTrack>&&);

    double appendWindowStart() const;
    ExceptionOr<void> setAppendWindowStart(double);
    double appendWindowEnd() const;
    ExceptionOr<void> setAppendWindowEnd(double);

    ExceptionOr<void> appendBuffer(const BufferSource&);
    ExceptionOr<void> abort();
    ExceptionOr<void> remove(double start, double end);
    ExceptionOr<void> remove(const WTF::MediaTime&, const WTF::MediaTime&);
    ExceptionOr<void> changeType(const String&);

    using ThreadSafeRefCounted::ref;
    using ThreadSafeRefCounted::deref;

    MediaSource* mediaSource() const { return m_source.get(); }
    const SourceBufferImpl& impl() const { return m_impl.get(); }
    SourceBufferImpl& impl() { return m_impl.get(); }

    void reportExtraMemoryAllocated(uint64_t extraMemory);

    WEBCORE_EXPORT void setShouldGenerateTimestamps(bool flag);

    size_t memoryCost() const;

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const { return m_logger.get(); }
    const void* logIdentifier() const { return m_logIdentifier; }
    const char* logClassName() const { return "SourceBuffer"; }
    WTFLogChannel& logChannel() const;
#endif

    // EventTarget
    ScriptExecutionContext* scriptExecutionContext() const final { return ActiveDOMObject::scriptExecutionContext(); }
    EventTargetInterface eventTargetInterface() const final { return SourceBufferEventTargetInterfaceType; }

    WebCoreOpaqueRoot opaqueRoot();

private:
    SourceBuffer(Ref<SourceBufferPrivate>&&, MediaSource&);

    void refEventTarget() final { ref(); }
    void derefEventTarget() final { deref(); }

    // ActiveDOMObject.
    void stop() final;
    const char* activeDOMObjectName() const final;
    bool virtualHasPendingActivity() const final;

    bool isRemoved() const;

    RefPtr<MediaSource> m_source;
    UniqueRef<SourceBufferImpl> m_impl;
    WTF::Observer<WebCoreOpaqueRoot()> m_opaqueRootProvider;

    // Can only grow.
    uint64_t m_reportedExtraMemoryCost { 0 };
    // Can grow and shrink.
    uint64_t m_extraMemoryCost { 0 };

#if !RELEASE_LOG_DISABLED
    Ref<const Logger> m_logger;
    const void* m_logIdentifier;
#endif
};

}

#endif
