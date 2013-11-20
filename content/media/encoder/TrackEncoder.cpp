/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "TrackEncoder.h"
#include "AudioChannelFormat.h"
#include "MediaStreamGraph.h"
#include "VideoUtils.h"

#undef LOG
#ifdef MOZ_WIDGET_GONK
#include <android/log.h>
#define LOG(args...) __android_log_print(ANDROID_LOG_INFO, "MediaEncoder", ## args);
#else
#define LOG(args, ...)
#endif

namespace mozilla {

static const int DEFAULT_CHANNELS = 1;
static const int DEFAULT_SAMPLING_RATE = 16000;
static const int DEFAULT_FRAME_WIDTH = 640;
static const int DEFAULT_FRAME_HEIGHT = 480;
static const int DEFAULT_TRACK_RATE = USECS_PER_S;

void
AudioTrackEncoder::NotifyQueuedTrackChanges(MediaStreamGraph* aGraph,
                                            TrackID aID,
                                            TrackRate aTrackRate,
                                            TrackTicks aTrackOffset,
                                            uint32_t aTrackEvents,
                                            const MediaSegment& aQueuedMedia)
{
  if (mCanceled) {
    return;
  }

  const AudioSegment audio = static_cast<const AudioSegment&>(aQueuedMedia);

  // Check and initialize parameters for codec encoder.
  if (!mInitialized) {
    AudioSegment::ChunkIterator iter(const_cast<AudioSegment&>(audio));
    while (!iter.IsEnded()) {
      AudioChunk chunk = *iter;

      // The number of channels is determined by the first non-null chunk, and
      // thus the audio encoder is initialized at this time.
      if (!chunk.IsNull()) {
        nsresult rv = Init(chunk.mChannelData.Length(), aTrackRate);
        if (NS_FAILED(rv)) {
          LOG("[AudioTrackEncoder]: Fail to initialize the encoder!");
          NotifyCancel();
        }
        break;
      } else {
        mSilentDuration += chunk.mDuration;
      }
      iter.Next();
    }
  }

  // Append and consume this raw segment.
  if (mInitialized) {
    AppendAudioSegment(audio);
  }

  // The stream has stopped and reached the end of track.
  if (aTrackEvents == MediaStreamListener::TRACK_EVENT_ENDED) {
    LOG("[AudioTrackEncoder]: Receive TRACK_EVENT_ENDED .");
    NotifyEndOfStream();
  }
}

void
AudioTrackEncoder::NotifyEndOfStream()
{
  // If source audio chunks are completely silent till the end of encoding,
  // initialize the encoder with default channel counts and sampling rate, and
  // append this many null data to the segment of track encoder.
  if (!mCanceled && !mInitialized) {
    Init(DEFAULT_CHANNELS, DEFAULT_SAMPLING_RATE);
    mRawSegment->AppendNullData(mSilentDuration);
    mSilentDuration = 0;
  }

  ReentrantMonitorAutoEnter mon(mReentrantMonitor);
  mEndOfStream = true;
  mReentrantMonitor.NotifyAll();
}

nsresult
AudioTrackEncoder::AppendAudioSegment(const AudioSegment& aSegment)
{
  ReentrantMonitorAutoEnter mon(mReentrantMonitor);

  // Append this many null data to our queued segment if there is a complete
  // silence before the audio track encoder has initialized.
  if (mSilentDuration > 0) {
    mRawSegment->AppendNullData(mSilentDuration);
    mSilentDuration = 0;
  }

  AudioSegment::ChunkIterator iter(const_cast<AudioSegment&>(aSegment));
  while (!iter.IsEnded()) {
    AudioChunk chunk = *iter;
    // Append and consume both non-null and null chunks.
    mRawSegment->AppendAndConsumeChunk(&chunk);
    iter.Next();
  }

  if (mRawSegment->GetDuration() >= GetPacketDuration()) {
    mReentrantMonitor.NotifyAll();
  }

  return NS_OK;
}

static const int AUDIO_PROCESSING_FRAMES = 640; /* > 10ms of 48KHz audio */
static const uint8_t gZeroChannel[MAX_AUDIO_SAMPLE_SIZE*AUDIO_PROCESSING_FRAMES] = {0};

void
AudioTrackEncoder::InterleaveTrackData(AudioChunk& aChunk,
                                       int32_t aDuration,
                                       uint32_t aOutputChannels,
                                       AudioDataValue* aOutput)
{
  if (aChunk.mChannelData.Length() < aOutputChannels) {
    // Up-mix. This might make the mChannelData have more than aChannels.
    AudioChannelsUpMix(&aChunk.mChannelData, aOutputChannels, gZeroChannel);
  }

  if (aChunk.mChannelData.Length() > aOutputChannels) {
    DownmixAndInterleave(aChunk.mChannelData, aChunk.mBufferFormat, aDuration,
                         aChunk.mVolume, mChannels, aOutput);
  } else {
    InterleaveAndConvertBuffer(aChunk.mChannelData.Elements(),
                               aChunk.mBufferFormat, aDuration, aChunk.mVolume,
                               mChannels, aOutput);
  }
}

void
VideoTrackEncoder::NotifyQueuedTrackChanges(MediaStreamGraph* aGraph,
                                            TrackID aID,
                                            TrackRate aTrackRate,
                                            TrackTicks aTrackOffset,
                                            uint32_t aTrackEvents,
                                            const MediaSegment& aQueuedMedia)
{
  if (mCanceled) {
    return;
  }

  const VideoSegment video = static_cast<const VideoSegment&>(aQueuedMedia);

   // Check and initialize parameters for codec encoder.
  if (!mInitialized) {
    VideoSegment::ChunkIterator iter(const_cast<VideoSegment&>(video));
    while (!iter.IsEnded()) {
      VideoChunk chunk = *iter;
      if (!chunk.IsNull()) {
        gfxIntSize imgsize = chunk.mFrame.GetImage()->GetSize();
        int width = (imgsize.width + 1) / 2 * 2;
        int height = (imgsize.height + 1) / 2 * 2;
        nsresult rv = Init(width, height, aTrackRate);
        if (NS_FAILED(rv)) {
          LOG("[VideoTrackEncoder]: Fail to initialize the encoder!");
          NotifyCancel();
        }
        break;
      } else {
        mSilentDuration += chunk.mDuration;
      }
      iter.Next();
    }
  }

  if (mInitialized) {
    AppendVideoSegment(video);
  }

  // The stream has stopped and reached the end of track.
  if (aTrackEvents == MediaStreamListener::TRACK_EVENT_ENDED) {
    LOG("[VideoTrackEncoder]: Receive TRACK_EVENT_ENDED .");
    NotifyEndOfStream();
  }

}

nsresult
VideoTrackEncoder::AppendVideoSegment(const VideoSegment& aSegment)
{
  ReentrantMonitorAutoEnter mon(mReentrantMonitor);

  // Append this many null data to our queued segment if there is a completely
  // muted video before receiving the first non-null video chunk.
  if (mSilentDuration > 0) {
    mRawSegment->AppendNullData(mSilentDuration);
    mSilentDuration = 0;
  }

  // Append all video segments from MediaStreamGraph, including null an
  // non-null frames.
  VideoSegment::ChunkIterator iter(const_cast<VideoSegment&>(aSegment));
  while (!iter.IsEnded()) {
    VideoChunk chunk = *iter;

    if (!mFirstFrameTaken) {
      mFirstFrameTaken = true;
      mLastChunk->mFrame.TakeFrom(&chunk.mFrame);
      mLastChunk->mFrame.SetForceBlack(chunk.mFrame.GetForceBlack());
      mLastChunk->mDuration = chunk.GetDuration();
    } else {
      if (mLastChunk->CanCombineWithFollowing(chunk)) {
        mLastChunk->mDuration += chunk.GetDuration();
      } else {
        nsRefPtr<layers::Image> lastImg = mLastChunk->mFrame.GetImage();
        mRawSegment->AppendFrame(lastImg.forget(), mLastChunk->GetDuration(),
                                 mLastChunk->mFrame.GetIntrinsicSize());
        mLastChunk->mDuration = chunk.GetDuration();
      }

      mLastChunk->mFrame.TakeFrom(&chunk.mFrame);
      mLastChunk->mFrame.SetForceBlack(chunk.mFrame.GetForceBlack());
    }
    iter.Next();
  }

  if (mRawSegment->GetDuration() > 0) {
    mReentrantMonitor.NotifyAll();
  }

  return NS_OK;
}

void
VideoTrackEncoder::NotifyEndOfStream()
{
  ReentrantMonitorAutoEnter mon(mReentrantMonitor);
  NS_ENSURE_TRUE_VOID(!mEndOfStream);

  mEndOfStream = true;

  // If source video chunks are muted till the end of encoding, initialize the
  // encoder with default frame width, frame height, track rate, and append this
  // many null data to mRawSegment.
  if (!mCanceled && !mInitialized) {
    Init(DEFAULT_FRAME_WIDTH, DEFAULT_FRAME_HEIGHT, DEFAULT_TRACK_RATE);
    mRawSegment->AppendNullData(mSilentDuration);
    mSilentDuration = 0;
  }

  // Append the last unique frame to mRawSegment.
  if (mLastChunk->GetDuration()) {
    nsRefPtr<layers::Image> lastImg = mLastChunk->mFrame.GetImage();
    mRawSegment->AppendFrame(lastImg.forget(), mLastChunk->GetDuration(),
                             mLastChunk->mFrame.GetIntrinsicSize());
    mLastChunk->mDuration = 0;
  }

  mReentrantMonitor.NotifyAll();
}

void
VideoTrackEncoder::CreateMutedFrame(nsTArray<uint8_t>* aOutputBuffer)
{
  // Supports YUV420 image format only, for now.
  int yPlaneLen = mFrameWidth * mFrameHeight;
  int cbcrPlaneLen = yPlaneLen / 2;
  int frameLen = yPlaneLen + cbcrPlaneLen;

  aOutputBuffer->SetLength(frameLen);
  // Fill Y plane.
  memset(aOutputBuffer->Elements(), 0x10, yPlaneLen);
  // Fill Cb/Cr planes.
  memset(aOutputBuffer->Elements() + yPlaneLen, 0x80, cbcrPlaneLen);
}

}
