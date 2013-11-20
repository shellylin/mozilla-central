/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TrackEncoder_h_
#define TrackEncoder_h_

#include "mozilla/ReentrantMonitor.h"

#include "AudioSegment.h"
#include "EncodedFrameContainer.h"
#include "StreamBuffer.h"
#include "TrackMetadataBase.h"
#include "VideoSegment.h"


namespace mozilla {

class MediaStreamGraph;

/**
 * Base class of AudioTrackEncoder and VideoTrackEncoder. Lifetimes managed by
 * MediaEncoder. Most methods can only be called on the MediaEncoder's thread,
 * but some subclass methods can be called on other threads when noted.
 *
 * NotifyQueuedTrackChanges is called on subclasses of this class from the
 * MediaStreamGraph thread, and AppendAudioSegment/AppendVideoSegment is then
 * called to store media data in the TrackEncoder. Later on, GetEncodedTrack is
 * called on MediaEncoder's thread to encode and retrieve the encoded data.
 */
class TrackEncoder
{
public:
  TrackEncoder()
    : mInitialized(false)
    , mEncodingComplete(false)
    , mSilentDuration(0)
    , mEndOfStream(false)
    , mCanceled(false)
  {}

  virtual ~TrackEncoder() {}

  /**
   * Notified by the same callbcak of MediaEncoder when it has received a track
   * change from MediaStreamGraph. Called on the MediaStreamGraph thread.
   */
  virtual void NotifyQueuedTrackChanges(MediaStreamGraph* aGraph, TrackID aID,
                                        TrackRate aTrackRate,
                                        TrackTicks aTrackOffset,
                                        uint32_t aTrackEvents,
                                        const MediaSegment& aQueuedMedia) = 0;

  /**
   * Notified by the same callback of MediaEncoder when it has been removed from
   * MediaStreamGraph. Called on the MediaStreamGraph thread.
   */
  void NotifyRemoved(MediaStreamGraph* aGraph) { NotifyEndOfStream(); }

  /**
   * Creates and sets up meta data for a specific codec
   */
  virtual already_AddRefed<TrackMetadataBase> GetMetadata() = 0;

  /**
   * Encodes raw segments. Result data is returned in aData.
   */
  virtual nsresult GetEncodedTrack(EncodedFrameContainer& aData) = 0;

  bool IsEncodingComplete() { return mEncodingComplete; }

  protected:
  /**
   * Notifies the audio encoder that we have reached the end of source stream,
   * and wakes up mReentrantMonitor if encoder is waiting for more track data.
   */
  virtual void NotifyEndOfStream() = 0;

  bool mInitialized;
  bool mEncodingComplete;

  /**
   * The total duration of null chunks we have received from MediaStreamGraph
   * before initializing the track encoder.
   */
  TrackTicks mSilentDuration;

  /**
   * True if we have received an event of TRACK_EVENT_ENDED from MediaStreamGraph,
   * or the MediaEncoder is removed from its source stream, protected by
   * mReentrantMonitor.
   */
  bool mEndOfStream;

  /**
   * True if a cancellation of encoding is sent from MediaEncoder, protected by
   * mReentrantMonitor.
   */
  bool mCanceled;
};

class AudioTrackEncoder : public TrackEncoder
{
public:
  AudioTrackEncoder()
    : TrackEncoder()
    , mChannels(0)
    , mSamplingRate(0)
    , mReentrantMonitor("media.AudioEncoder")
    , mRawSegment(new AudioSegment())
  {}

  void NotifyQueuedTrackChanges(MediaStreamGraph* aGraph, TrackID aID,
                                TrackRate aTrackRate,
                                TrackTicks aTrackOffset,
                                uint32_t aTrackEvents,
                                const MediaSegment& aQueuedMedia) MOZ_OVERRIDE;

  /**
   * Notifies from MediaEncoder to cancel the encoding, and wakes up
   * mReentrantMonitor if encoder is waiting on it.
   */
  void NotifyCancel()
  {
    ReentrantMonitorAutoEnter mon(mReentrantMonitor);
    mCanceled = true;
    mReentrantMonitor.NotifyAll();
  }

protected:
  /**
   * Number of samples per channel in a pcm buffer. This is also the value of
   * frame size required by audio encoder, and mReentrantMonitor will be
   * notified when at least this much data has been added to mRawSegment.
   */
  virtual int GetPacketDuration() { return 0; }

  /**
   * Initializes the audio encoder. The call of this method is delayed until we
   * have received the first valid track from MediaStreamGraph, and the
   * mReentrantMonitor will be notified if other methods is waiting for encoder
   * to be completely initialized. This method is called on the MediaStreamGraph
   * thread.
   */
  virtual nsresult Init(int aChannels, int aSamplingRate) = 0;

  /**
   * Appends and consumes track data from aSegment, this method is called on
   * the MediaStreamGraph thread. mReentrantMonitor will be notified when at
   * least GetPacketDuration() data has been added to mRawSegment, wake up other
   * method which is waiting for more data from mRawSegment.
   */
  nsresult AppendAudioSegment(const AudioSegment& aSegment);

  /**
   * Notifies the audio encoder that we have reached the end of source stream,
   * and wakes up mReentrantMonitor if encoder is waiting for more track data.
   */
  void NotifyEndOfStream() MOZ_OVERRIDE;

  /**
   * Interleaves the track data and stores the result into aOutput. Might need
   * to up-mix or down-mix the channel data if the channels number of this chunk
   * is different from mChannels. The channel data from aChunk might be modified
   * by up-mixing.
   */
  void InterleaveTrackData(AudioChunk& aChunk, int32_t aDuration,
                           uint32_t aOutputChannels, AudioDataValue* aOutput);

  /**
   * The number of channels in the first valid audio chunk, and is being used
   * to initialize the audio encoder.
   */
  int mChannels;
  int mSamplingRate;

  /**
   * A ReentrantMonitor to protect the pushing and pulling of mRawSegment, and
   * some other flags.
   */
  ReentrantMonitor mReentrantMonitor;

  /**
   * A segment queue of audio track data, protected by mReentrantMonitor.
   */
  nsAutoPtr<AudioSegment> mRawSegment;

};

class VideoTrackEncoder : public TrackEncoder
{
public:
  VideoTrackEncoder()
    : TrackEncoder()
    , mFrameWidth(0)
    , mFrameHeight(0)
    , mTrackRate(0)
    , mLastChunk(new VideoChunk())
    , mFirstFrameTaken(false)
    , mReentrantMonitor("media.VideoEncoder")
    , mRawSegment(new VideoSegment())
  {}

  void NotifyQueuedTrackChanges(MediaStreamGraph* aGraph, TrackID aID,
                                TrackRate aTrackRate,
                                TrackTicks aTrackOffset,
                                uint32_t aTrackEvents,
                                const MediaSegment& aQueuedMedia) MOZ_OVERRIDE;


  // Notifies from MediaEncoder to cancel the encoding, and wakes up
  // mReentrantMonitor if encoder is waiting on it.
  void NotifyCancel()
  {
    ReentrantMonitorAutoEnter mon(mReentrantMonitor);
    mCanceled = true;
    mReentrantMonitor.NotifyAll();
  }

protected:
  // Initialized the video encoder. In order to collect the value of width and
  // height of source frames, this initialization is delayed until we have
  // received the first valid video frame from MediaStreamGraph;
  // mReentrantMonitor will be notified after it has successfully initialized,
  // and this method is called on the MediaStramGraph thread.
  virtual nsresult Init(int aWidth, int aHeight, TrackRate aTrackRate) = 0;

  // Appends source video frames to mRawSegment. mLastChunk takes every frame
  // from the source, sums up the duration of duplicated frames, and append
  // itself to mRawSegment before it takes the next unique frame.
  nsresult AppendVideoSegment(const VideoSegment& aSegment);

  // Tells the video track encoder that we've reached the end of source stream,
  // and wakes up mReentrantMonitor if encoder is waiting for more track data.
  void NotifyEndOfStream() MOZ_OVERRIDE;

  // Create a buffer of black image in format of YUV:420.
  void CreateMutedFrame(nsTArray<uint8_t>* aOutputBuffer);

  int mFrameWidth;
  int mFrameHeight;
  TrackRate mTrackRate;

  // Takes every frame from the source segment, sums up the duration of
  // duplicated ones, and append itself to the mRawSegment before it takes the
  // next unique frame.
  nsAutoPtr<VideoChunk> mLastChunk;

  // True if mLastChunk has taken the first valid video frame from source
  // segment.
  bool mFirstFrameTaken;

  // A ReentrantMonitor to protect the pushing and pulling of mRawSegment, and
  // some other flags.
  ReentrantMonitor mReentrantMonitor;

  // A segment queue of audio track data, protected by mReentrantMonitor.
  nsAutoPtr<VideoSegment> mRawSegment;
};

}
#endif
