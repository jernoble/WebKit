/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "ActiveDOMObject.h"
#include "EventTarget.h"
#include "ExceptionOr.h"
#include "URLRegistry.h"
#include <wtf/LoggerHelper.h>
#include <wtf/RefCounted.h>
#include <wtf/WeakPtr.h>

namespace WTF {
class MediaTime;
}

namespace WebCore {

class ContentType;
class HTMLMediaElement;
class MediaSourceImpl;
class MediaSourcePrivateClient;
class PlatformTimeRanges;
class SourceBuffer;
class SourceBufferList;
class SourceBufferPrivate;
class TimeRanges;

class MediaSource final
    : public RefCounted<MediaSource>
    , public ActiveDOMObject
    , public EventTarget
    , public URLRegistrable
#if !RELEASE_LOG_DISABLED
    , private LoggerHelper
#endif
{
    WTF_MAKE_ISO_ALLOCATED(MediaSource);
public:
    static void setRegistry(URLRegistry*);
    static MediaSource* lookup(const String& url) { return s_registry ? static_cast<MediaSource*>(s_registry->lookup(url)) : nullptr; }

    static Ref<MediaSource> create(ScriptExecutionContext&);
    virtual ~MediaSource();

    void addedToRegistry();
    void removedFromRegistry();
    void openIfInEndedState();
    bool isOpen() const;
    bool isClosed() const;
    bool isEnded() const;
    void sourceBufferDidChangeActiveState(SourceBuffer&, bool);
    void sourceBufferDidChangeBufferedDirty(SourceBuffer&, bool);
    MediaSourcePrivateClient& client();

    enum class EndOfStreamError { Network, Decode };
    ExceptionOr<void> endOfStream(std::optional<EndOfStreamError>);
    void streamEndedWithError(std::optional<EndOfStreamError>);

    WTF::MediaTime currentTime() const;
    WTF::MediaTime duration() const;
    std::unique_ptr<PlatformTimeRanges> buffered() const;

    bool attachToElement(HTMLMediaElement&);
    void detachFromElement(HTMLMediaElement&);
    void monitorSourceBuffers();
    Ref<TimeRanges> seekable();
    ExceptionOr<void> setLiveSeekableRange(double start, double end);
    ExceptionOr<void> clearLiveSeekableRange();

    ExceptionOr<void> setDuration(double);
    ExceptionOr<void> setDurationInternal(const WTF::MediaTime&);

    enum class ReadyState { Closed, Open, Ended };
    ReadyState readyState() const;

    WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData> mediaElement() const;
    SourceBufferList& sourceBuffers();
    SourceBufferList& activeSourceBuffers();
    ExceptionOr<Ref<SourceBuffer>> addSourceBuffer(const String& type);
    ExceptionOr<void> removeSourceBuffer(SourceBuffer&);
    static bool isTypeSupported(ScriptExecutionContext&, const String& type);

    ScriptExecutionContext* scriptExecutionContext() const final;

    using RefCounted::ref;
    using RefCounted::deref;

    static bool contentTypeShouldGenerateTimestamps(const ContentType&);

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger.get(); }
    const void* logIdentifier() const final { return m_logIdentifier; }
    const char* logClassName() const final { return "MediaSource"; }
    WTFLogChannel& logChannel() const final;
    void setLogIdentifier(const void*);
#endif

    void didReceiveInitializationSegment(bool activeTrackFlag);

    MediaSourceImpl& impl() { return m_impl; }
    const MediaSourceImpl& impl() const { return m_impl; }

private:
    explicit MediaSource(ScriptExecutionContext&);

    // ActiveDOMObject
    void stop() final;
    const char* activeDOMObjectName() const final;
    bool virtualHasPendingActivity() const final;

    // EventTarget
    void refEventTarget() final { ref(); }
    void derefEventTarget() final { deref(); }
    EventTargetInterface eventTargetInterface() const final;

    // URLRegistrable
    URLRegistry& registry() const final;

    static URLRegistry* s_registry;

#if !RELEASE_LOG_DISABLED
    Ref<const Logger> m_logger;
    const void* m_logIdentifier { nullptr };
#endif
    UniqueRef<MediaSourceImpl> m_impl;
    uint64_t m_associatedRegistryCount { 0 };
};

String convertEnumerationToString(MediaSource::EndOfStreamError);
String convertEnumerationToString(MediaSource::ReadyState);

} // namespace WebCore

namespace WTF {

template<typename Type>
struct LogArgument;

template <>
struct LogArgument<WebCore::MediaSource::EndOfStreamError> {
    static String toString(const WebCore::MediaSource::EndOfStreamError error)
    {
        return convertEnumerationToString(error);
    }
};

template <>
struct LogArgument<WebCore::MediaSource::ReadyState> {
    static String toString(const WebCore::MediaSource::ReadyState state)
    {
        return convertEnumerationToString(state);
    }
};

} // namespace WTF

#endif
