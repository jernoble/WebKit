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
#include "MediaSourceImpl.h"

#if ENABLE(MEDIA_SOURCE)

#include "ContentTypeUtilities.h"
#include "DeprecatedGlobalSettings.h"
#include "Document.h"
#include "EventNames.h"
#include "HTMLMediaElement.h"
#include "Logging.h"
#include "MediaSourcePrivate.h"
#include "Quirks.h"
#include "SourceBuffer.h"
#include "SourceBufferImpl.h"
#include "TimeRanges.h"
#include <MediaSource.h>
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(MediaSourceImpl);

MediaSourceImpl::MediaSourceImpl(MediaSource& mediaSource, ScriptExecutionContext& context)
: m_mediaSource(mediaSource)
, m_workQueue(WorkQueue::create("MediaSource work queue"))
, m_sourceBufferWorkQueue(WorkQueue::create("MediaSource sourcebuffer work queue"))
, m_ivarWorkQueue(WorkQueue::create("MediaSource ivar work queue"))
, m_sourceBuffers(SourceBufferList::create(&context))
, m_activeSourceBuffers(SourceBufferList::create(&context))
, m_buffered(makeUniqueRef<PlatformTimeRanges>())
, m_duration(MediaTime::invalidTime())
, m_pendingSeekTime(MediaTime::invalidTime())
, m_webmParserEnabled(DeprecatedGlobalSettings::webMParserEnabled())
, m_contentTypesRequiringHardwareSupport(context.settingsValues().mediaContentTypesRequiringHardwareSupport)
, m_allowedMediaCodecTypes(context.settingsValues().allowedMediaCodecTypes)
, m_allowedMediaContainerTypes(context.settingsValues().allowedMediaContainerTypes)
, m_allowedMediaVideoCodecIDs(context.settingsValues().allowedMediaVideoCodecIDs)
, m_allowedMediaAudioCodecIDs(context.settingsValues().allowedMediaAudioCodecIDs)
, m_allowedMediaCaptionFormatTypes(context.settingsValues().allowedMediaCaptionFormatTypes)
#if !RELEASE_LOG_DISABLED
, m_logger(mediaSource.logger())
#endif
{
    if (context.isDocument())
        m_needsVP9FullRangeFlagQuirk = downcast<Document>(context).quirks().needsVP9FullRangeFlagQuirk();
}

MediaSourceImpl::~MediaSourceImpl()
{
    ASSERT(!m_private);
}

void MediaSourceImpl::setPrivateAndOpen(Ref<MediaSourcePrivate>&& mediaSourcePrivate)
{
    assertIsCurrent(workQueue());
    DEBUG_LOG(LOGIDENTIFIER);
    ASSERT(!m_private);
    m_private = WTFMove(mediaSourcePrivate);
    m_private->setTimeFudgeFactor(currentTimeFudgeFactor());

    // 2.4.1 Attaching to a media element
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#mediasource-attach

    // ↳ If readyState is NOT set to "closed"
    //    Run the "If the media data cannot be fetched at all, due to network errors, causing the user agent to give up trying
    //    to fetch the resource" steps of the resource fetch algorithm's media data processing steps list.
    if (!isClosed()) {
        dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {                mediaElement.mediaLoadingFailedFatally(MediaPlayer::NetworkState::NetworkError);
        });
        return;
    }

    // ↳ Otherwise
    // 1. Set the media element's delaying-the-load-event-flag to false.
    dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
        mediaElement.setShouldDelayLoadEvent(false);
    });

    // 2. Set the readyState attribute to "open".
    // 3. Queue a task to fire a simple event named sourceopen at the MediaSource.
    setReadyState(ReadyState::Open);

    // 4. Continue the resource fetch algorithm by running the remaining "Otherwise (mode is local)" steps,
    // with these clarifications:
    // NOTE: This is handled in HTMLMediaElement.
}

WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData> MediaSourceImpl::mediaElement() const
{
    return accessIvarSync([&] { return m_mediaElement; });
}

void MediaSourceImpl::setMediaElement(WeakPtr<HTMLMediaElement, WeakPtrImplWithEventTargetData>&& mediaElement)
{
    accessIvarSync([&] { m_mediaElement = WTFMove(mediaElement); });
}

MediaTime MediaSourceImpl::duration() const
{
    return accessIvarSync([&] { return m_duration; });
}

void MediaSourceImpl::invalidateDuration()
{
    accessIvarSync([&] { m_duration = MediaTime::invalidTime(); });
}

std::unique_ptr<PlatformTimeRanges> MediaSourceImpl::buffered() const
{
    return accessIvarSync([&] { return makeUnique<PlatformTimeRanges>(m_buffered.get()); });
}

void MediaSourceImpl::setBuffered(UniqueRef<PlatformTimeRanges>&& buffered)
{
    accessIvarSync([&] () mutable { m_buffered = WTFMove(buffered); });
}

MediaTime MediaSourceImpl::currentTime() const
{
    // FIXME: Sync calls between contexts is bad.
    if (isMainThread()) {
        if (auto mediaElement = this->mediaElement())
            return mediaElement->currentMediaTime();
        return MediaTime::zeroTime();
    }

    MediaTime currentTime;
    callOnMainThreadAndWait([&] { currentTime = this->currentTime(); });
    return currentTime;
}

void MediaSourceImpl::seekToTime(const MediaTime& time)
{
    assertIsCurrent(workQueue());
    if (isClosed())
        return;

    ALWAYS_LOG(LOGIDENTIFIER, time);

    // 2.4.3 Seeking
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#mediasource-seeking

    m_pendingSeekTime = time;
    m_private->setIsSeeking(true);

    // Run the following steps as part of the "Wait until the user agent has established whether or not the
    // media data for the new playback position is available, and, if it is, until it has decoded enough data
    // to play back that position" step of the seek algorithm:
    // ↳ If new playback position is not in any TimeRange of HTMLMediaElement.buffered
    if (!hasBufferedTime(time)) {
        // 1. If the HTMLMediaElement.readyState attribute is greater than HAVE_METADATA,
        // then set the HTMLMediaElement.readyState attribute to HAVE_METADATA.
        m_private->setReadyState(MediaPlayer::ReadyState::HaveMetadata);

        // 2. The media element waits until an appendBuffer() or an appendStream() call causes the coded
        // frame processing algorithm to set the HTMLMediaElement.readyState attribute to a value greater
        // than HAVE_METADATA.
        m_private->waitForSeekCompleted();
        return;
    }
    // ↳ Otherwise
    // Continue

    // https://bugs.webkit.org/show_bug.cgi?id=125157 broke seek on MediaPlayerPrivateGStreamerMSE
#if !USE(GSTREAMER)
    m_private->waitForSeekCompleted();
#endif
    completeSeek();
}

void MediaSourceImpl::completeSeek()
{
    assertIsCurrent(workQueue());
    if (isClosed())
        return;

    // 2.4.3 Seeking, ctd.
    // https://dvcs.w3.org/hg/html-media/raw-file/tip/media-source/media-source.html#mediasource-seeking

    ASSERT(m_pendingSeekTime.isValid());

    ALWAYS_LOG(LOGIDENTIFIER, m_pendingSeekTime);

    // 2. The media element resets all decoders and initializes each one with data from the appropriate
    // initialization segment.
    // 3. The media element feeds coded frames from the active track buffers into the decoders starting
    // with the closest random access point before the new playback position.
    MediaTime pendingSeekTime = m_pendingSeekTime;
    m_pendingSeekTime = MediaTime::invalidTime();
    m_private->setIsSeeking(false);
    forEachActiveSourceBuffer([&] (auto& sourceBuffer) {
        sourceBuffer.impl().seekToTime(pendingSeekTime);
    });

    // 4. Resume the seek algorithm at the "Await a stable state" step.
    m_private->seekCompleted();

    monitorSourceBuffers();
}

Ref<TimeRanges> MediaSourceImpl::seekable()
{
    assertIsCurrent(workQueue());

    // 6. HTMLMediaElement Extensions, seekable
    // W3C Editor's Draft 16 September 2016
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#htmlmediaelement-extensions

    // ↳ If duration equals NaN:
    // Return an empty TimeRanges object.
    if (m_duration.isInvalid())
        return TimeRanges::create();

    // ↳ If duration equals positive Infinity:
    if (m_duration.isPositiveInfinite()) {
        auto buffered = this->buffered();
        // If live seekable range is not empty:
        if (m_liveSeekable && m_liveSeekable->length()) {
            // Let union ranges be the union of live seekable range and the HTMLMediaElement.buffered attribute.
            buffered->unionWith(*m_liveSeekable);
            // Return a single range with a start time equal to the earliest start time in union ranges
            // and an end time equal to the highest end time in union ranges and abort these steps.
            buffered->add(buffered->start(0), buffered->maximumBufferedTime());
            return TimeRanges::create(*buffered);
        }

        // If the HTMLMediaElement.buffered attribute returns an empty TimeRanges object, then return
        // an empty TimeRanges object and abort these steps.
        if (!buffered->length())
            return TimeRanges::create();

        // Return a single range with a start time of 0 and an end time equal to the highest end time
        // reported by the HTMLMediaElement.buffered attribute.
        return TimeRanges::create({MediaTime::zeroTime(), buffered->maximumBufferedTime()});
    }

    // ↳ Otherwise:
    // Return a single range with a start time of 0 and an end time equal to duration.
    return TimeRanges::create({MediaTime::zeroTime(), m_duration});
}

ExceptionOr<void> MediaSourceImpl::setLiveSeekableRange(double start, double end)
{
    assertIsCurrent(workQueue());

    // W3C Editor's Draft 16 September 2016
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#dom-mediasource-setliveseekablerange

    ALWAYS_LOG(LOGIDENTIFIER, "start = ", start, ", end = ", end);

    // If the readyState attribute is not "open" then throw an InvalidStateError exception and abort these steps.
    if (!isOpen())
        return Exception { InvalidStateError };

    // If start is negative or greater than end, then throw a TypeError exception and abort these steps.
    if (start < 0 || start > end)
        return Exception { TypeError };

    // Set live seekable range to be a new normalized TimeRanges object containing a single range
    // whose start position is start and end position is end.
    m_liveSeekable = makeUnique<PlatformTimeRanges>(MediaTime::createWithDouble(start), MediaTime::createWithDouble(end));

    return { };
}

ExceptionOr<void> MediaSourceImpl::clearLiveSeekableRange()
{
    assertIsCurrent(workQueue());

    // W3C Editor's Draft 16 September 2016
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#dom-mediasource-clearliveseekablerange

    ALWAYS_LOG(LOGIDENTIFIER);

    // If the readyState attribute is not "open" then throw an InvalidStateError exception and abort these steps.
    if (!isOpen())
        return Exception { InvalidStateError };
    m_liveSeekable = nullptr;
    return { };
}

const MediaTime& MediaSourceImpl::currentTimeFudgeFactor()
{
    // Allow hasCurrentTime() to be off by as much as the length of two 24fps video frames
    static NeverDestroyed<MediaTime> fudgeFactor(2002, 24000);
    return fudgeFactor;
}

bool MediaSourceImpl::contentTypeShouldGenerateTimestamps(const ContentType& contentType)
{
    return contentType.containerType() == "audio/aac"_s || contentType.containerType() == "audio/mpeg"_s;
}

bool MediaSourceImpl::hasBufferedTime(const MediaTime& time)
{
    assertIsCurrent(workQueue());
    if (time > duration())
        return false;

    auto ranges = buffered();
    if (!ranges->length())
        return false;

    return abs(ranges->nearest(time) - time) <= currentTimeFudgeFactor();
}

bool MediaSourceImpl::hasFutureTimeAfterCurrentTime(const MediaTime& currentTime)
{
    assertIsCurrent(workQueue());
    MediaTime duration = this->duration();

    if (currentTime >= duration)
        return true;

    auto ranges = buffered();
    MediaTime nearest = ranges->nearest(currentTime);
    if (abs(nearest - currentTime) > currentTimeFudgeFactor())
        return false;

    size_t found = ranges->find(nearest);
    if (found == notFound)
        return false;

    MediaTime localEnd = ranges->end(found);
    if (localEnd == duration)
        return true;

    return localEnd - currentTime > currentTimeFudgeFactor();
}

void MediaSourceImpl::monitorSourceBuffers()
{
    dispatchMediaElementTask([this] (HTMLMediaElement& mediaElement) {
        dispatchWorkQueueTask([this, currentTime = mediaElement.currentMediaTime(), readyState = mediaElement.readyState()] {
            monitorSourceBuffersWithCurrentTimeAndReadyState(currentTime, readyState);
        });
    });
}

void MediaSourceImpl::monitorSourceBuffersWithCurrentTimeAndReadyState(const MediaTime& currentTime, HTMLMediaElement::ReadyState readyState)
{
    assertIsCurrent(workQueue());
    if (isClosed())
        return;

    // 2.4.4 SourceBuffer Monitoring
    // https://rawgit.com/w3c/media-source/45627646344eea0170dd1cbc5a3d508ca751abb8/media-source-respec.html#buffer-monitoring

    // Note, the behavior if activeSourceBuffers is empty is undefined.
    if (!activeSourceBuffers().length()) {
        m_private->setReadyState(MediaPlayer::ReadyState::HaveNothing);
        return;
    }

    // ↳ If the HTMLMediaElement.readyState attribute equals HAVE_NOTHING:
    if (readyState == HTMLMediaElement::HAVE_NOTHING) {
        // 1. Abort these steps.
        return;
    }

    // ↳ If HTMLMediaElement.buffered does not contain a TimeRange for the current playback position:
    if (!hasBufferedTime(currentTime)) {
        // 1. Set the HTMLMediaElement.readyState attribute to HAVE_METADATA.
        // 2. If this is the first transition to HAVE_METADATA, then queue a task to fire a simple event
        // named loadedmetadata at the media element.
        m_private->setReadyState(MediaPlayer::ReadyState::HaveMetadata);

        // 3. Abort these steps.
        return;
    }

    // ↳ If HTMLMediaElement.buffered contains a TimeRange that includes the current
    //  playback position and enough data to ensure uninterrupted playback:
    auto ranges = buffered();
    if (everyActiveSourceBuffer([&](auto& sourceBuffer) {
        return sourceBuffer.impl().canPlayThroughRange(*ranges);
    })) {
        // 1. Set the HTMLMediaElement.readyState attribute to HAVE_ENOUGH_DATA.
        // 2. Queue a task to fire a simple event named canplaythrough at the media element.
        // 3. Playback may resume at this point if it was previously suspended by a transition to HAVE_CURRENT_DATA.
        m_private->setReadyState(MediaPlayer::ReadyState::HaveEnoughData);

        if (m_pendingSeekTime.isValid())
            completeSeek();

        // 4. Abort these steps.
        return;
    }

    // ↳ If HTMLMediaElement.buffered contains a TimeRange that includes the current playback
    //  position and some time beyond the current playback position, then run the following steps:
    if (hasFutureTimeAfterCurrentTime(currentTime)) {
        // 1. Set the HTMLMediaElement.readyState attribute to HAVE_FUTURE_DATA.
        // 2. If the previous value of HTMLMediaElement.readyState was less than HAVE_FUTURE_DATA, then queue a task to fire a simple event named canplay at the media element.
        // 3. Playback may resume at this point if it was previously suspended by a transition to HAVE_CURRENT_DATA.
        m_private->setReadyState(MediaPlayer::ReadyState::HaveFutureData);

        if (m_pendingSeekTime.isValid())
            completeSeek();

        // 4. Abort these steps.
        return;
    }

    // ↳ If HTMLMediaElement.buffered contains a TimeRange that ends at the current playback position and does not have a range covering the time immediately after the current position:
    // NOTE: Logically, !(all objects do not contain currentTime) == (some objects contain current time)

    // 1. Set the HTMLMediaElement.readyState attribute to HAVE_CURRENT_DATA.
    // 2. If this is the first transition to HAVE_CURRENT_DATA, then queue a task to fire a simple
    // event named loadeddata at the media element.
    // 3. Playback is suspended at this point since the media element doesn't have enough data to
    // advance the media timeline.
    m_private->setReadyState(MediaPlayer::ReadyState::HaveCurrentData);

    if (m_pendingSeekTime.isValid())
        completeSeek();

    // 4. Abort these steps.
}

ExceptionOr<void> MediaSourceImpl::setDuration(double duration)
{
    assertIsCurrent(workQueue());

    // 2.1 Attributes - Duration
    // https://www.w3.org/TR/2016/REC-media-source-20161117/#attributes

    ALWAYS_LOG(LOGIDENTIFIER, duration);

    // On setting, run the following steps:
    // 1. If the value being set is negative or NaN then throw a TypeError exception and abort these steps.
    if (duration < 0.0 || std::isnan(duration))
        return Exception { TypeError };

    // 2. If the readyState attribute is not "open" then throw an InvalidStateError exception and abort these steps.
    if (!isOpen())
        return Exception { InvalidStateError };

    // 3. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an InvalidStateError
    // exception and abort these steps.
    if (anySourceBuffer([](auto& sourceBuffer) { return sourceBuffer.impl().updating(); }))
        return Exception { InvalidStateError };

    return setDurationInternal(MediaTime::createWithDouble(duration));
}

ExceptionOr<void> MediaSourceImpl::setDurationInternal(const MediaTime& duration)
{
    assertIsCurrent(workQueue());
    // 2.4.6 Duration Change
    // https://www.w3.org/TR/2016/REC-media-source-20161117/#duration-change-algorithm

    MediaTime newDuration = duration;

    // 1. If the current value of duration is equal to new duration, then return.
    if (newDuration == this->duration())
        return { };

    // 2. If new duration is less than the highest presentation timestamp of any buffered coded frames
    // for all SourceBuffer objects in sourceBuffers, then throw an InvalidStateError exception and
    // abort these steps.
    // 3. Let highest end time be the largest track buffer ranges end time across all the track buffers
    // across all SourceBuffer objects in sourceBuffers.
    MediaTime highestPresentationTimestamp;
    MediaTime highestEndTime;
    forEachSourceBuffer([&] (auto& sourceBuffer) {
        highestPresentationTimestamp = std::max(highestPresentationTimestamp, sourceBuffer.impl().highestPresentationTimestamp());
        highestEndTime = std::max(highestEndTime, sourceBuffer.impl().bufferedInternal().ranges().maximumBufferedTime());
    });
    if (highestPresentationTimestamp.isValid() && newDuration < highestPresentationTimestamp)
        return Exception { InvalidStateError };

    // 4. If new duration is less than highest end time, then
    // 4.1. Update new duration to equal highest end time.
    if (highestEndTime.isValid() && newDuration < highestEndTime)
        newDuration = highestEndTime;

    // 5. Update duration to new duration.
    m_duration = newDuration;
    ALWAYS_LOG(LOGIDENTIFIER, duration);

    // 6. Update the media duration to new duration and run the HTMLMediaElement duration change algorithm.
    m_private->durationChanged(duration);

    return { };
}

void MediaSourceImpl::setReadyState(ReadyState state)
{
    assertIsCurrent(workQueue());
    auto oldState = readyState();
    if (oldState == state)
        return;

    m_readyState = state;

    onReadyStateChange(oldState, state);
}

ExceptionOr<void> MediaSourceImpl::endOfStream(std::optional<EndOfStreamError> error)
{
    // 2.2 https://dvcs.w3.org/hg/html-media/raw-file/tip/media-source/media-source.html#widl-MediaSource-endOfStream-void-EndOfStreamError-error
    // 1. If the readyState attribute is not in the "open" state then throw an
    // InvalidStateError exception and abort these steps.
    if (!isOpen())
        return Exception { InvalidStateError };

    // 2. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an
    // InvalidStateError exception and abort these steps.
    if (anySourceBuffer([](auto& sourceBuffer) { return sourceBuffer.impl().updating(); }))
        return Exception { InvalidStateError };

    // 3. Run the end of stream algorithm with the error parameter set to error.
    streamEndedWithError(error);
    return { };
}

void MediaSourceImpl::streamEndedWithError(std::optional<EndOfStreamError> error)
{
    assertIsCurrent(workQueue());
#if !RELEASE_LOG_DISABLED
    if (error)
        ALWAYS_LOG(LOGIDENTIFIER, error.value());
    else
        ALWAYS_LOG(LOGIDENTIFIER);
#endif

    if (isClosed())
        return;

    // 2.4.7 https://dvcs.w3.org/hg/html-media/raw-file/tip/media-source/media-source.html#end-of-stream-algorithm

    // 1. Change the readyState attribute value to "ended".
    // 2. Queue a task to fire a simple event named sourceended at the MediaSource.
    setReadyState(ReadyState::Ended);

    // 3.
    if (!error) {
        // ↳ If error is not set, is null, or is an empty string
        // 1. Run the duration change algorithm with new duration set to the highest end time reported by
        // the buffered attribute across all SourceBuffer objects in sourceBuffers.
        MediaTime maxEndTime;
        forEachSourceBuffer([&] (auto& sourceBuffer) {
            if (auto length = sourceBuffer.impl().bufferedInternal().length())
                maxEndTime = std::max(sourceBuffer.impl().bufferedInternal().ranges().end(length - 1), maxEndTime);
        });
        setDurationInternal(maxEndTime);

        // 2. Notify the media element that it now has all of the media data.
        forEachSourceBuffer([&] (auto& sourceBuffer) {
            sourceBuffer.impl().setMediaSourceEnded(true);
        });
        m_private->markEndOfStream(MediaSourcePrivate::EosNoError);
    } else if (error == EndOfStreamError::Network) {
        m_private->markEndOfStream(MediaSourcePrivate::EosNetworkError);
        // ↳ If error is set to "network"
        dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
            if (mediaElement.readyState() == HTMLMediaElement::HAVE_NOTHING) {
                //  ↳ If the HTMLMediaElement.readyState attribute equals HAVE_NOTHING
                //    Run the "If the media data cannot be fetched at all, due to network errors, causing
                //    the user agent to give up trying to fetch the resource" steps of the resource fetch algorithm.
                //    NOTE: This step is handled by HTMLMediaElement::mediaLoadingFailed().
                mediaElement.mediaLoadingFailed(MediaPlayer::NetworkState::NetworkError);
            } else {
                //  ↳ If the HTMLMediaElement.readyState attribute is greater than HAVE_NOTHING
                //    Run the "If the connection is interrupted after some media data has been received, causing the
                //    user agent to give up trying to fetch the resource" steps of the resource fetch algorithm.
                //    NOTE: This step is handled by HTMLMediaElement::mediaLoadingFailedFatally().
                mediaElement.mediaLoadingFailedFatally(MediaPlayer::NetworkState::NetworkError);
            }
        });
    } else {
        // ↳ If error is set to "decode"
        ASSERT(error == EndOfStreamError::Decode);
        m_private->markEndOfStream(MediaSourcePrivate::EosDecodeError);

        dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
            if (mediaElement.readyState() == HTMLMediaElement::HAVE_NOTHING) {
                //  ↳ If the HTMLMediaElement.readyState attribute equals HAVE_NOTHING
                //    Run the "If the media data can be fetched but is found by inspection to be in an unsupported
                //    format, or can otherwise not be rendered at all" steps of the resource fetch algorithm.
                //    NOTE: This step is handled by HTMLMediaElement::mediaLoadingFailed().
                mediaElement.mediaLoadingFailed(MediaPlayer::NetworkState::FormatError);
            } else {
                //  ↳ If the HTMLMediaElement.readyState attribute is greater than HAVE_NOTHING
                //    Run the media data is corrupted steps of the resource fetch algorithm.
                //    NOTE: This step is handled by HTMLMediaElement::mediaLoadingFailedFatally().
                mediaElement.mediaLoadingFailedFatally(MediaPlayer::NetworkState::DecodeError);
            }
        });
    }
}

static ContentType addVP9FullRangeVideoFlagToContentType(const ContentType& type)
{
    auto countPeriods = [] (const String& codec) {
        unsigned count = 0;
        unsigned position = 0;

        while (codec.find('.', position) != notFound) {
            ++count;
            ++position;
        }

        return count;
    };

    for (auto codec : type.codecs()) {
        if (!codec.startsWith("vp09"_s) || countPeriods(codec) != 7)
            continue;

        auto rawType = type.raw();
        auto position = rawType.find(codec);
        ASSERT(position != notFound);
        if (position == notFound)
            continue;

        return ContentType(makeStringByInserting(rawType, ".00"_s, position + codec.length()));
    }
    return type;
}

MediaSource::ReadyState MediaSourceImpl::readyState() const
{
    return accessIvarSync([&] { return m_readyState; });
}

bool MediaSourceImpl::isTypeSupported(const String& type,
                                      bool needsVP9FullRangeFlagQuirk,
                                      const Vector<ContentType>& contentTypesRequiringHardwareSupport,
                                      const std::optional<Vector<String>>& allowedMediaCodecTypes,
                                      const std::optional<Vector<String>>& allowedMediaContainerTypes,
                                      const std::optional<Vector<FourCC>>& allowedMediaVideoCodecIDs,
                                      const std::optional<Vector<FourCC>>& allowedMediaAudioCodecIDs,
                                      const std::optional<Vector<FourCC>>& allowedMediaCaptionFormatTypes)
{
    // Section 2.2 isTypeSupported() method steps.
    // https://dvcs.w3.org/hg/html-media/raw-file/tip/media-source/media-source.html#widl-MediaSource-isTypeSupported-boolean-DOMString-type
    // 1. If type is an empty string, then return false.
    if (type.isNull() || type.isEmpty())
        return false;

    ContentType contentType(type);
    if (needsVP9FullRangeFlagQuirk)
        contentType = addVP9FullRangeVideoFlagToContentType(contentType);

    String codecs = contentType.parameter("codecs"_s);

    // 2. If type does not contain a valid MIME type string, then return false.
    if (contentType.containerType().isEmpty())
        return false;

    if (!contentTypeMeetsContainerAndCodecTypeRequirements(contentType, allowedMediaContainerTypes, allowedMediaCodecTypes))
        return false;

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then return false.
    // 4. If type contains at a codec that the MediaSource does not support, then return false.
    // 5. If the MediaSource does not support the specified combination of media type, media subtype, and codecs then return false.
    // 6. Return true.
    MediaEngineSupportParameters parameters;
    parameters.type = contentType;
    parameters.isMediaSource = true;
    parameters.contentTypesRequiringHardwareSupport = contentTypesRequiringHardwareSupport;
    parameters.allowedMediaContainerTypes = allowedMediaContainerTypes;
    parameters.allowedMediaVideoCodecIDs = allowedMediaVideoCodecIDs;
    parameters.allowedMediaAudioCodecIDs = allowedMediaAudioCodecIDs;
    parameters.allowedMediaCaptionFormatTypes = allowedMediaCaptionFormatTypes;

    MediaPlayer::SupportsType supported = MediaPlayer::supportsType(parameters);

    if (codecs.isEmpty())
        return supported != MediaPlayer::SupportsType::IsNotSupported;

    return supported == MediaPlayer::SupportsType::IsSupported;
}

bool MediaSourceImpl::isOpen() const
{
    return readyState() == ReadyState::Open;
}

bool MediaSourceImpl::isClosed() const
{
    return readyState() == ReadyState::Closed;
}

bool MediaSourceImpl::isEnded() const
{
    return readyState() == ReadyState::Ended;
}

SourceBufferList& MediaSourceImpl::sourceBuffers() const
{
    return *accessIvarSync([&] { return m_sourceBuffers.ptr(); });
}

SourceBufferList& MediaSourceImpl::activeSourceBuffers() const
{
    return *accessIvarSync([&] { return m_activeSourceBuffers.ptr(); });
}

void MediaSourceImpl::detachFromElement(HTMLMediaElement&)
{
    assertIsCurrent(workQueue());

    // 1. Set the readyState attribute to "closed".
    // 7. Queue a task to fire a simple event named sourceclose at the MediaSource.
    setReadyState(ReadyState::Closed);

    m_private = nullptr;
    setMediaElement(nullptr);
}

void MediaSourceImpl::openIfInEndedState()
{
    assertIsCurrent(workQueue());
    if (m_readyState != ReadyState::Ended)
        return;

    ALWAYS_LOG(LOGIDENTIFIER);

    setReadyState(ReadyState::Open);
    m_private->unmarkEndOfStream();

    forEachSourceBuffer([&] (auto& sourceBuffer) {
        sourceBuffer.impl().setMediaSourceEnded(false);
    });
}

void MediaSourceImpl::stop()
{
    assertIsCurrent(workQueue());
    ALWAYS_LOG(LOGIDENTIFIER);

    dispatchMediaElementTask([] (HTMLMediaElement& mediaElement) {
        mediaElement.detachMediaSource();
    });
    m_readyState = ReadyState::Closed;
    m_taskGroup.cancel();
    m_private = nullptr;
}

void MediaSourceImpl::onReadyStateChange(ReadyState oldState, ReadyState newState)
{
    assertIsCurrent(workQueue());
    ALWAYS_LOG(LOGIDENTIFIER, "old state = ", oldState, ", new state = ", newState);

    forEachSourceBuffer([&] (auto& sourceBuffer) {
        sourceBuffer.impl().readyStateChanged();
    });

    if (isOpen()) {
        dispatchContextTask([this] (auto&) {
            scheduleEvent(eventNames().sourceopenEvent);
        });
        return;
    }

    if (oldState == ReadyState::Open && newState == ReadyState::Ended) {
        dispatchContextTask([this] (auto&) {
            scheduleEvent(eventNames().sourceendedEvent);
        });
        return;
    }

    ASSERT(isClosed());
    dispatchContextTask([this] (auto&) {
        scheduleEvent(eventNames().sourcecloseEvent);
    });
}

Vector<PlatformTimeRanges> MediaSourceImpl::activeRanges() const
{
    return WTF::map(activeSourceBuffers(), [](auto& sourceBuffer) {
        return sourceBuffer->impl().bufferedInternal().ranges();
    });
}

ExceptionOr<Ref<SourceBufferPrivate>> MediaSourceImpl::createSourceBufferPrivate(const String& type)
{
    assertIsCurrent(workQueue());
    DEBUG_LOG(LOGIDENTIFIER, type);

    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.isEmpty())
        return Exception { TypeError };

    // 2. If type contains a MIME type that is not supported ..., then throw a
    // NotSupportedError exception and abort these steps.
    if (!isTypeSupported(type,
                         m_needsVP9FullRangeFlagQuirk,
                         m_contentTypesRequiringHardwareSupport,
                         m_allowedMediaCodecTypes,
                         m_allowedMediaContainerTypes,
                         m_allowedMediaVideoCodecIDs,
                         m_allowedMediaAudioCodecIDs,
                         m_allowedMediaCaptionFormatTypes))
        return Exception { NotSupportedError };

    // 4. If the readyState attribute is not in the "open" state then throw an
    // InvalidStateError exception and abort these steps.
    if (!isOpen())
        return Exception { InvalidStateError };

    // 5. Create a new SourceBuffer object and associated resources.
    ContentType contentType(type);
    if (m_needsVP9FullRangeFlagQuirk)
        contentType = addVP9FullRangeVideoFlagToContentType(contentType);

    RefPtr<SourceBufferPrivate> sourceBufferPrivate;
    switch (m_private->addSourceBuffer(contentType, m_webmParserEnabled, sourceBufferPrivate)) {
        case MediaSourcePrivate::AddStatus::Ok:
            break;
        case MediaSourcePrivate::AddStatus::NotSupported:
            // 2.2 https://dvcs.w3.org/hg/html-media/raw-file/default/media-source/media-source.html#widl-MediaSource-addSourceBuffer-SourceBuffer-DOMString-type
            // Step 2: If type contains a MIME type ... that is not supported with the types
            // specified for the other SourceBuffer objects in sourceBuffers, then throw
            // a NotSupportedError exception and abort these steps.
            return Exception { NotSupportedError };
        case MediaSourcePrivate::AddStatus::ReachedIdLimit:
            // 2.2 https://dvcs.w3.org/hg/html-media/raw-file/default/media-source/media-source.html#widl-MediaSource-addSourceBuffer-SourceBuffer-DOMString-type
            // Step 3: If the user agent can't handle any more SourceBuffer objects then throw
            // a QuotaExceededError exception and abort these steps.
            return Exception { QuotaExceededError };
        default:
            ASSERT_NOT_REACHED();
            return Exception { QuotaExceededError };
    }

    return sourceBufferPrivate.releaseNonNull();
}

void MediaSourceImpl::scheduleEvent(const AtomString& eventName)
{
    DEBUG_LOG(LOGIDENTIFIER, "scheduling '", eventName, "'");
    m_mediaSource.queueTaskToDispatchEvent(m_mediaSource, TaskSource::MediaElement, Event::create(eventName, Event::CanBubble::No, Event::IsCancelable::No));
}

void MediaSourceImpl::regenerateActiveSourceBuffers()
{
    assertIsCurrent(workQueue());

    dispatchSourceBufferTaskSync([&] {
        Vector<RefPtr<SourceBuffer>> newList;
        for (auto& sourceBuffer : sourceBuffers()) {
            if (sourceBuffer->impl().active())
                newList.append(sourceBuffer);
        }
        activeSourceBuffers().swap(newList);
        for (auto& sourceBuffer : activeSourceBuffers())
            sourceBuffer->impl().setBufferedDirty(true);
    });
    updateBufferedIfNeeded();
}

void MediaSourceImpl::updateBufferedIfNeeded()
{
    assertIsCurrent(workQueue());
    if (WTF::allOf(activeSourceBuffers(), [](auto& buffer) { return !buffer->impl().isBufferedDirty(); }))
        return;

    auto buffered = makeUniqueRef<PlatformTimeRanges>();
    forEachActiveSourceBuffer([] (auto& sourceBuffer) {
        sourceBuffer.impl().setBufferedDirty(false);
    });

    // Implements MediaSource algorithm for HTMLMediaElement.buffered.
    // https://dvcs.w3.org/hg/html-media/raw-file/default/media-source/media-source.html#htmlmediaelement-extensions
    Vector<PlatformTimeRanges> activeRanges = this->activeRanges();

    // 1. If activeSourceBuffers.length equals 0 then return an empty TimeRanges object and abort these steps.
    if (activeRanges.isEmpty())
        return;

    // 2. Let active ranges be the ranges returned by buffered for each SourceBuffer object in activeSourceBuffers.
    // 3. Let highest end time be the largest range end time in the active ranges.
    MediaTime highestEndTime = MediaTime::zeroTime();
    for (auto& ranges : activeRanges) {
        unsigned length = ranges.length();
        if (length)
            highestEndTime = std::max(highestEndTime, ranges.end(length - 1));
    }

    // Return an empty range if all ranges are empty.
    if (!highestEndTime) {
        setBuffered(WTFMove(buffered));
        return;
    }

    // 4. Let intersection ranges equal a TimeRange object containing a single range from 0 to highest end time.
    buffered->add(MediaTime::zeroTime(), highestEndTime);

    // 5. For each SourceBuffer object in activeSourceBuffers run the following steps:
    bool ended = readyState() == ReadyState::Ended;
    for (auto& sourceRanges : activeRanges) {
        // 5.1 Let source ranges equal the ranges returned by the buffered attribute on the current SourceBuffer.
        // 5.2 If readyState is "ended", then set the end time on the last range in source ranges to highest end time.
        if (ended && sourceRanges.length())
            sourceRanges.add(sourceRanges.start(sourceRanges.length() - 1), highestEndTime);

        // 5.3 Let new intersection ranges equal the intersection between the intersection ranges and the source ranges.
        // 5.4 Replace the ranges in intersection ranges with the new intersection ranges.
        buffered->intersectWith(sourceRanges);
    }

    setBuffered(WTFMove(buffered));

    if (m_private)
        m_private->bufferedChanged(buffered.get());
}

#if !RELEASE_LOG_DISABLED
void MediaSourceImpl::setLogIdentifier(const void* identifier)
{
    m_logIdentifier = identifier;
    ALWAYS_LOG(LOGIDENTIFIER);

    dispatchContextTask([this, identifier] (auto&) {
        m_mediaSource.setLogIdentifier(identifier);
    });
}
#endif

void MediaSourceImpl::failedToCreateRenderer(RendererType type)
{
    assertIsCurrent(workQueue());
    dispatchContextTask([type] (ScriptExecutionContext& context) {
        context.addConsoleMessage(MessageSource::JS, MessageLevel::Error, makeString("MediaSource ", type == RendererType::Video ? "video" : "audio", " renderer creation failed."));
    });
}

void MediaSourceImpl::didReceiveInitializationSegment(bool activeTrackFlag)
{
    assertIsCurrent(workQueue());
    bool anySourceBufferHasNotReceivedInitializationSegment = anySourceBuffer([](auto& sourceBuffer) {
        return !sourceBuffer.impl().receivedFirstInitializationSegment();
    });

    // 6. If the HTMLMediaElement.readyState attribute is HAVE_NOTHING, then run the following steps:
    if (m_private->readyState() == MediaPlayer::ReadyState::HaveNothing) {
        // 6.1 If one or more objects in sourceBuffers have first initialization segment flag set to false, then abort these steps.
        if (anySourceBufferHasNotReceivedInitializationSegment)
            return;

        // 6.2 Set the HTMLMediaElement.readyState attribute to HAVE_METADATA.
        // 6.3 Queue a task to fire a simple event named loadedmetadata at the media element.
        m_private->setReadyState(MediaPlayer::ReadyState::HaveMetadata);
    }

    // 7. If the active track flag equals true and the HTMLMediaElement.readyState
    // attribute is greater than HAVE_CURRENT_DATA, then set the HTMLMediaElement.readyState
    // attribute to HAVE_METADATA.
    if (activeTrackFlag && m_private->readyState() > MediaPlayer::ReadyState::HaveCurrentData)
        m_private->setReadyState(MediaPlayer::ReadyState::HaveMetadata);
}

void MediaSourceImpl::forEachSourceBuffer(Function<void(SourceBuffer&)>&& task)
{
    m_sourceBufferWorkQueue->dispatchSync([&] () mutable {
        for (auto& sourceBuffer : m_sourceBuffers.get()) {
            if (sourceBuffer)
                task(*sourceBuffer);
        }
    });
}

void MediaSourceImpl::forEachActiveSourceBuffer(Function<void(SourceBuffer&)>&& task)
{
    m_sourceBufferWorkQueue->dispatchSync([&] () mutable {
        for (auto& sourceBuffer : m_activeSourceBuffers.get()) {
            if (sourceBuffer)
                task(*sourceBuffer);
        }
    });
}

bool MediaSourceImpl::anySourceBuffer(Function<bool(SourceBuffer&)>&& task)
{
    bool returnValue;
    m_sourceBufferWorkQueue->dispatchSync([&] () mutable {
        returnValue = WTF::anyOf(m_sourceBuffers.get(), [task = WTFMove(task)] (auto& sourceBuffer) {
            return sourceBuffer && task(*sourceBuffer);
        });
    });
    return returnValue;
}

bool MediaSourceImpl::everyActiveSourceBuffer(Function<bool(SourceBuffer&)>&& task)
{
    bool returnValue;
    m_sourceBufferWorkQueue->dispatchSync([&] () mutable {
        returnValue = WTF::allOf(m_sourceBuffers.get(), [task = WTFMove(task)] (auto& sourceBuffer) {
            return sourceBuffer && task(*sourceBuffer);
        });
    });
    return returnValue;
}

void MediaSourceImpl::dispatchSourceBufferTaskSync(Function<void()>&& task)
{
    m_sourceBufferWorkQueue->dispatchSync([&] {
        task();
    });
}

void MediaSourceImpl::dispatchMediaElementTask(Function<void(HTMLMediaElement&)>&& task)
{
    callOnMainThread(CancellableTask(m_taskGroup, [mediaElement = m_mediaElement, task = WTFMove(task)] {
        if (mediaElement)
            task(*mediaElement);
    }));
}

void MediaSourceImpl::dispatchContextTask(Function<void(ScriptExecutionContext&)>&& task)
{
    if (auto* context = m_mediaSource.scriptExecutionContext()) {
        auto cancellableTask = CancellableTask(m_taskGroup, [context, task = WTFMove(task)] {
            task(*context);
        });
        context->postTask([cancellableTask = WTFMove(cancellableTask)] (auto&) mutable {
            cancellableTask();
        });
    }
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& MediaSourceImpl::logChannel() const
{
    return LogMediaSource;
}
#endif

}

#endif
