/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "OMXCodecWrapper.h"

#include <binder/ProcessState.h>
#include <media/ICrypto.h>
#include <media/IOMX.h>
#include <OMX_Component.h>
#include <stagefright/MediaDefs.h>
#include <stagefright/MediaErrors.h>

using namespace mozilla;
using namespace mozilla::layers;

namespace android {

#define ENCODER_CONFIG_BITRATE 200000 // bps
// How many seconds per I-frame.
#define ENCODER_CONFIG_I_FRAME_INTERVAL 1
// Wait up to 5ms for input buffer.
#define INPUT_BUFFER_TIMEOUT_US (5 * 1000ll)

// MediaCodec needs a Android Binder thread pool.
static bool sThreadPoolStarted = false;

OMXCodecWrapper::OMXCodecWrapper(const int aFlags)
  : mStarted(false)
  , mInputTimestamp(0)
  , mOutputTimestamp(0)
{
  mFlags = aFlags;

  // Starts the thread pool once only.
  if (!sThreadPoolStarted) {
    ProcessState::self()->startThreadPool();
    sThreadPoolStarted = true;
  }

  mLooper = new ALooper();
  mLooper->start();

  if (mFlags & TYPE_VIDEO) {
    mCodec = MediaCodec::CreateByType(mLooper, MEDIA_MIMETYPE_VIDEO_AVC, true);
  } else if (mFlags & TYPE_AUDIO) {
    mCodec = MediaCodec::CreateByType(mLooper, MEDIA_MIMETYPE_AUDIO_AAC, true);
  }
}

OMXCodecWrapper::~OMXCodecWrapper()
{
  Stop();
  mCodec->release();
  mLooper->stop();
}

// Common
nsresult
OMXCodecWrapper::Start()
{
  NS_ENSURE_TRUE(!mStarted, NS_OK);

  status_t err = mCodec->start();
  mStarted = (err == OK);

  NS_ENSURE_TRUE(err == OK, NS_ERROR_FAILURE);

  mCodec->getInputBuffers(&mInputBufs);
  mCodec->getOutputBuffers(&mOutputBufs);

  return err == OK ? NS_OK : NS_ERROR_FAILURE;
}

nsresult
OMXCodecWrapper::Stop()
{
  NS_ENSURE_TRUE(mStarted, NS_OK);

  status_t err = mCodec->stop();
  mStarted = !(err == OK);

  return err == OK ? NS_OK : NS_ERROR_FAILURE;
}

nsresult
OMXCodecWrapper::ConfigureVideo(int aWidth, int aHeight, int aFrameRate)
{
  NS_ENSURE_TRUE(mCodec != nullptr && mFlags & TYPE_VIDEO, NS_ERROR_FAILURE);

  // Set up configuration parameters for AVC/H.264 encoder.
  sp<AMessage> format = new AMessage;
  // Fixed values
  format->setString("mime", MEDIA_MIMETYPE_VIDEO_AVC);
  format->setInt32("bitrate", ENCODER_CONFIG_BITRATE);
  format->setInt32("i-frame-interval", ENCODER_CONFIG_I_FRAME_INTERVAL);
  // See mozilla::layers::GrallocImage, supports YUV 4:2:0, CbCr width and
  // height is half that of Y
  format->setInt32("color-format", OMX_COLOR_FormatYUV420SemiPlanar);
  format->setInt32("profile", OMX_VIDEO_AVCProfileBaseline);
  format->setInt32("level", OMX_VIDEO_AVCLevel3);
  format->setInt32("bitrate-mode", OMX_Video_ControlRateConstant);
  format->setInt32("store-metadata-in-buffers", 0);
  format->setInt32("prepend-sps-pps-to-idr-frames", 0);
  // Input values.
  format->setInt32("width", aWidth);
  format->setInt32("height", aHeight);
  format->setInt32("stride", aWidth);
  format->setInt32("slice-height", aHeight);
  format->setInt32("frame-rate", aFrameRate);

  status_t err = mCodec->configure(format, nullptr, nullptr,
                                   MediaCodec::CONFIGURE_FLAG_ENCODE);

  return err == OK ? NS_OK : NS_ERROR_FAILURE;
}

nsresult
OMXCodecWrapper::ConfigureAudio(int aChannels, int aSamplingRate)
{
  NS_ENSURE_TRUE(mCodec != nullptr && mFlags & TYPE_AUDIO, NS_ERROR_FAILURE);

  // Set up configuration parameters for AAC encoder.
  sp<AMessage> format = new AMessage;
  // Fixed values.
  format->setString("mime", MEDIA_MIMETYPE_AUDIO_AAC);
  format->setInt32("bitrate", kAACBitrate);
  format->setInt32("profile", OMX_AUDIO_AACObjectLC);
  // Input values.
  format->setInt32("channel-count", aChannels);
  format->setInt32("sample-rate", aSamplingRate);

  status_t err = mCodec->configure(format, nullptr, nullptr,
                                   MediaCodec::CONFIGURE_FLAG_ENCODE);

  return err == OK ? NS_OK : NS_ERROR_FAILURE;
}

void
OMXCodecWrapper::EncodeVideoFrame(const GrallocImage& aImage, int64_t aDuration,
                                  int aInputFlags)
{
  GrallocImage* nativeImage = const_cast<GrallocImage*>(&aImage);
  SurfaceDescriptor handle = nativeImage->GetSurfaceDescriptor();
  SurfaceDescriptorGralloc grallocHandle = handle.get_SurfaceDescriptorGralloc();
  sp<GraphicBuffer> graphicBuffer = GrallocBufferActor::GetFrom(grallocHandle);

  // Size of PLANAR_YCBCR 4:2:0.
  uint32_t frameLen = graphicBuffer->getWidth()*graphicBuffer->getHeight()*3/2;
  //
  void *imgPtr;
  graphicBuffer->lock(GraphicBuffer::USAGE_SW_READ_MASK, &imgPtr);
  PushInput(imgPtr, frameLen, aDuration, aInputFlags,
            INPUT_BUFFER_TIMEOUT_US);
  graphicBuffer->unlock();
  mInputTimestamp  += aDuration;
}

void
OMXCodecWrapper::EncodeVideoFrame(const nsTArray<uint8_t>& aImage,
                                  int64_t aDuration, int aInputFlags)
{
  PushInput(aImage.Elements(), aImage.Length(), aDuration, aInputFlags,
            INPUT_BUFFER_TIMEOUT_US);
  mInputTimestamp  += aDuration;
}

void
OMXCodecWrapper::EncodeAudioSamples(const nsTArray<AudioDataValue>& aSamples,
                                    int64_t aDuration, int aInputFlags)
{
// MediaCodec accepts only 16-bit PCM data.
#ifndef MOZ_SAMPLE_TYPE_S16
  MOZ_ASSERT(false);
#endif

  PushInput(aSamples.Elements(), aSamples.Length() * sizeof(int16_t),
            mInputTimestamp, aInputFlags, INPUT_BUFFER_TIMEOUT_US);
  mInputTimestamp  += aDuration;
}

status_t
OMXCodecWrapper::PushInput(const void* aData, size_t aSize, int64_t aDuration,
                           int aInputFlags, int64_t aTimeOut)
{
  MOZ_ASSERT(aData != nullptr);

  status_t err;
  size_t copied = 0;
  int64_t timeOffset = 0;
  do {
    // Dequeue an input buffer.
    uint32_t index;
    while (true) {
      err = mCodec->dequeueInputBuffer(&index, aTimeOut);
      if (err != -EAGAIN) {
        break;
      }
    }

    if (err != OK) {
      break;
    }
    const sp<ABuffer>& inBuf = mInputBufs.itemAt(index);
    size_t bufferSize = inBuf->capacity();

    size_t remain = aSize - copied;
    size_t toCopy = remain > bufferSize? bufferSize:remain;
    inBuf->setRange(0, toCopy);

    // Copy data to this input buffer.
    memcpy(inBuf->data(), aData + copied, toCopy);
    copied += toCopy;
    timeOffset = aDuration * copied / aSize;

    int inFlags = aInputFlags;
    // Don't signal EOS if there is still data to copy.
    if (copied < aSize) {
      inFlags &= ~BUFFER_EOS;
    }
    // Queue this input buffer.
    err = mCodec->queueInputBuffer(index, 0, toCopy,
                                   mInputTimestamp + timeOffset, inFlags);
    if (err != OK) {
      break;
    }
  } while (copied < aSize);

  return err;
}

void
OMXCodecWrapper::GetNextEncodedFrame(nsTArray<uint8_t>* aOutputBuf,
                                     int64_t* aOutputDuration,
                                     int* aOutputFlags, int64_t aTimeOut)
{
  // Dequeue a buffer from output buffers.
  size_t index;
  size_t outOffset;
  size_t outSize;
  int64_t outTimeUs;
  uint32_t outFlags;
  bool retry = false;
  do {
    status_t err = mCodec->dequeueOutputBuffer(&index, &outOffset, &outSize,
                           &outTimeUs, &outFlags, aTimeOut);
    switch (err) {
      case OK:
        break;
      case INFO_OUTPUT_BUFFERS_CHANGED:
        // Update our references to output buffers.
        err = mCodec->getOutputBuffers(&mOutputBufs);
        // Get output from a new buffer.
        retry = true;
        break;
      case INFO_FORMAT_CHANGED:
      default:
        return;
    }
  } while (retry);

  if (aOutputBuf) {
    sp<ABuffer> outBuf = mOutputBufs.itemAt(index);
    aOutputBuf->SetLength(outSize);
    memcpy(aOutputBuf->Elements(), outBuf->data(), outSize);
  }
  mCodec->releaseOutputBuffer(index);

  if (aOutputDuration) {
    *aOutputDuration = outTimeUs - mOutputTimestamp;
  }
  mOutputTimestamp = outTimeUs;

  if (aOutputFlags) {
    *aOutputFlags = outFlags;
  }
}

}
