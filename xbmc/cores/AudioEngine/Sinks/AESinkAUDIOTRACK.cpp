 /*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AESinkAUDIOTRACK.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "platform/android/activity/XBMCApp.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"

#include "platform/android/jni/AudioFormat.h"
#include "platform/android/jni/AudioManager.h"
#include "platform/android/jni/AudioTrack.h"
#include "platform/android/jni/Build.h"

#if defined(HAS_LIBAMCODEC)
#include "utils/AMLUtils.h"
#endif

//#define DEBUG_VERBOSE 1

using namespace jni;

// those are empirical values while the HD buffer
// is the max TrueHD package
const unsigned int MAX_RAW_AUDIO_BUFFER_HD = 61440;
const unsigned int MAX_RAW_AUDIO_BUFFER = 16384;
const unsigned int NUM_RAW_PACKAGES = 8;

/*
 * ADT-1 on L preview as of 2014-10 downmixes all non-5.1/7.1 content
 * to stereo, so use 7.1 or 5.1 for all multichannel content for now to
 * avoid that (except passthrough).
 * If other devices surface that support other multichannel layouts,
 * this should be disabled or adapted accordingly.
 */
#define LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1 1

#define SMOOTHED_DELAY_MAX 10

static const AEChannel KnownChannels[] = { AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_SL, AE_CH_SR, AE_CH_BL, AE_CH_BR, AE_CH_BC, AE_CH_BLOC, AE_CH_BROC, AE_CH_NULL };

static bool Has71Support()
{
  /* Android 5.0 introduced side channels */
  return CJNIAudioManager::GetSDKVersion() >= 21;
}

static AEChannel AUDIOTRACKChannelToAEChannel(int atChannel)
{
  AEChannel aeChannel;

  /* cannot use switch since CJNIAudioFormat is populated at runtime */

       if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT)            aeChannel = AE_CH_FL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT)           aeChannel = AE_CH_FR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER)          aeChannel = AE_CH_FC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY)         aeChannel = AE_CH_LFE;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT)             aeChannel = AE_CH_BL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT)            aeChannel = AE_CH_BR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT)             aeChannel = AE_CH_SL;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT)            aeChannel = AE_CH_SR;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER)  aeChannel = AE_CH_FLOC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER) aeChannel = AE_CH_FROC;
  else if (atChannel == CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER)           aeChannel = AE_CH_BC;
  else                                                                      aeChannel = AE_CH_UNKNOWN1;

  return aeChannel;
}

static int AEChannelToAUDIOTRACKChannel(AEChannel aeChannel)
{
  int atChannel;
  switch (aeChannel)
  {
    case AE_CH_FL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT; break;
    case AE_CH_FR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT; break;
    case AE_CH_FC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_CENTER; break;
    case AE_CH_LFE:   atChannel = CJNIAudioFormat::CHANNEL_OUT_LOW_FREQUENCY; break;
    case AE_CH_BL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_LEFT; break;
    case AE_CH_BR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_RIGHT; break;
    case AE_CH_SL:    atChannel = CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT; break;
    case AE_CH_SR:    atChannel = CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT; break;
    case AE_CH_BC:    atChannel = CJNIAudioFormat::CHANNEL_OUT_BACK_CENTER; break;
    case AE_CH_FLOC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_LEFT_OF_CENTER; break;
    case AE_CH_FROC:  atChannel = CJNIAudioFormat::CHANNEL_OUT_FRONT_RIGHT_OF_CENTER; break;
    default:          atChannel = CJNIAudioFormat::CHANNEL_INVALID; break;
  }
  return atChannel;
}

static CAEChannelInfo AUDIOTRACKChannelMaskToAEChannelMap(int atMask)
{
  CAEChannelInfo info;

  int mask = 0x1;
  for (unsigned int i = 0; i < sizeof(int32_t) * 8; i++)
  {
    if (atMask & mask)
      info += AUDIOTRACKChannelToAEChannel(mask);
    mask <<= 1;
  }

  return info;
}

static int AEChannelMapToAUDIOTRACKChannelMask(CAEChannelInfo info)
{
#ifdef LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1
  if (info.Count() > 6 && Has71Support())
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1
         | CJNIAudioFormat::CHANNEL_OUT_SIDE_LEFT
         | CJNIAudioFormat::CHANNEL_OUT_SIDE_RIGHT;
  else if (info.Count() > 2)
    return CJNIAudioFormat::CHANNEL_OUT_5POINT1;
  else
    return CJNIAudioFormat::CHANNEL_OUT_STEREO;
#endif

  info.ResolveChannels(KnownChannels);

  int atMask = 0;

  for (unsigned int i = 0; i < info.Count(); i++)
    atMask |= AEChannelToAUDIOTRACKChannel(info[i]);

  return atMask;
}

static jni::CJNIAudioTrack *CreateAudioTrack(int stream, int sampleRate, int channelMask, int encoding, int localbufferSize, int remoteBufferSize)
{
  jni::CJNIAudioTrack *jniAt = NULL;

  try
  {
    jniAt = new CJNIAudioTrack(stream,
                               sampleRate,
                               channelMask,
                               encoding,
                               localbufferSize,
                               remoteBufferSize,
                               CJNIAudioTrack::MODE_STREAM);
  }
  catch (const std::invalid_argument& e)
  {
    CLog::Log(LOGINFO, "AESinkAUDIOTRACK - AudioTrack creation (channelMask 0x%08x): %s", channelMask, e.what());
  }

  return jniAt;
}


CAEDeviceInfo CAESinkAUDIOTRACK::m_info;
std::set<unsigned int> CAESinkAUDIOTRACK::m_sink_sampleRates;

////////////////////////////////////////////////////////////////////////////////////////////
CAESinkAUDIOTRACK::CAESinkAUDIOTRACK()
{
  m_alignedS16 = NULL;
  m_sink_frameSize = 0;
  m_encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
  m_audiotrackbuffer_sec = 0.0;
  m_at_jni = NULL;
  m_duration_written = 0;
  m_offset = -1;
  m_volume = -1;
  m_smoothedDelayCount = 0;
  m_sink_sampleRate = 0;
  m_passthrough = false;
  m_min_buffer_size = 0;
  m_raw_buffer_time = 0;
  m_extSilenceTimer.SetExpired();
  // intermediate buffer management
  m_raw_buffer = nullptr;
  m_raw_buffer_packages = 0;
  m_raw_package_sum_size = 0;
  m_raw_sink_delay = 0;
  m_atbuffer = MAX_RAW_AUDIO_BUFFER * 2;
  m_realRawTime = 0;
}

CAESinkAUDIOTRACK::~CAESinkAUDIOTRACK()
{
  Deinitialize();
}

bool CAESinkAUDIOTRACK::IsSupported(int sampleRateInHz, int channelConfig, int encoding)
{
  int ret = CJNIAudioTrack::getMinBufferSize( sampleRateInHz, channelConfig, encoding);
  return (ret > 0);
}

bool CAESinkAUDIOTRACK::Initialize(AEAudioFormat &format, std::string &device)
{
  m_format      = format;
  m_volume      = -1;
  m_smoothedDelayCount = 0;
  m_smoothedDelayVec.clear();
  m_extSilenceTimer.SetExpired();

  m_offset = -1;
  m_raw_buffer_time = 0;
  m_raw_buffer_packages = 0;
  m_raw_package_sum_size = 0;
  m_raw_sink_delay = 0;
  m_realRawTime = 0;

  m_atbuffer = MAX_RAW_AUDIO_BUFFER * 2;

  CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Initialize requested: sampleRate %u; format: %s; channels: %d", format.m_sampleRate, CAEUtil::DataFormatToStr(format.m_dataFormat), format.m_channelLayout.Count());

  int stream = CJNIAudioManager::STREAM_MUSIC;
  m_encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;

  // Get equal or lower supported sample rate
  std::set<unsigned int>::iterator s = m_sink_sampleRates.upper_bound(m_format.m_sampleRate);
  if (--s != m_sink_sampleRates.begin())
    m_sink_sampleRate = *s;
  else
    m_sink_sampleRate = CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC);

  if (m_format.m_dataFormat == AE_FMT_RAW && !CXBMCApp::IsHeadsetPlugged())
  {
    m_passthrough = true;

    if (!m_info.m_wantsIECPassthrough)
    {
      switch (m_format.m_streamInfo.m_type)
      {
        case CAEStreamInfo::STREAM_TYPE_AC3:
          m_encoding              = CJNIAudioFormat::ENCODING_AC3;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          break;

        case CAEStreamInfo::STREAM_TYPE_EAC3:
          m_encoding              = CJNIAudioFormat::ENCODING_E_AC3;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          break;

        case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
        case CAEStreamInfo::STREAM_TYPE_DTS_512:
        case CAEStreamInfo::STREAM_TYPE_DTS_1024:
        case CAEStreamInfo::STREAM_TYPE_DTS_2048:
          m_encoding              = CJNIAudioFormat::ENCODING_DTS;
          m_format.m_channelLayout = AE_CH_LAYOUT_2_0;
          break;

        case CAEStreamInfo::STREAM_TYPE_DTSHD:
          m_encoding              = CJNIAudioFormat::ENCODING_DTS_HD;
          m_format.m_channelLayout = AE_CH_LAYOUT_7_1;
          m_atbuffer = MAX_RAW_AUDIO_BUFFER_HD * 2;
          break;

        case CAEStreamInfo::STREAM_TYPE_TRUEHD:
          m_encoding              = CJNIAudioFormat::ENCODING_DOLBY_TRUEHD;
          m_format.m_channelLayout = AE_CH_LAYOUT_7_1;
          m_atbuffer = MAX_RAW_AUDIO_BUFFER_HD * 2;
          break;

        default:
          m_format.m_dataFormat   = AE_FMT_S16LE;
          break;
      }
    }
    else
    {
      m_format.m_dataFormat     = AE_FMT_S16LE;
      m_format.m_sampleRate     = m_sink_sampleRate;
    }
  }
  else
  {
    m_passthrough = false;
    m_format.m_sampleRate     = m_sink_sampleRate;
    if (CJNIAudioManager::GetSDKVersion() >= 21 && m_format.m_channelLayout.Count() == 2)
    {
      m_encoding = CJNIAudioFormat::ENCODING_PCM_FLOAT;
      m_format.m_dataFormat     = AE_FMT_FLOAT;
    }
    else
    {
      m_encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
      m_format.m_dataFormat     = AE_FMT_S16LE;
    }
  }

  int atChannelMask = AEChannelMapToAUDIOTRACKChannelMask(m_format.m_channelLayout);
  m_format.m_channelLayout  = AUDIOTRACKChannelMaskToAEChannelMap(atChannelMask);

#if defined(HAS_LIBAMCODEC)
  if (aml_present() && m_passthrough)
    atChannelMask = CJNIAudioFormat::CHANNEL_OUT_STEREO;
#endif

  while (!m_at_jni)
  {
    m_min_buffer_size       = CJNIAudioTrack::getMinBufferSize( m_sink_sampleRate,
                                                                           atChannelMask,
                                                                           m_encoding);
    CLog::Log(LOGNOTICE, "Min Buffer size: %u", m_min_buffer_size);
    unsigned int real_minimum = m_min_buffer_size;
    if (m_passthrough && !m_info.m_wantsIECPassthrough)
    {
      // heap memory is cheap - this is only for the intermediate buffer
      // to save packages while we are loaded with silence
      // BufferSize is around: 480K
      unsigned int storage      = NUM_RAW_PACKAGES * MAX_RAW_AUDIO_BUFFER_HD;
      CLog::Log(LOGDEBUG, "storage: %u", storage);

      m_format.m_frameSize      = 1;
      m_min_buffer_size         = std::max(m_min_buffer_size, storage * m_format.m_frameSize);
      m_raw_buffer = (uint8_t*) malloc(m_min_buffer_size);
      if (!m_raw_buffer)
      {
        CLog::Log(LOGERROR, "Failed to create buffer");
        Deinitialize();
        return false;
      }
    }
    else
    {
      m_min_buffer_size           *= 2;
      m_format.m_frameSize    = m_format.m_channelLayout.Count() *
        (CAEUtil::DataFormatToBits(m_format.m_dataFormat) / 8);
      m_format.m_frames         = (int)(m_min_buffer_size / m_format.m_frameSize) / 2;
      // in non pass-through mode we can use proper calculations for AT, meaning
      // frames have a direct mapping to play-length
      m_atbuffer = m_min_buffer_size;
    }
    m_sink_frameSize          = m_format.m_frameSize;

    if (m_passthrough && !m_info.m_wantsIECPassthrough)
    {
       // aim for 500 ms buffer but for TrueHD we hard code
       //real_minimum = std::max(m_sink_sampleRate * m_sink_frameSize / 2, 2 * real_minimum);
       // think again about it
       real_minimum = MAX_RAW_AUDIO_BUFFER + MAX_RAW_AUDIO_BUFFER / NUM_RAW_PACKAGES;

       if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD || m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD)
	 real_minimum = MAX_RAW_AUDIO_BUFFER_HD;

       CLog::Log(LOGDEBUG, "Initialized with: %u", real_minimum);

       m_audiotrackbuffer_sec = (double)(real_minimum / m_sink_frameSize) / (double)m_sink_sampleRate;
       m_format.m_frames = m_atbuffer / 2;
    }
    else
      m_audiotrackbuffer_sec    = (double)(m_min_buffer_size / m_sink_frameSize) / (double)m_sink_sampleRate;




    CLog::Log(LOGDEBUG, "Created Audiotrackbuffer with playing time of %lf ms ", m_audiotrackbuffer_sec * 1000);

    if (m_passthrough && !m_info.m_wantsIECPassthrough)
    {
      m_at_jni                  = CreateAudioTrack(stream, m_sink_sampleRate,
                                                   atChannelMask, m_encoding,
                                                   m_atbuffer, real_minimum); // let's see what happens
    }
    else
    {
      m_at_jni                  = CreateAudioTrack(stream, m_sink_sampleRate,
	                                           atChannelMask, m_encoding,
	                                           m_atbuffer, m_atbuffer);
    }

    if (!IsInitialized())
    {
      if (!m_passthrough)
      {
        if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO &&
            atChannelMask != CJNIAudioFormat::CHANNEL_OUT_5POINT1)
        {
          atChannelMask = CJNIAudioFormat::CHANNEL_OUT_5POINT1;
          CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying multichannel playback with a 5.1 layout");
          continue;
        }
        else if (atChannelMask != CJNIAudioFormat::CHANNEL_OUT_STEREO)
        {
          atChannelMask = CJNIAudioFormat::CHANNEL_OUT_STEREO;
          CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - Retrying with a stereo layout");
          continue;
        }
      }
      CLog::Log(LOGERROR, "AESinkAUDIOTRACK - Unable to create AudioTrack");
      Deinitialize();
      return false;
    }
    CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Initialize returned: m_sampleRate %u; format:%s; min_buffer_size %u; m_frames %u; m_frameSize %u; channels: %d", m_format.m_sampleRate, CAEUtil::DataFormatToStr(m_format.m_dataFormat), m_min_buffer_size, m_format.m_frames, m_format.m_frameSize, m_format.m_channelLayout.Count());
  }
  format                    = m_format;

  // Force volume to 100% for passthrough
  if (m_passthrough && m_info.m_wantsIECPassthrough)
  {
    CXBMCApp::AcquireAudioFocus();
    m_volume = CXBMCApp::GetSystemVolume();
    CXBMCApp::SetSystemVolume(1.0);
  }

  return true;
}

void CAESinkAUDIOTRACK::Deinitialize()
{
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGDEBUG, "CAESinkAUDIOTRACK::Deinitialize");
#endif
  // Restore volume
  if (m_volume != -1)
  {
    CXBMCApp::SetSystemVolume(m_volume);
    CXBMCApp::ReleaseAudioFocus();
  }

  if (!m_at_jni)
    return;

  if (IsInitialized())
  {
    m_at_jni->stop();
    m_at_jni->flush();
  }
  m_at_jni->release();

  m_duration_written = 0;
  m_offset = -1;

  free (m_raw_buffer);
  m_raw_buffer = nullptr;
  m_raw_buffer_packages = 0;

  m_extSilenceTimer.SetExpired();
  m_raw_buffer_time = 0;
  m_raw_buffer_packages = 0;
  m_raw_package_sum_size = 0;
  m_raw_sink_delay = 0;
  m_realRawTime = 0;

  delete m_at_jni;
  m_at_jni = NULL;
}

bool CAESinkAUDIOTRACK::IsInitialized()
{
  return (m_at_jni && m_at_jni->getState() == CJNIAudioTrack::STATE_INITIALIZED);
}

void CAESinkAUDIOTRACK::GetDelay(AEDelayStatus& status)
{
  if (!m_at_jni)
  {
    status.SetDelay(0);
    return;
  }

  // In their infinite wisdom, Google decided to make getPlaybackHeadPosition
  // return a 32bit "int" that you should "interpret as unsigned."  As such,
  // for wrap saftey, we need to do all ops on it in 32bit integer math.
  uint32_t head_pos = (uint32_t)m_at_jni->getPlaybackHeadPosition();
  // head_pos does not necessarily start at the beginning
  if (m_offset == -1)
  {
    CLog::Log(LOGDEBUG, "Offset update to %u", head_pos);
    m_offset = head_pos;
  }

  uint32_t normHead_pos = head_pos - m_offset;

  double gone = (double)normHead_pos / m_sink_sampleRate;

  double delay = (double) m_duration_written - gone;
  CLog::Log(LOGNOTICE, "Jetzt aber: duration written: %lf sampleRate: %u gone: %lf", m_duration_written, m_sink_sampleRate, gone);
  bool playing = m_at_jni->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING;

  // silence timer might still running
  if (m_passthrough && !m_info.m_wantsIECPassthrough)
  {
    // save as measurement for for real payload
    m_raw_sink_delay = delay;
    delay += m_extSilenceTimer.MillisLeft() / 1000.0;
    delay += m_raw_buffer_time;
  }

    double smootheDelay = delay;
/*  m_smoothedDelayVec.push_back(delay);
  if (m_smoothedDelayCount <= SMOOTHED_DELAY_MAX)
    m_smoothedDelayCount++;
  else
    m_smoothedDelayVec.erase(m_smoothedDelayVec.begin());

  double smootheDelay = 0;
  for (double d : m_smoothedDelayVec)
    smootheDelay += d;
  smootheDelay /= m_smoothedDelayCount;
*/

  CLog::Log(LOGDEBUG, "Current-Delay: %lf Head Pos: %u Raw Intermediate Buffer Time: %lf, Real Raw Buffer Time: %lf, Real Sink delay: %lf, Silence: %u Playing: %s", smootheDelay * 1000,
                       normHead_pos, m_raw_buffer_time * 1000, m_realRawTime * 1000, m_raw_sink_delay * 1000, m_extSilenceTimer.MillisLeft(), playing ? "yes" : "no");

  status.SetDelay(smootheDelay);
}

double CAESinkAUDIOTRACK::GetLatency()
{
  return 0.0;
}

double CAESinkAUDIOTRACK::GetCacheTotal()
{
  // total amount that the audio sink can buffer in units of seconds
  return m_audiotrackbuffer_sec;
}

// this method is supposed to block until all frames are written to the device buffer
// when it returns ActiveAESink will take the next buffer out of a queue
unsigned int CAESinkAUDIOTRACK::AddPackets(uint8_t **data, unsigned int frames, unsigned int offset)
{
  if (!IsInitialized())
    return INT_MAX;

  CLog::Log(LOGNOTICE, "Got frames: %u", frames);
  uint8_t *buffer = data[0]+offset*m_format.m_frameSize;
  uint8_t *out_buf = buffer;
  int size = frames * m_format.m_frameSize;

  // write as many frames of audio as we can fit into our internal buffer.
  int written = 0;
  int loop_written = 0;
  if (frames)
  {
    if (m_passthrough && !m_info.m_wantsIECPassthrough)
    {
     if (size > MAX_RAW_AUDIO_BUFFER_HD)
     {
       CLog::Log(LOGERROR, "Sorry We cannot cope with so much samples!");
       return INT_MAX;
     }

     // No matter what happens if we have already filled up >= our half real sink buffer
     // we could overrun the AT buffer with the next package so directly write out everything we
     // have
     if (m_raw_package_sum_size + size > m_atbuffer / 2)
     {
       memcpy(m_raw_buffer + m_raw_package_sum_size, buffer, size);
       m_raw_buffer_packages++;
       m_raw_package_sum_size += size;
       m_raw_buffer_time += m_format.m_streamInfo.GetDuration() / 1000.0;
       // update out_buf and increased size
       out_buf = m_raw_buffer;
       size = m_raw_package_sum_size;
       m_extSilenceTimer.SetExpired();
       CLog::Log(LOGDEBUG, "(A) Trying to add the complete intermediate buffer: %u %lf", size, m_raw_buffer_time);
     }
     else if (!m_extSilenceTimer.IsTimePast())
     {
        // let the timer run down
        while (GetIntermediateBufferSpace() < (m_format.m_streamInfo.GetDuration() / 1000.0) && !m_extSilenceTimer.IsTimePast())
        {
          usleep(m_format.m_streamInfo.GetDuration() * 1000);
          CLog::Log(LOGDEBUG, "Waiting for buffer space, free space: %lf ms", GetIntermediateBufferSpace() * 1000);
        }

        // at this position we have either enough space or timer is stopped and we need to add
        bool enoughSpace = GetIntermediateBufferSpace() > (m_format.m_streamInfo.GetDuration() / 1000.0);

        // just add it if timer is still running
        if (enoughSpace && !m_extSilenceTimer.IsTimePast())
        {
          memcpy(m_raw_buffer + m_raw_package_sum_size, buffer, size);
          m_raw_buffer_packages++;
          m_raw_package_sum_size += size;
          m_raw_buffer_time += m_format.m_streamInfo.GetDuration() / 1000.0;
          return frames;
        }

        // timer has ended we need to write all the data
        if ((enoughSpace || (m_raw_package_sum_size + size < m_min_buffer_size)) && m_extSilenceTimer.IsTimePast())
        {
          memcpy(m_raw_buffer + m_raw_package_sum_size, buffer, size);
          m_raw_buffer_packages++;
          m_raw_package_sum_size += size;
          m_raw_buffer_time += m_format.m_streamInfo.GetDuration() / 1000.0;
          // update out_buf and increased size
          out_buf = m_raw_buffer;
          size = m_raw_package_sum_size;
          CLog::Log(LOGDEBUG, "(1) Trying to add the complete intermediate buffer: %u %lf", size, m_raw_buffer_time);
        }
        else
        {
          CLog::Log(LOGERROR, "Buffer miss-management - Timer running: %s, m_minbuffer_size: %u, size: %d bufnew: %d",
                               m_extSilenceTimer.IsTimePast() ? "no" : "yes",
                               m_min_buffer_size, size, m_raw_package_sum_size + size);
          return INT_MAX;
        }
      }
      else
      {
	// if we get here our intermediate buffer is perhaps "full by calculation" - but as we have more space
	// just append it and write out that shit
	if (m_raw_package_sum_size + size >= m_min_buffer_size)
        {
	    CLog::Log(LOGDEBUG, "Buffer miscalculation (AddPakets 3) - cannot find space - aborting, free: %lf needed: %lf", GetIntermediateBufferSpace(), m_format.m_streamInfo.GetDuration() / 1000.0);
	   return INT_MAX;
	}
	// enqueue and add completely onto the real sink
        if (m_raw_buffer_packages > 0)
        {
          memcpy(m_raw_buffer + m_raw_package_sum_size, buffer, size);
          m_raw_buffer_packages++;
          m_raw_package_sum_size += size;
          m_raw_buffer_time += m_format.m_streamInfo.GetDuration() / 1000.0;
          // update out_buf and increased size
          out_buf = m_raw_buffer;
          size = m_raw_package_sum_size;
          CLog::Log(LOGDEBUG, "Trying to add the complete intermediate buffer: %u %lf", size, m_raw_buffer_time);
        }
      }
    }
    // android will auto pause the playstate when it senses idle,
    // check it and set playing if it does this. Do this before
    // writing into its buffer.
    if (m_at_jni->getPlayState() != CJNIAudioTrack::PLAYSTATE_PLAYING)
      m_at_jni->play();
    int frag = 0;

    while (written < size)
    {
      loop_written = m_at_jni->write((char*)out_buf, 0, size);
      written += loop_written;
      if (loop_written < 0)
      {
        CLog::Log(LOGERROR, "CAESinkAUDIOTRACK::AddPackets write returned error:  %d", loop_written);
        return INT_MAX;
      }
      if (m_passthrough && !m_info.m_wantsIECPassthrough)
      {
	// if we did not get a full raw package onto the sink
	// error out as implementations cannot cope with with fragmented raw packages
        if (loop_written != size)
        {
            CLog::Log(LOGERROR, "CAESinkAUDIOTRACK::AddPackets causes fragmentation of raw packages:  %d", loop_written);
            m_raw_buffer_packages = 0;
            m_raw_buffer_time = 0;
            m_raw_package_sum_size = 0;
            return INT_MAX;
        }
        // add the time for raw packages
        if (m_raw_buffer_packages > 0)
        {
          m_duration_written += m_raw_buffer_time;
          m_realRawTime += m_raw_buffer_time;
        }
        else
        {
          m_duration_written += m_format.m_streamInfo.GetDuration() / 1000;
          m_realRawTime += m_format.m_streamInfo.GetDuration() / 1000;
        }
      }
      else
        m_duration_written += ((double)loop_written / m_sink_frameSize) / m_format.m_sampleRate;

      // just try again to care for fragmentation
      if (written < size)
      {
	out_buf = out_buf + loop_written;
      }
      loop_written = 0;
      frag++;
    }

    if (m_passthrough && !m_info.m_wantsIECPassthrough)
    {
       if (m_raw_buffer_packages > 0)
       {
         // remove from calculation as it was added into the sink buffer
         m_raw_buffer_time = 0;
         // all went fine intermediate buffer is on sink
         m_raw_buffer_packages = 0;
         m_raw_package_sum_size = 0;
         // tell AE that all data was written
         written = frames;
       }
    }
  }

  unsigned int written_frames = (unsigned int)(written/m_format.m_frameSize);
  return written_frames;
}

double CAESinkAUDIOTRACK::GetIntermediateBufferSpace()
{
  return GetCacheTotal() - m_raw_buffer_time - m_extSilenceTimer.MillisLeft() / 1000.0;
}

void CAESinkAUDIOTRACK::AddPause(unsigned int millis)
{
  if (!m_at_jni)
    return;


  CLog::Log(LOGDEBUG, "Trying to add Pause packet of size: %u ms", millis);

  // AE wants to flush our audio - we still have data in buffer
  // give it time to write it away
  if (m_raw_sink_delay > 0)
  {
    CLog::Log(LOGDEBUG, "Syncing: %u", millis);
    usleep(millis * 1000);
    return;
  }

  // if we are here - we have nothing to play in buffer
  if (m_at_jni->getPlayState() == CJNIAudioTrack::PLAYSTATE_PLAYING)
    m_at_jni->pause();

  while (GetIntermediateBufferSpace() < millis / 1000.0 && !m_extSilenceTimer.IsTimePast())
  {
    usleep(m_format.m_streamInfo.GetDuration() * 1000);
    CLog::Log(LOGDEBUG, "Waiting for space in buffer %lf", GetIntermediateBufferSpace());
  }

  // increase running silence timer
  if (GetIntermediateBufferSpace() >= millis / 1000.0)
  {
    m_extSilenceTimer.Set(m_extSilenceTimer.MillisLeft() + millis);
  }
  else
   CLog::Log(LOGDEBUG, "We are running out of space - miscalculation - ignoring silence");
}

void CAESinkAUDIOTRACK::Drain()
{
  if (!m_at_jni)
    return;

  // TODO: does this block until last samples played out?
  // we should not return from drain as long the device is in playing state

  m_at_jni->stop();
  m_duration_written = 0;
  m_offset = -1;
  m_raw_buffer_packages = 0;
  m_raw_buffer_time = 0;
  m_raw_package_sum_size = 0;
  m_realRawTime = 0;
}

void CAESinkAUDIOTRACK::EnumerateDevicesEx(AEDeviceInfoList &list, bool force)
{
  m_info.m_channels.Reset();
  m_info.m_dataFormats.clear();
  m_info.m_sampleRates.clear();

  m_info.m_deviceType = AE_DEVTYPE_PCM;
  m_info.m_deviceName = "AudioTrack";
  m_info.m_displayName = "android";
  m_info.m_displayNameExtra = "audiotrack";
#ifdef LIMIT_TO_STEREO_AND_5POINT1_AND_7POINT1
  if (Has71Support())
    m_info.m_channels = AE_CH_LAYOUT_7_1;
  else
    m_info.m_channels = AE_CH_LAYOUT_5_1;
#else
  m_info.m_channels = KnownChannels;
#endif
  m_info.m_dataFormats.push_back(AE_FMT_S16LE);

  m_sink_sampleRates.clear();
  m_sink_sampleRates.insert(CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC));

  m_info.m_wantsIECPassthrough = true;
  if (!CXBMCApp::IsHeadsetPlugged())
  {
    m_info.m_deviceType = AE_DEVTYPE_HDMI;
    m_info.m_dataFormats.push_back(AE_FMT_RAW);
    m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_AC3);
    m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD_CORE);
    m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_1024);
    m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_2048);
    m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_512);

#if defined(HAS_LIBAMCODEC)
    if (aml_present())
    {
      // passthrough
      m_info.m_wantsIECPassthrough = true;
      m_sink_sampleRates.insert(48000);
    }
    else
#endif
    {
      int test_sample[] = { 32000, 44100, 48000, 96000, 192000 };
      int test_sample_sz = sizeof(test_sample) / sizeof(int);
      int encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
      if (CJNIAudioManager::GetSDKVersion() >= 21)
        encoding = CJNIAudioFormat::ENCODING_PCM_FLOAT;
      for (int i=0; i<test_sample_sz; ++i)
      {
        if (IsSupported(test_sample[i], CJNIAudioFormat::CHANNEL_OUT_STEREO, encoding))
        {
          m_sink_sampleRates.insert(test_sample[i]);
          CLog::Log(LOGDEBUG, "AESinkAUDIOTRACK - %d supported", test_sample[i]);
        }
      }
      if (CJNIAudioManager::GetSDKVersion() >= 21)
      {
        m_info.m_wantsIECPassthrough = false;
        m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_EAC3);

        if (CJNIAudioManager::GetSDKVersion() >= 23)
        {
          m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD);
        }
        if (StringUtils::StartsWithNoCase(CJNIBuild::DEVICE, "foster")) // SATV is ahead of API
        {
          m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD);
          m_info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_TRUEHD);
        }
      }
    }
    std::copy(m_sink_sampleRates.begin(), m_sink_sampleRates.end(), std::back_inserter(m_info.m_sampleRates));
  }

  list.push_back(m_info);
}

