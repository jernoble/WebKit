/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 * Copyright (C) 2013-2022 Apple Inc. All rights reserved.
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

#include "config.h"
#include "MediaSource.h"

#if ENABLE(MEDIA_SOURCE)

#include "AudioTrackList.h"
#include "ContentType.h"
#include "ContentTypeUtilities.h"
#include "DeprecatedGlobalSettings.h"
#include "Event.h"
#include "EventNames.h"
#include "HTMLMediaElement.h"
#include "Logging.h"
#include "MediaSourceImpl.h"
#include "MediaSourcePrivate.h"
#include "MediaSourcePrivateClient.h"
#include "MediaSourceRegistry.h"
#include "Quirks.h"
#include "Settings.h"
#include "SourceBuffer.h"
#include "SourceBufferList.h"
#include "SourceBufferPrivate.h"
#include "TextTrackList.h"
#include "TimeRanges.h"
#include "VideoTrack.h"
#include "VideoTrackList.h"
#include <wtf/CancellableTask.h>
#include <wtf/IsoMallocInlines.h>
#include <wtf/WorkQueue.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(MediaSource);

String convertEnumerationToString(MediaSourcePrivate::AddStatus enumerationValue)
{
    static const NeverDestroyed<String> values[] = {
        MAKE_STATIC_STRING_IMPL("Ok"),
        MAKE_STATIC_STRING_IMPL("NotSupported"),
        MAKE_STATIC_STRING_IMPL("ReachedIdLimit"),
    };
    static_assert(static_cast<size_t>(MediaSourcePrivate::AddStatus::Ok) == 0, "MediaSourcePrivate::AddStatus::Ok is not 0 as expected");
    static_assert(static_cast<size_t>(MediaSourcePrivate::AddStatus::NotSupported) == 1, "MediaSourcePrivate::AddStatus::NotSupported is not 1 as expected");
    static_assert(static_cast<size_t>(MediaSourcePrivate::AddStatus::ReachedIdLimit) == 2, "MediaSourcePrivate::AddStatus::ReachedIdLimit is not 2 as expected");
    ASSERT(static_cast<size_t>(enumerationValue) < WTF_ARRAY_LENGTH(values));
    return values[static_cast<size_t>(enumerationValue)];
}

String convertEnumerationToString(MediaSourcePrivate::EndOfStreamStatus enumerationValue)
{
    static const NeverDestroyed<String> values[] = {
        MAKE_STATIC_STRING_IMPL("EosNoError"),
        MAKE_STATIC_STRING_IMPL("EosNetworkError"),
        MAKE_STATIC_STRING_IMPL("EosDecodeError"),
    };
    static_assert(static_cast<size_t>(MediaSourcePrivate::EndOfStreamStatus::EosNoError) == 0, "MediaSourcePrivate::EndOfStreamStatus::EosNoError is not 0 as expected");
    static_assert(static_cast<size_t>(MediaSourcePrivate::EndOfStreamStatus::EosNetworkError) == 1, "MediaSourcePrivate::EndOfStreamStatus::EosNetworkError is not 1 as expected");
    static_assert(static_cast<size_t>(MediaSourcePrivate::EndOfStreamStatus::EosDecodeError) == 2, "MediaSourcePrivate::EndOfStreamStatus::EosDecodeError is not 2 as expected");
    ASSERT(static_cast<size_t>(enumerationValue) < WTF_ARRAY_LENGTH(values));
    return values[static_cast<size_t>(enumerationValue)];
}

URLRegistry* MediaSource::s_registry;

void MediaSource::setRegistry(URLRegistry* registry)
{
    ASSERT(!s_registry);
    s_registry = registry;
}

Ref<MediaSource> MediaSource::create(ScriptExecutionContext& context)
{
    auto mediaSource = adoptRef(*new MediaSource(context));
    mediaSource->suspendIfNeeded();
    return mediaSource;
}

MediaSource::MediaSource(ScriptExecutionContext& context)
    : ActiveDOMObject(&context)
#if !RELEASE_LOG_DISABLED
    , m_logger(context.logger())
#endif
    , m_impl(makeUniqueRef<MediaSourceImpl>(*this, context))
{
}

MediaSource::~MediaSource()
{
    ALWAYS_LOG(LOGIDENTIFIER);
    ASSERT(isClosed());
    stop();
}

void MediaSource::addedToRegistry()
{
    DEBUG_LOG(LOGIDENTIFIER);
    ++m_associatedRegistryCount;
}

void MediaSource::removedFromRegistry()
{
    DEBUG_LOG(LOGIDENTIFIER);
    ASSERT(m_associatedRegistryCount);
    --m_associatedRegistryCount;
}

MediaTime MediaSource::currentTime() const
{
    return m_impl->currentTime();
}

MediaTime MediaSource::duration() const
{
    return m_impl->duration();
}

ExceptionOr<void> MediaSource::setDuration(double duration)
{
    return m_impl->dispatchWorkQueueTaskSync([&] () mutable {
        return m_impl->setDuration(duration);
    });
}

ExceptionOr<void> MediaSource::setDurationInternal(const MediaTime& duration)
{
    return m_impl->dispatchWorkQueueTaskSync([&] () mutable {
        return m_impl->setDurationInternal(duration);
    });
}

std::unique_ptr<PlatformTimeRanges> MediaSource::buffered() const
{
    return m_impl->buffered();
}

Ref<TimeRanges> MediaSource::seekable()
{
    return m_impl->seekable();
}

ExceptionOr<void> MediaSource::setLiveSeekableRange(double start, double end)
{
    return m_impl->dispatchWorkQueueTaskSync([&] () mutable {
        return m_impl->setLiveSeekableRange(start, end);
    });
}

ExceptionOr<void> MediaSource::clearLiveSeekableRange()
{
    return m_impl->dispatchWorkQueueTaskSync([&] () mutable {
        return m_impl->clearLiveSeekableRange();
    });
}

bool MediaSource::contentTypeShouldGenerateTimestamps(const ContentType& contentType)
{
    return MediaSourceImpl::contentTypeShouldGenerateTimestamps(contentType);
}

auto MediaSource::readyState() const -> ReadyState
{
    return m_impl->readyState();
}

ExceptionOr<void> MediaSource::endOfStream(std::optional<EndOfStreamError> error)
{
    return m_impl->dispatchWorkQueueTaskSync([this, error = WTFMove(error)] () -> ExceptionOr<void> {
        return m_impl->endOfStream(error);
    });
}

void MediaSource::streamEndedWithError(std::optional<EndOfStreamError> error)
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->streamEndedWithError(error);
    });
}

ExceptionOr<Ref<SourceBuffer>> MediaSource::addSourceBuffer(const String& type)
{
    // 2.2 http://www.w3.org/TR/media-source/#widl-MediaSource-addSourceBuffer-SourceBuffer-DOMString-type
    // When this method is invoked, the user agent must run the following steps:

    // Steps 1-7 are continued in MediaSourceImpl::addSourceBufferPrivate()

    auto sourceBufferOrException = m_impl->dispatchWorkQueueTaskSync([&] {
        return m_impl->createSourceBufferPrivate(type);
    });

    if (sourceBufferOrException.hasException()) {
        // 2. If type contains a MIME type that is not supported ..., then throw a NotSupportedError exception and abort these steps.
        // 3. If the user agent can't handle any more SourceBuffer objects then throw a QuotaExceededError exception and abort these steps
        return sourceBufferOrException.releaseException();
    }

    auto buffer = SourceBuffer::create(sourceBufferOrException.releaseReturnValue(), *this);
    DEBUG_LOG(LOGIDENTIFIER, "created SourceBuffer");

    // 6. Set the generate timestamps flag on the new object to the value in the "Generate Timestamps Flag"
    // column of the byte stream format registry [MSE-REGISTRY] entry that is associated with type.
    // NOTE: In the current byte stream format registry <http://www.w3.org/2013/12/byte-stream-format-registry/>
    // only the "MPEG Audio Byte Stream Format" has the "Generate Timestamps Flag" value set.
    bool shouldGenerateTimestamps = contentTypeShouldGenerateTimestamps(ContentType { type });
    buffer->setShouldGenerateTimestamps(shouldGenerateTimestamps);

    // 7. If the generate timestamps flag equals true:
    // ↳ Set the mode attribute on the new object to "sequence".
    // Otherwise:
    // ↳ Set the mode attribute on the new object to "segments".
    buffer->setMode(shouldGenerateTimestamps ? SourceBuffer::AppendMode::Sequence : SourceBuffer::AppendMode::Segments);

    // 8. Add the new object to sourceBuffers and fire a addsourcebuffer on that object.
    m_impl->dispatchSourceBufferTaskSync([&] {
        m_impl->sourceBuffers().add(buffer.copyRef());
    });
    m_impl->workQueue().dispatchSync([&] {
        m_impl->regenerateActiveSourceBuffers();
    });


    // 9. Return the new object to the caller.
    return buffer;
}

ExceptionOr<void> MediaSource::removeSourceBuffer(SourceBuffer& buffer)
{
    DEBUG_LOG(LOGIDENTIFIER);

    Ref<SourceBuffer> protect(buffer);

    // 2. If sourceBuffer specifies an object that is not in sourceBuffers then
    // throw a NotFoundError exception and abort these steps.
    if (!m_impl->sourceBuffers().length() || !m_impl->sourceBuffers().contains(buffer))
        return Exception { NotFoundError };

    // 3. If the sourceBuffer.updating attribute equals true, then run the following steps: ...
    buffer.abortIfUpdating();

    if (!isContextStopped()) {
        // 4. Let SourceBuffer audioTracks list equal the AudioTrackList object returned by sourceBuffer.audioTracks.
        auto* audioTracks = buffer.audioTracksIfExists();

        // 5. If the SourceBuffer audioTracks list is not empty, then run the following steps:
        if (audioTracks && audioTracks->length()) {
            // 5.1 Let HTMLMediaElement audioTracks list equal the AudioTrackList object returned by the audioTracks
            // attribute on the HTMLMediaElement.
            // 5.2 Let the removed enabled audio track flag equal false.
            bool removedEnabledAudioTrack = false;

            // 5.3 For each AudioTrack object in the SourceBuffer audioTracks list, run the following steps:
            while (audioTracks->length()) {
                auto& track = *audioTracks->lastItem();

                // 5.3.1 Set the sourceBuffer attribute on the AudioTrack object to null.
                track.setSourceBuffer(nullptr);

                // 5.3.2 If the enabled attribute on the AudioTrack object is true, then set the removed enabled
                // audio track flag to true.
                if (track.enabled())
                    removedEnabledAudioTrack = true;

                // 5.3.3 Remove the AudioTrack object from the HTMLMediaElement audioTracks list.
                // 5.3.4 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the HTMLMediaElement audioTracks list.
                m_impl->dispatchMediaElementTask([track = Ref { track }] (HTMLMediaElement& mediaElement) mutable {
                    mediaElement.removeAudioTrack(WTFMove(track));
                });

                // 5.3.5 Remove the AudioTrack object from the SourceBuffer audioTracks list.
                // 5.3.6 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the SourceBuffer audioTracks list.
                audioTracks->remove(track);
            }

            // 5.4 If the removed enabled audio track flag equals true, then queue a task to fire a simple event
            // named change at the HTMLMediaElement audioTracks list.
            if (removedEnabledAudioTrack) {
                m_impl->dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
                    mediaElement.ensureAudioTracks().scheduleChangeEvent();
                });
            }
        }

        // 6. Let SourceBuffer videoTracks list equal the VideoTrackList object returned by sourceBuffer.videoTracks.
        auto* videoTracks = buffer.videoTracksIfExists();

        // 7. If the SourceBuffer videoTracks list is not empty, then run the following steps:
        if (videoTracks && videoTracks->length()) {
            // 7.1 Let HTMLMediaElement videoTracks list equal the VideoTrackList object returned by the videoTracks
            // attribute on the HTMLMediaElement.
            // 7.2 Let the removed selected video track flag equal false.
            bool removedSelectedVideoTrack = false;

            // 7.3 For each VideoTrack object in the SourceBuffer videoTracks list, run the following steps:
            while (videoTracks->length()) {
                auto& track = *videoTracks->lastItem();

                // 7.3.1 Set the sourceBuffer attribute on the VideoTrack object to null.
                track.setSourceBuffer(nullptr);

                // 7.3.2 If the selected attribute on the VideoTrack object is true, then set the removed selected
                // video track flag to true.
                if (track.selected())
                    removedSelectedVideoTrack = true;

                // 7.3.3 Remove the VideoTrack object from the HTMLMediaElement videoTracks list.
                // 7.3.4 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the HTMLMediaElement videoTracks list.
                m_impl->dispatchMediaElementTask([track = Ref { track }] (HTMLMediaElement& mediaElement) mutable {
                    mediaElement.removeVideoTrack(WTFMove(track));
                });

                // 7.3.5 Remove the VideoTrack object from the SourceBuffer videoTracks list.
                // 7.3.6 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the SourceBuffer videoTracks list.
                videoTracks->remove(track);
            }

            // 7.4 If the removed selected video track flag equals true, then queue a task to fire a simple event
            // named change at the HTMLMediaElement videoTracks list.
            if (removedSelectedVideoTrack) {
                m_impl->dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {                        mediaElement.ensureVideoTracks().scheduleChangeEvent();
                });
            }
        }

        // 8. Let SourceBuffer textTracks list equal the TextTrackList object returned by sourceBuffer.textTracks.
        auto* textTracks = buffer.textTracksIfExists();

        // 9. If the SourceBuffer textTracks list is not empty, then run the following steps:
        if (textTracks && textTracks->length()) {
            // 9.1 Let HTMLMediaElement textTracks list equal the TextTrackList object returned by the textTracks
            // attribute on the HTMLMediaElement.
            // 9.2 Let the removed enabled text track flag equal false.
            bool removedEnabledTextTrack = false;

            // 9.3 For each TextTrack object in the SourceBuffer textTracks list, run the following steps:
            while (textTracks->length()) {
                auto& track = *textTracks->lastItem();

                // 9.3.1 Set the sourceBuffer attribute on the TextTrack object to null.
                track.setSourceBuffer(nullptr);

                // 9.3.2 If the mode attribute on the TextTrack object is set to "showing" or "hidden", then
                // set the removed enabled text track flag to true.
                if (track.mode() == TextTrack::Mode::Showing || track.mode() == TextTrack::Mode::Hidden)
                    removedEnabledTextTrack = true;

                // 9.3.3 Remove the TextTrack object from the HTMLMediaElement textTracks list.
                // 9.3.4 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the HTMLMediaElement textTracks list.
                m_impl->dispatchMediaElementTask([track = Ref { track }] (HTMLMediaElement& mediaElement) mutable {
                    mediaElement.removeTextTrack(WTFMove(track));
                });

                // 9.3.5 Remove the TextTrack object from the SourceBuffer textTracks list.
                // 9.3.6 Queue a task to fire a trusted event named removetrack, that does not bubble and is not
                // cancelable, and that uses the TrackEvent interface, at the SourceBuffer textTracks list.
                textTracks->remove(track);
            }

            // 9.4 If the removed enabled text track flag equals true, then queue a task to fire a simple event
            // named change at the HTMLMediaElement textTracks list.
            if (removedEnabledTextTrack) {
                m_impl->dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
                    mediaElement.ensureTextTracks().scheduleChangeEvent();
                });
            }
        }
    }

    m_impl->dispatchSourceBufferTaskSync([&] {
        // 10. If sourceBuffer is in activeSourceBuffers, then remove sourceBuffer from activeSourceBuffers ...
        m_impl->activeSourceBuffers().remove(buffer);

        // 11. Remove sourceBuffer from sourceBuffers and fire a removesourcebuffer event
        // on that object.
        m_impl->sourceBuffers().remove(buffer);
    });

    // 12. Destroy all resources for sourceBuffer.
    buffer.removedFromMediaSource();

    return { };
}

void MediaSource::monitorSourceBuffers()
{
    return m_impl->dispatchWorkQueueTask([&] {
        return m_impl->monitorSourceBuffers();
    });
}

bool MediaSource::isTypeSupported(ScriptExecutionContext& context, const String& type)
{
    bool needsVP9FullRangeQuirk = false;
    if (context.isDocument())
        needsVP9FullRangeQuirk = downcast<Document>(context).quirks().needsVP9FullRangeFlagQuirk();

    return MediaSourceImpl::isTypeSupported(type,
        needsVP9FullRangeQuirk,
        context.settingsValues().mediaContentTypesRequiringHardwareSupport,
        context.settingsValues().allowedMediaCodecTypes,
        context.settingsValues().allowedMediaContainerTypes,
        context.settingsValues().allowedMediaVideoCodecIDs,
        context.settingsValues().allowedMediaAudioCodecIDs,
        context.settingsValues().allowedMediaCaptionFormatTypes);
}

bool MediaSource::isOpen() const
{
    return m_impl->isOpen();
}

bool MediaSource::isClosed() const
{
    return m_impl->isClosed();
}

bool MediaSource::isEnded() const
{
    return m_impl->isEnded();
}

WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData> MediaSource::mediaElement() const
{
    return m_impl->mediaElement();
}

SourceBufferList& MediaSource::sourceBuffers()
{
    return m_impl->sourceBuffers();
}

SourceBufferList& MediaSource::activeSourceBuffers()
{
    return m_impl->activeSourceBuffers();
}

void MediaSource::openIfInEndedState()
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->openIfInEndedState();
    });
}

bool MediaSource::attachToElement(HTMLMediaElement& element)
{
    if (m_impl->mediaElement())
        return false;

    ASSERT(isClosed());

    m_impl->setMediaElement(element);
    return true;
}

void MediaSource::detachFromElement(HTMLMediaElement& element)
{
    ALWAYS_LOG(LOGIDENTIFIER);

    // 2.4.2 Detaching from a media element
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#mediasource-detach

    m_impl->dispatchSourceBufferTaskSync([&] {
        // 3. Remove all the SourceBuffer objects from activeSourceBuffers.
        // 4. Queue a task to fire a simple event named removesourcebuffer at activeSourceBuffers.
        activeSourceBuffers().clear();

        // 5. Remove all the SourceBuffer objects from sourceBuffers.
        // 6. Queue a task to fire a simple event named removesourcebuffer at sourceBuffers.
        sourceBuffers().clear();
    });

    // 2. Update duration to NaN.
    m_impl->invalidateDuration();

    // Continued in MediaSourceImpl::detachFromElement()
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->detachFromElement(element);
    });
}

void MediaSource::sourceBufferDidChangeActiveState(SourceBuffer&, bool)
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->regenerateActiveSourceBuffers();
    });
}

void MediaSource::sourceBufferDidChangeBufferedDirty(SourceBuffer&, bool)
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->updateBufferedIfNeeded();
    });
}

MediaSourcePrivateClient& MediaSource::client()
{
    return m_impl.get();
}

bool MediaSource::virtualHasPendingActivity() const
{
    return isOpen() || isEnded() || m_associatedRegistryCount;
}

void MediaSource::stop()
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->stop();
    });
}

const char* MediaSource::activeDOMObjectName() const
{
    return "MediaSource";
}

ScriptExecutionContext* MediaSource::scriptExecutionContext() const
{
    return ActiveDOMObject::scriptExecutionContext();
}

EventTargetInterface MediaSource::eventTargetInterface() const
{
    return MediaSourceEventTargetInterfaceType;
}

URLRegistry& MediaSource::registry() const
{
    return MediaSourceRegistry::registry();
}

#if !RELEASE_LOG_DISABLED
void MediaSource::setLogIdentifier(const void* identifier)
{
    m_logIdentifier = identifier;
    ALWAYS_LOG(LOGIDENTIFIER);
}

WTFLogChannel& MediaSource::logChannel() const
{
    return LogMediaSource;
}
#endif

void MediaSource::didReceiveInitializationSegment(bool activeTrackFlag)
{
    m_impl->dispatchWorkQueueTask([&] {
        m_impl->didReceiveInitializationSegment(activeTrackFlag);
    });
}

}

#endif
