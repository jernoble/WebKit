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

#include "config.h"
#include "SourceBuffer.h"

#if ENABLE(MEDIA_SOURCE)

#include "AudioTrackList.h"
#include "Logging.h"
#include "ScriptExecutionContext.h"
#include "SourceBufferImpl.h"
#include "SourceBufferPrivate.h"
#include "MediaSource.h"
#include "TextTrackList.h"
#include "VideoTrackList.h"
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/VM.h>
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(SourceBuffer);

static const double ExponentialMovingAverageCoefficient = 0.2;

Ref<SourceBuffer> SourceBuffer::create(Ref<SourceBufferPrivate>&& sourceBufferPrivate, MediaSource& source)
{
    auto sourceBuffer = adoptRef(*new SourceBuffer(WTFMove(sourceBufferPrivate), source));
    sourceBuffer->suspendIfNeeded();
    return sourceBuffer;
}

SourceBuffer::SourceBuffer(Ref<SourceBufferPrivate>&& sourceBufferPrivate, MediaSource& source)
: ActiveDOMObject(source.scriptExecutionContext())
, m_source(&source)
, m_impl(makeUniqueRef<SourceBufferImpl>(WTFMove(sourceBufferPrivate), *this))
, m_opaqueRootProvider([this] { return opaqueRoot(); })
#if !RELEASE_LOG_DISABLED
, m_logger(m_impl->logger())
, m_logIdentifier(m_impl->logIdentifier())
#endif
{
    ALWAYS_LOG(LOGIDENTIFIER);
}

SourceBuffer::~SourceBuffer()
{
    ASSERT(isRemoved());
    ALWAYS_LOG(LOGIDENTIFIER);
}

auto SourceBuffer::mode() const -> AppendMode
{
    return m_impl->mode();
}

ExceptionOr<void> SourceBuffer::setMode(AppendMode mode)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->setMode(mode);
    });
}

bool SourceBuffer::updating() const
{
    return m_impl->updating();
}

ExceptionOr<Ref<TimeRanges>> SourceBuffer::buffered() const
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->buffered();
    });
}

double SourceBuffer::timestampOffset() const
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->timestampOffset();
    });
}

ExceptionOr<void> SourceBuffer::setTimestampOffset(double timestampOffset)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->setTimestampOffset(timestampOffset);
    });
}

VideoTrackList& SourceBuffer::videoTracks()
{
    if (auto* videoTracks = m_impl->videoTracksIfExists())
        return *videoTracks;

    auto videoTracks = VideoTrackList::create(scriptExecutionContext());
    videoTracks->setOpaqueRootObserver(m_opaqueRootProvider);
    m_impl->setVideoTracks(WTFMove(videoTracks));
    return *m_impl->videoTracksIfExists();
}

VideoTrackList* SourceBuffer::videoTracksIfExists() const
{
    return m_impl->videoTracksIfExists();
}

AudioTrackList& SourceBuffer::audioTracks()
{
    if (auto* audioTracks = m_impl->audioTracksIfExists())
        return *audioTracks;

    auto audioTracks = AudioTrackList::create(scriptExecutionContext());
    audioTracks->setOpaqueRootObserver(m_opaqueRootProvider);
    m_impl->setAudioTracks(WTFMove(audioTracks));
    return *m_impl->audioTracksIfExists();
}

AudioTrackList* SourceBuffer::audioTracksIfExists() const
{
    return m_impl->audioTracksIfExists();
}

TextTrackList& SourceBuffer::textTracks()
{
    if (auto* textTracks = m_impl->textTracksIfExists())
        return *textTracks;

    auto textTracks = TextTrackList::create(scriptExecutionContext());
    textTracks->setOpaqueRootObserver(m_opaqueRootProvider);
    m_impl->setTextTracks(WTFMove(textTracks));
    return *m_impl->textTracksIfExists();
}

void SourceBuffer::addAudioTrack(Ref<AudioTrack>&& track)
{
    audioTracks().append(WTFMove(track));
}

void SourceBuffer::addVideoTrack(Ref<VideoTrack>&& track)
{
    videoTracks().append(WTFMove(track));
}

void SourceBuffer::addTextTrack(Ref<TextTrack>&& track)
{
    textTracks().append(WTFMove(track));
}

TextTrackList* SourceBuffer::textTracksIfExists() const
{
    return m_impl->textTracksIfExists();
}

double SourceBuffer::appendWindowStart() const
{
    return m_impl->appendWindowStart();
}

ExceptionOr<void> SourceBuffer::setAppendWindowStart(double appendWindowStart)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->setAppendWindowStart(appendWindowStart);
    });
}

double SourceBuffer::appendWindowEnd() const
{
    return m_impl->appendWindowEnd();
}

ExceptionOr<void> SourceBuffer::setAppendWindowEnd(double appendWindowEnd)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->setAppendWindowStart(appendWindowEnd);
    });
}

ExceptionOr<void> SourceBuffer::appendBuffer(const BufferSource& buffer)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->appendBuffer(buffer);
    });
}

ExceptionOr<void> SourceBuffer::abort()
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->abort();
    });
}

ExceptionOr<void> SourceBuffer::remove(double start, double end)
{
    // Limit timescale to 1/1000 of microsecond so samples won't accidentally overlap with removal range by precision lost (e.g. by 0.000000000000X [sec]).
    return m_impl->dispatchWorkQueueTaskSync([&] {
    static const uint32_t timescale = 1000000000;
        return m_impl->remove(MediaTime::createWithDouble(start, timescale), MediaTime::createWithDouble(end, timescale));
    });
}

ExceptionOr<void> SourceBuffer::remove(const MediaTime& start, const MediaTime& end)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->remove(start, end);
    });
}

ExceptionOr<void> SourceBuffer::changeType(const String& type)
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->changeType(type);
    });
}

void SourceBuffer::abortIfUpdating()
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->abortIfUpdating();
    });
}

void SourceBuffer::removedFromMediaSource()
{
    m_source = nullptr;
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->removedFromMediaSource();
    });
}

bool SourceBuffer::virtualHasPendingActivity() const
{
    return m_source;
}

void SourceBuffer::stop()
{
    m_impl->dispatchWorkQueueTaskSync([&] {
        m_impl->stop();
    });
}

const char* SourceBuffer::activeDOMObjectName() const
{
    return "SourceBuffer";
}

bool SourceBuffer::isRemoved() const
{
    return !m_source;
}

bool SourceBuffer::hasVideo() const
{
    return m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->hasVideo();
    });
}

void SourceBuffer::reportExtraMemoryAllocated(uint64_t extraMemory)
{
    uint64_t extraMemoryCost = extraMemory;

    m_extraMemoryCost = extraMemoryCost;

    if (extraMemoryCost <= m_reportedExtraMemoryCost)
        return;

    uint64_t extraMemoryCostDelta = extraMemoryCost - m_reportedExtraMemoryCost;
    m_reportedExtraMemoryCost = extraMemoryCost;

    JSC::JSLockHolder lock(scriptExecutionContext()->vm());
    // FIXME: Adopt reportExtraMemoryVisited, and switch to reportExtraMemoryAllocated.
    // https://bugs.webkit.org/show_bug.cgi?id=142595
    scriptExecutionContext()->vm().heap.deprecatedReportExtraMemory(extraMemoryCostDelta);
}

void SourceBuffer::setShouldGenerateTimestamps(bool flag)
{
    m_impl->dispatchWorkQueueTask([this, flag] {
        m_impl->setShouldGenerateTimestamps(flag);
    });
}

size_t SourceBuffer::memoryCost() const
{
    return sizeof(SourceBuffer) + m_extraMemoryCost;
}

WebCoreOpaqueRoot SourceBuffer::opaqueRoot()
{
    return WebCoreOpaqueRoot { this };
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBuffer::logChannel() const
{
    return LogMediaSource;
}
#endif

}

#endif
