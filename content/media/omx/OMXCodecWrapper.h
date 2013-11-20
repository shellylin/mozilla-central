/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef OMXCodecWrapper_h_
#define OMXCodecWrapper_h_

#include <gui/Surface.h>
#include <stagefright/foundation/AMessage.h>
#include <stagefright/foundation/ABuffer.h>
#include <stagefright/MediaCodec.h>

#include "AudioSampleFormat.h"
#include "GonkNativeWindow.h"
#include "GonkNativeWindowClient.h"

namespace android {

/**
 * AVC/H.264 and AAC encoder using MediaCodec API in libstagefright.
 */
class OMXCodecWrapper
{
public:
  // Flags for constructor.
  enum {
    TYPE_VIDEO = 1 << 0,
    TYPE_AUDIO = 1 << 1,
  };

  // Output flags for GetNextEncodedFrame().
  enum {
    // Output buffer for the last frame at the end of stream.
    BUFFER_EOS = MediaCodec::BUFFER_FLAG_EOS,
    // Output buffer is I-frame.
    BUFFER_SYNC_FRAME = MediaCodec::BUFFER_FLAG_SYNCFRAME,
    // Output buffer is not an frame but codec specific data.
    BUFFER_CODEC_CONFIG = MediaCodec::BUFFER_FLAG_CODECCONFIG,
  };
  // Hard-coded values for AAC DecoderConfigDescriptor in libstagefright.
  // See MPEG4Writer::Track::writeMp4aEsdsBox()
  // Exposed for the need of MP4 container writer.
  const static uint32_t kAACBitrate = 96000; // kbps
  const static uint32_t kAACFrameSize = 768; // bytes

  OMXCodecWrapper(const int aFlags);

  virtual ~OMXCodecWrapper();

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(OMXCodecWrapper)

  /**
   * Starts the MediaCodec.
   */
  nsresult Start();

  /**
   * Stops the MediaCodec.
   */
  nsresult Stop();

  /**
   * Configure video codec parameters.
   */
  nsresult ConfigureVideo(int aWidth, int aHeight, int aFrameRate);

  /**
   * Configure audio codec parameters.
   */
  nsresult ConfigureAudio(int aChannelCount, int aSampleRate);

  /**
   * Encode a video frame with type of GrallocImage.
   */
  void EncodeVideoFrame(const mozilla::layers::GrallocImage& aImage,
                        int64_t aDuration, int aInputFlags = 0);

  /**
   * Encode a video frame with type of nsTArray<uint8_t>.
   */
  void EncodeVideoFrame(const nsTArray<uint8_t>& aImage,
                        int64_t aDuration, int aInputFlags = 0);

  /**
   * Encode audio samples.
   */
  void EncodeAudioSamples(const nsTArray<mozilla::AudioDataValue>& aSamples,
                          int64_t aDuration, int aInputFlags = 0);

  /**
   * Get the next available encoded data from MediaCodec. The data will be
   * copied into aOutputBuf array, with its duration (in milliseconds) in
   * aOutputDuration.
   * Wait at most aTimeout microseconds to dequeue a outpout buffer.
   */
  void GetNextEncodedFrame(nsTArray<uint8_t>* aOutputBuf,
                           int64_t* aOutputDuration, int* aOutputFlags,
                           int64_t aTimeOut);

private:

  /**
   * Push a buffer of raw audio or video data into the input buffers of
   * MediaCodec. aData is the pointer of buffer, with buffer size of aSize.
   * Wait at most aTimeout microseconds to dequeue a buffer out from the input
   * buffers.
   */
  status_t PushInput(const void* aData, size_t aSize, int64_t aDuration,
                     int aInputFlags, int64_t aTimeOut);

  sp<MediaCodec> mCodec;
  sp<ALooper> mLooper; // A looper/thread for MediaCodec

  Vector<sp<ABuffer> > mInputBufs; // MediaCodec input buffers.
  Vector<sp<ABuffer> > mOutputBufs; // MediaCodec output buffers.

  int mFlags; // Video or audio?
  bool mStarted; // Has MediaCodec been started?

  // MediaCodec accepts and returns only timestamp values in microseconds.
  // Timestamp at the end of latest input. Derived from input duration.
  int64_t mInputTimestamp;
  int64_t mOutputTimestamp; // Timestamp at the end of latest output.
};

}
#endif
