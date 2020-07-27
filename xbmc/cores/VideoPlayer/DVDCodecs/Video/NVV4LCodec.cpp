#include "NVV4LCodec.h"

#include "Buffers/VideoBuffer.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "Process/ProcessInfo.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoSettings.h"
#include "ServiceBroker.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/log.h"

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libv4l2.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <chrono>


#define VERBOSE 1


// copied from NVidia v4l2 extensions
#ifndef __V4L2_NV_EXTENSIONS_H__

#define V4L2_EVENT_RESOLUTION_CHANGE 5
#define V4L2_CID_MPEG_VIDEO_DEVICE_POLL (V4L2_CID_MPEG_BASE+550)
#define V4L2_CID_MPEG_SET_POLL_INTERRUPT (V4L2_CID_MPEG_BASE+551)

#define V4L2_PIX_FMT_H265     v4l2_fourcc('H', '2', '6', '5')


/**
 * Poll device
 */
typedef struct _v4l2_ctrl_video_device_poll
{
    __u16 req_events;    // Requested events, a bitmask of POLLIN, POLLOUT, POLLERR, POLLPRI.
    __u16 resp_events;    // Returned events a similar bitmask of above events.
} v4l2_ctrl_video_device_poll;


#endif


using namespace KODI::NVV4L;

NVV4LCodec::NVV4LCodec(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo), m_dec_dev("/dev/nvhost-nvdec")
{
  memset(m_pts, 0, sizeof(m_pts));
  memset(m_dts, 0, sizeof(m_dts));
  m_ipts = 0;
};


NVV4LCodec::~NVV4LCodec() 
{
  Close();
};

bool NVV4LCodec::OpenDevice()
{
  CLog::Log(LOGINFO, "NVV4LCodec::Open opening device %s", m_dec_dev);

  device_fd = v4l2_open(m_dec_dev, O_RDWR | O_NONBLOCK);
 
  if (device_fd < 0)
  {
    CLog::Log(LOGERROR, "NVV4LCodec::Open v4l2 device open failed");
    return false;
  }

  struct v4l2_capability caps;

  if (v4l2_ioctl(device_fd, VIDIOC_QUERYCAP, &caps) < 0)
  {
    CLog::Log(LOGERROR, "NVV4LCodec::Open video capabilities query failed");
    return false;
  }

  if (!(caps.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE))
  {
    CLog::Log(LOGERROR, "NVV4LCodec::Open video capability M2M not supported");
    return false;
  }


  memset(&output_format, 0, sizeof(output_format));
  output_format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  output_format.fmt.pix_mp.pixelformat = m_coding_type;
  output_format.fmt.pix_mp.num_planes = 1;
  output_format.fmt.pix_mp.plane_fmt[0].sizeimage = BUFFER_SIZE;

  m_pool_output = std::make_shared<CNVV4LBufferPool>(device_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_MMAP); 
  m_pool_capture = std::make_shared<CNVV4LBufferPool>(device_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP);

  if (!m_pool_output->Init(output_format, INPUT_BUFFERS)) 
  {
    CLog::Log(LOGERROR, "NVV4LCodec::Open filed to initalize buffer pool");
    return false;
  }


  CLog::Log(LOGINFO, "NVV4LCodec::Open device ready");

  return true;
};

bool NVV4LCodec::SubscribeEvent(uint32_t type, uint32_t id, uint32_t flags)
{
  CLog::Log(LOGINFO, "NVV4LCodec::SubscribeEvent subscribe to event");

  struct v4l2_event_subscription sub;
  memset(&sub, 0, sizeof(struct v4l2_event_subscription));

  sub.id = id;
  sub.type = type;
  sub.flags = flags;

  if (v4l2_ioctl(device_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) 
  {
    CLog::Log(LOGERROR, "NVV4LCodec::SubscribeEvent failed");
    return false;
  }

  return true;
};

bool NVV4LCodec::DequeueEvent(struct v4l2_event *ev) 
{
  if (v4l2_ioctl(device_fd, VIDIOC_DQEVENT, ev) < 0)
    return false;

  return true;
};

bool NVV4LCodec::QueryCaptureFormat(struct v4l2_format *format)
{
  format->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

  if (v4l2_ioctl(device_fd, VIDIOC_G_FMT, format) < 0)
  {
    CLog::Log(LOGERROR, "NVV4LCodec::QueryCaptureFormat failed");
    return false;
  }

  return true;
};


void NVV4LCodec::Close()
{
  if (m_is_open.load(std::memory_order_relaxed))
  {
    CLog::Log(LOGINFO, "NVV4LCodec::Close closing decoder");

    m_is_open.store(false, std::memory_order_acquire);

//    if (m_decoder_thread.joinable())
//      m_decoder_thread.join();

    StreamOff(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    StreamOff(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    m_is_capturing.store(false);

    if (m_pool_output)
      m_pool_output->Dispose();

    if (m_pool_capture)
      m_pool_capture->Dispose();

    v4l2_close(device_fd);
    device_fd = 0;

    CLog::Log(LOGINFO, "NVV4LCodec::Close decoder closed");
  }
};

bool NVV4LCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) 
{
  switch(hints.codec)
  {
    case AV_CODEC_ID_H264:
      m_coding_type = V4L2_PIX_FMT_H264;
      m_codec = "h264";
    break;
    case AV_CODEC_ID_HEVC:
      m_coding_type = V4L2_PIX_FMT_H265;
      m_codec = "hevc";
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_coding_type = V4L2_PIX_FMT_MPEG4;
      m_codec = "mpeg4";
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_coding_type = V4L2_PIX_FMT_MPEG2;
      m_codec = "mpeg2";
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_coding_type =  V4L2_PIX_FMT_VP8;
      m_codec = "vp8";
    break;
    case AV_CODEC_ID_VP9:
      // VP8
      m_coding_type =  V4L2_PIX_FMT_VP9;
      m_codec = "vp9";
    default:
      CLog::Log(LOGERROR, LOGVIDEO, "NVV4LCodec::Open Video codec unknown: %x", hints.codec);
      return false;
    break;
  };

  if (! OpenDevice())
    return false; 

  if (! SubscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0))
    return false;

  if (! StreamOn(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE))
    return false;

  m_bitconverter = std::make_unique<CBitstreamConverter>();
  m_bitconverter->Open(hints.codec, (uint8_t*)hints.extradata, hints.extrasize, true);

  m_is_open.store(true, std::memory_order_relaxed);

  // start decoder thread
  // m_decoder_thread = std::thread(&NVV4LCodec::DecoderLoop, this);


  return true;
};

bool NVV4LCodec::AddData(const DemuxPacket &packet)
{


  CNVV4LBuffer *buffer;
  if (m_pool_output->HasFreeBuffers())
  {
    buffer = m_pool_output->GetBuffer();
  } else {
    buffer = m_pool_output->DequeueBuffer();
  }

  if (!buffer)
  {
    return false;
  }

  if (packet.iSize == 0)
  {
     m_eos.store(true, std::memory_order_relaxed);
     // send one empty buffer to decoder to indicate end of stream
  
     m_pts[m_ipts % PTS_MAX] = packet.pts;
     m_dts[m_ipts % PTS_MAX] = packet.dts;
     buffer->SetPts(m_ipts++);

     buffer->Enqueue();
  }

  m_bitconverter->Convert(packet.pData, packet.iSize);

  if (!m_bitconverter->CanStartDecode()) {
      CLog::Log(LOGDEBUG, "NVV4LCodec::AddData: waiting for keyframe (bitstream)");
      return true;
  }

  uint8_t* data = m_bitconverter->GetConvertBuffer();
  size_t len = m_bitconverter->GetConvertSize();


  m_pts[m_ipts % PTS_MAX] = packet.pts;
  m_dts[m_ipts % PTS_MAX] = packet.dts;

  buffer->SetPts(m_ipts++);
  buffer->write(data, len);

  if (!buffer->Enqueue())
  {
    CLog::Log(LOGERROR, "NVV4LCodec::AddData: failed to enqueue buffer id:%d : %s", buffer->GetId(), strerror(errno));
    return false;
  }

  if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "NVV4LCodec::AddData: enqueued output buffer id:%d pts:%d ptsv:%.3f", buffer->GetId(),
              buffer->GetPts(), packet.pts);

  return true;
};


void NVV4LCodec::Reset()
{
  m_coder_control_flag = 0;

  m_preroll.store(true, std::memory_order_relaxed);

  memset(m_pts, 0, sizeof(m_pts));
  memset(m_dts, 0, sizeof(m_dts));

  m_ipts = 0;
  m_flushed = true;

  StreamOff(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  StreamOff(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

  m_pool_output->Reset();
  m_pool_capture->Dispose();

  m_eos.store(false, std::memory_order_relaxed);
  StreamOn(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

  CLog::Log(LOGINFO, "NVV4LCodec::Reset decoder reset");
};

CDVDVideoCodec::VCReturn NVV4LCodec::GetPicture(VideoPicture* pVideoPicture)
{

  if (pVideoPicture->videoBuffer) 
  {
    pVideoPicture->videoBuffer->Release();
    pVideoPicture->videoBuffer = nullptr;
  }

  if (m_flushed)
  {
    m_flushed = false;
    return CDVDVideoCodec::VCReturn::VC_REOPEN;
  }

  HandleEvent();

  if (!m_is_capturing.load(std::memory_order_relaxed) || (m_coder_control_flag & DVD_CODEC_CTRL_DRAIN)) 
    return CDVDVideoCodec::VCReturn::VC_BUFFER;

  
  if (m_eos.load(std::memory_order_relaxed)) {
    return CDVDVideoCodec::VCReturn::VC_EOF;
  }

  DispatchCapture();

  CNVV4LBuffer* buffer = m_pool_capture->DequeueBuffer();

  if (!buffer)
  {
    return CDVDVideoCodec::VCReturn::VC_BUFFER;
  }

  buffer->Acquire(m_pool_capture);

  pVideoPicture->videoBuffer = buffer;

  const struct v4l2_format fmt = m_pool_capture->GetFormat();

  pVideoPicture->iHeight = fmt.fmt.pix_mp.height;
  pVideoPicture->iWidth = fmt.fmt.pix_mp.width;
  pVideoPicture->iDisplayHeight = fmt.fmt.pix_mp.height;
  pVideoPicture->iDisplayWidth = fmt.fmt.pix_mp.width;
  pVideoPicture->color_range = 0;
  pVideoPicture->iFlags = 0;

  pVideoPicture->iRepeatPicture = 0;
  pVideoPicture->color_space = 0; // not relevant for NVRenderer

  pVideoPicture->pts = m_pts[buffer->GetPts() % PTS_MAX];
  pVideoPicture->dts = m_dts[buffer->GetPts() % PTS_MAX];

  if (m_coder_control_flag & DVD_CODEC_CTRL_DROP) 
  {
    pVideoPicture->videoBuffer->Release();
    pVideoPicture->videoBuffer = nullptr;

    pVideoPicture->iFlags |= DVP_FLAG_DROPPED;
  }
  return CDVDVideoCodec::VCReturn::VC_PICTURE;
};


void NVV4LCodec::DecoderLoop() 
{

  while (m_is_open.load(std::memory_order_relaxed))
  {

    while (!m_preroll.load(std::memory_order_relaxed) && m_pool_output->WaitForFullPool(100)) 
    {

      if (!m_eos.load(std::memory_order_relaxed))
        DispatchCapture();

      EnableInterrupt();

      struct v4l2_ext_control control;
      struct v4l2_ext_controls ctrls;
      v4l2_ctrl_video_device_poll devicepoll;
    
      memset(&control, 0, sizeof(struct v4l2_ext_control));
      memset(&ctrls, 0, sizeof(struct v4l2_ext_controls));
      memset(&devicepoll, 0, sizeof(v4l2_ctrl_video_device_poll));
      devicepoll.req_events = POLLIN | POLLOUT | POLLERR | POLLPRI;
      devicepoll.resp_events = 0;

      ctrls.count = 1;
      ctrls.controls = &control;

      control.id = V4L2_CID_MPEG_VIDEO_DEVICE_POLL;
      control.string = (char*)&devicepoll;

      // thread will block here and wait for IO or Interrupt
      v4l2_ioctl(device_fd, VIDIOC_S_EXT_CTRLS, &ctrls);

      HandleEvent();

      HandleOutputPool();

      HandleCapturePool();

      DisabeInterrupt();
    }

  }

  CLog::Log(LOGINFO, "NVV4LCodec::DecoderLoop thread stopped");
}

void NVV4LCodec::HandleEvent()
{
  struct v4l2_event ev;

  if (DequeueEvent(&ev))
  {
    if (ev.type == V4L2_EVENT_RESOLUTION_CHANGE)
    {
      CLog::Log(LOGINFO, "NVV4LCodec::DecoderLoop resolution change received");

      size_t min_buffers;

      struct v4l2_control ctl;
      ctl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;

      if (v4l2_ioctl(device_fd, VIDIOC_G_CTRL, &ctl) < 0)
      {
        CLog::Log(LOGERROR, "NVV4LCodec::DecoderLoop getting min_buffers failed");
      }

      min_buffers = ctl.value;

      struct v4l2_format format;
      memset(&format, 0, sizeof(struct v4l2_format));
      QueryCaptureFormat(&format);

      StreamOn(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

      m_pool_capture->Init(format, min_buffers + EXTRA_OUTPUT_BUFFERS);

      while (m_pool_capture->HasFreeBuffers())
      {
        CNVV4LBuffer *buffer = m_pool_capture->GetBuffer();
        
        if ( buffer->Enqueue() )
        {
          if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
            CLog::Log(LOGDEBUG, "NVV4LCodec::DecoderLoop capture plane enqueued buffer id:%d",
                      buffer->GetId());
        } else {
            CLog::Log(LOGERROR, "NVV4LCodec::DecoderLoop capture plane failed to enqueued buffer id:%d",
                      buffer->GetId());
        }
      }

      CLog::Log(LOGINFO, "NVV4LCodec::DecoderLoop capture plane initalized");
      m_is_capturing.store(true, std::memory_order_relaxed);
  
      
      m_processInfo.SetVideoDimensions(format.fmt.pix_mp.width, format.fmt.pix_mp.height);
      m_processInfo.SetVideoPixelFormat("nvmm:nv12");
      m_processInfo.SetVideoDecoderName("nvdec-" + m_codec, true);
    }
  }
};


void NVV4LCodec::HandleOutputPool() 
{
  for (CNVV4LBuffer* buffer = m_pool_output->DequeueBuffer(); buffer != nullptr; buffer = m_pool_output->DequeueBuffer())
  {
    buffer->Release();
    if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "NVV4LCodec::DecoderLoop dequed output buffer id:%d, pts:%d", buffer->GetId(), buffer->GetPts());
  }
};

void NVV4LCodec::HandleCapturePool() 
{
  if (m_is_capturing.load(std::memory_order_relaxed))
  {
    for (CNVV4LBuffer *buffer = m_pool_capture->DequeueBuffer(); buffer != nullptr; buffer = m_pool_capture->DequeueBuffer())
    {
      m_pool_capture->Ready(buffer->GetId());

      if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG, "NVV4LCodec::DecoderLoop dequed capture buffer id:%d, pts:%d",
                  buffer->GetId(), buffer->GetPts());
    }
  }
};


void NVV4LCodec::DispatchCapture() 
{
  if (m_is_capturing.load(std::memory_order_relaxed))
  {
    for (CNVV4LBuffer *buffer = m_pool_capture->GetBuffer(); buffer != nullptr; buffer = m_pool_capture->GetBuffer())
    {
      if (buffer->Enqueue()) {
        if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
          CLog::Log(LOGDEBUG, "NVV4LCodec::DecoderLoop enqueued capture buffer id:%d",
                    buffer->GetId());
      } else {
        CLog::Log(LOGERROR, "NVV4LCodec::DecoderLoop failed to enqueued capture buffer id:%d", buffer->GetId());
        buffer->Release();
        break;
      }
    }
  }
};

void NVV4LCodec::DispatchOutput() 
{

  for (CNVV4LBuffer* buffer = m_pool_output->PeekReadyBuffer(); buffer != nullptr; buffer = m_pool_output->PeekReadyBuffer())
  {
    if (buffer->Enqueue()) {
      m_pool_output->GetReadyBuffer();
      if (VERBOSE && CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG, "NVV4LCodec::DecoderLoop enqueued output buffer id:%d, pts:%d",
                  buffer->GetId(), buffer->GetPts());
    } else {
      CLog::Log(LOGWARNING, "NVV4LCodec::DecoderLoop failed enqueue output buffer id:%d, pts:%d", buffer->GetId(), buffer->GetPts());
      break;
    }
  }
};

static bool SetIterrupt(const int device_fd, const int value) {
  struct v4l2_ext_control control;
  struct v4l2_ext_controls ctrls;

  memset(&control, 0, sizeof(struct v4l2_ext_control));
  memset(&ctrls, 0, sizeof(struct v4l2_ext_controls));
  
  ctrls.count = 1;
  ctrls.controls = &control;

  control.id = V4L2_CID_MPEG_SET_POLL_INTERRUPT;
  control.value = value;

  int ret = v4l2_ioctl(device_fd, VIDIOC_S_EXT_CTRLS, &ctrls);

  return ret == 0;
}

void NVV4LCodec::EnableInterrupt()
{
  if (!SetIterrupt(device_fd, 1))
    CLog::Log(LOGINFO, "NVV4LCodec::EnableInterrupt failed: %s", strerror(errno));
};


void NVV4LCodec::DisabeInterrupt()
{
  if (!SetIterrupt(device_fd, 0))
    CLog::Log(LOGINFO, "NVV4LCodec::DisableInterrupt failed: %s", strerror(errno));
};

bool NVV4LCodec::StreamOn(uint32_t type)
{
  if (v4l2_ioctl(device_fd, VIDIOC_STREAMON, &type) < 0)
  {
    CLog::Log(LOGINFO, "NVV4LCodec::StreamON failed to start stream: %s", strerror(errno));
  }

  return true;
};

bool NVV4LCodec::StreamOff(uint32_t type) 
{
  if (v4l2_ioctl(device_fd, VIDIOC_STREAMOFF, &type) < 0)
  {
      CLog::Log(LOGINFO, "NVV4LCodec::StreamON failed to stop stream: %s", strerror(errno));
  }

  return true;
};


CDVDVideoCodec* NVV4LCodec::Create(CProcessInfo &processInfo) {
  return new NVV4LCodec(processInfo);
};

void NVV4LCodec::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("nvv4l", NVV4LCodec::Create);
};

CNVV4LBufferPool::CNVV4LBufferPool(int fd, uint32_t type, v4l2_memory memory)
  : m_fd(fd),
    m_type(type),
    m_memory(memory)
{

};

bool CNVV4LBufferPool::Init(struct v4l2_format format, size_t size) 
{
  const std::lock_guard<std::mutex> lock(m_pool_mutex);

  m_size = size;
  m_format = format;

  if (m_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
    if (v4l2_ioctl(m_fd, VIDIOC_S_FMT, &m_format) < 0) 
    {
      CLog::Log(LOGERROR, "CNVV4LBuffer::Query query buffer failed %s", strerror(errno));
      return false;
    }
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(struct v4l2_requestbuffers));

  req.count = m_size;
  req.type = m_type;
  req.memory = m_memory;

  if (v4l2_ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
  {
    CLog::Log(LOGERROR, "CNVV4LBufferPool::Init v4l2 buffer request failed: %s", strerror(errno));
    return false;
  }
  m_size = req.count;


  for (size_t i = m_bufs.size(); i < m_size; ++i)
  {
     m_bufs.push_back(new CNVV4LBuffer(i));
  }

  for (size_t i = 0; i < m_size; ++i)
  {
     CNVV4LBuffer *buffer = m_bufs[i];
     buffer->Init(m_fd, m_format, m_memory);

     if (!buffer->Query())
       return false;

     if (!buffer->Export())
       return false;

     if (m_memory == V4L2_MEMORY_MMAP && !buffer->Map())
       return false;

     m_free.push_back(buffer->GetId());

  }
  CLog::Log(LOGINFO, "CNVV4LBufferPool::Init %d v4l2 buffers initalized", req.count);

  return true;
};


CNVV4LBuffer* CNVV4LBufferPool::GetBuffer()
{
  std::unique_lock<std::mutex> lock(m_pool_mutex);

  if (! m_free.empty()) 
  {
    int buf_id = m_free.front();
    m_free.erase(m_free.begin());

    CNVV4LBuffer* buffer = m_bufs[buf_id];
    buffer->Reset();

    buffer->Acquire(GetPtr());
    
    m_used.push_back(buf_id);

    lock.unlock();

    if (m_free.empty())
      m_pool_wait_full.notify_one();

    return buffer;
  }
  else
    return nullptr;
}

void CNVV4LBufferPool::Return(int id) 
{
  std::unique_lock<std::mutex> lock(m_pool_mutex);
  bool returned = false;

  for (auto it = std::begin(m_used); it < std::end(m_used); it++) {
    if (*it == id)
    {
      returned = true;

      m_used.erase(it);
      m_free.push_back(id);
      break;
    }
  }

  lock.unlock();

  if (returned)
    m_pool_wait_free.notify_one();
}


CNVV4LBuffer* CNVV4LBufferPool::DequeueBuffer()
{
  struct v4l2_buffer buf;
  struct v4l2_plane planes[YuvImage::MAX_PLANES];

  memset(&buf, 0, sizeof(struct v4l2_buffer));
  memset(planes, 0, sizeof(planes));

  buf.type = m_type;
  buf.memory = m_memory;
  buf.m.planes = planes;

  if (v4l2_ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    return nullptr;

  CNVV4LBuffer* nv_buffer = m_bufs[buf.index];
  nv_buffer->Update(buf);
  
  return nv_buffer;
};


CNVV4LBuffer* CNVV4LBufferPool::PeekReadyBuffer()
{
  if (m_ready.empty())
    return nullptr;

  const std::lock_guard<std::mutex> lock(m_pool_mutex);
  if (! m_ready.empty()) {
    return m_bufs[m_ready.front()];
  }

  return nullptr;
}

CNVV4LBuffer* CNVV4LBufferPool::GetReadyBuffer()
{
  if (m_ready.empty())
    return nullptr;

  const std::lock_guard<std::mutex> lock(m_pool_mutex);
  if (! m_ready.empty()) {
    CNVV4LBuffer *buffer = m_bufs[m_ready.front()];
    m_ready.pop();
    
    buffer->Acquire(GetPtr());

    return buffer;
  }

  return nullptr;

}

void CNVV4LBufferPool::Ready(int id)
{
  std::unique_lock<std::mutex> lock(m_pool_mutex);

  m_ready.push(id);
  m_free.erase(std::remove(m_free.begin(), m_free.end(), id), m_free.end());
  m_used.push_back(id);

  lock.unlock();

  m_pool_wait_ready.notify_one();
}

bool CNVV4LBufferPool::WaitForFreeBuffer(int timeout) 
{
  if (HasFreeBuffers())
    return true;

  std::unique_lock<std::mutex> lock(m_pool_mutex);

  return m_pool_wait_free.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return this->HasFreeBuffers(); });
};

bool CNVV4LBufferPool::WaitForReadyBuffer(int timeout) 
{
  if (HasReadyBuffers())
    return true;

  std::unique_lock<std::mutex> lock(m_pool_mutex);

  return m_pool_wait_ready.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return this->HasReadyBuffers(); });
}

bool CNVV4LBufferPool::WaitForFullPool(int timeout)
{
  if (!HasFreeBuffers())
    return true;

  std::unique_lock<std::mutex> lock(m_pool_mutex);

  return m_pool_wait_full.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return !this->HasFreeBuffers(); });
};

bool CNVV4LBufferPool::WaitForEmptyPool(int timeout)
{
  if (FreeCount() == GetSize())
    return true;

  std::unique_lock<std::mutex> lock(m_pool_mutex);

  return m_pool_wait_free.wait_for(lock, std::chrono::milliseconds(timeout), [this] { return FreeCount() == GetSize(); });
};

void CNVV4LBufferPool::Reset() {
  Dispose();

  Init(m_format, m_size);
}

template<typename T>
inline static void clear(T &cont) {
  T empty;
  std::swap(cont, empty);
}

void CNVV4LBufferPool::Dispose()
{
  const std::lock_guard<std::mutex> lock(m_pool_mutex);

  clear(m_free);
  clear(m_ready);
  clear(m_used);

  for (auto buffer : m_bufs) {
    buffer->UnMap();
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(struct v4l2_requestbuffers));

  req.type = m_type;
  req.memory = m_memory;
  req.count = 0;

  if (v4l2_ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
  {
    CLog::Log(LOGERROR, "CNVV4LBufferPool::Dispose v4l2 buffer request failed: %s", strerror(errno));
  }
}

CNVV4LBufferPool::~CNVV4LBufferPool()
{
  for (auto buffer : m_bufs)
  {
    delete buffer;
  }
}

/**
 * CNVV4LBuffer
 */


CNVV4LBuffer::CNVV4LBuffer(int id) : CVideoBuffer(id)
{

};


CNVV4LBuffer::~CNVV4LBuffer() 
{

};

void CNVV4LBuffer::Init(int device_fd, struct v4l2_format format, v4l2_memory memory)
{
  memset(&m_buffer, 0, sizeof(m_buffer));
  memset(m_planes, 0, sizeof(m_planes));
  memset(m_fd_dma, 0, sizeof(m_fd_dma));
  memset(m_data, 0, sizeof(m_data));

  m_device_fd = device_fd;
  m_format = format;
  m_memory = memory;
  m_local = true;

  m_buffer.index = m_id;
  m_buffer.type = m_format.type;
  m_buffer.memory = m_memory;
  m_buffer.length = m_format.fmt.pix_mp.num_planes;
  m_buffer.m.planes = m_planes;

}

bool CNVV4LBuffer::Query() 
{
  if (v4l2_ioctl(m_device_fd, VIDIOC_QUERYBUF, &m_buffer) < 0) 
  {
    CLog::Log(LOGERROR, "CNVV4LBuffer::Query query buffer failed: %s", strerror(errno));
    return false;
  }

  return true;
};

bool CNVV4LBuffer::Map() 
{
  for (size_t i = 0; i < m_buffer.length; ++i) 
  {
    v4l2_plane &plane = m_buffer.m.planes[i];

    m_data[i] = static_cast<uint8_t*>(v4l2_mmap(NULL, 
                          plane.length, 
                          PROT_READ | PROT_WRITE, MAP_SHARED, 
                          m_fd_dma[i], 
                          plane.m.mem_offset));

    if (m_data[i] == MAP_FAILED) 
    {
      CLog::Log(LOGERROR, "CNVV4LBuffer::Map failed to mmap buffer id:%d fd:%d offset:%d : %s", 
                m_id, m_fd_dma[i], plane.m.mem_offset,  strerror(errno));
      return false;
    }
  }

  m_local = true;

  return true;
};


bool CNVV4LBuffer::UnMap()
{
  for (size_t i = 0; i < m_buffer.length; ++i) 
  {
    if (m_data[i])
    {
      v4l2_plane &plane = m_buffer.m.planes[i];

      if (v4l2_munmap(m_data[i], plane.length) < 0)
      {
        CLog::Log(LOGERROR, "CNVV4LBuffer::UnMap failed to unmap buffer id:%d: %s", m_id, strerror(errno));
        return false;
      }

      m_data[i] = nullptr;
      m_fd_dma[i] = 0;
    }
  }

  return true;
}

bool CNVV4LBuffer::Export()
{
  struct v4l2_exportbuffer expbuf;
  memset(&expbuf, 0, sizeof(struct v4l2_exportbuffer));
  expbuf.type = m_format.type;
  expbuf.index = m_id;


  for (size_t i = 0; i < m_buffer.length; ++i)
  {
    expbuf.plane = i;

    if (v4l2_ioctl(m_device_fd, VIDIOC_EXPBUF, &expbuf) < 0)
    {
      CLog::Log(LOGERROR, "CNVV4LBuffer::Export failed to export buffer id:%d : %s", m_id, strerror(errno));
      return false;
    }

    m_fd_dma[i] = expbuf.fd;
  }

  return true;
};


bool CNVV4LBuffer::Enqueue()
{
  if (!m_local)
    return true;

  if (v4l2_ioctl(m_device_fd, VIDIOC_QBUF, &m_buffer) < 0)
  {
    return false;
  }

  m_local = false;

  return true;
}

int CNVV4LBuffer::write(uint8_t* data, size_t len)
{
  size_t write_size = m_planes[0].length < len ? m_planes[0].length : len;

  memcpy(m_data[0], data, write_size);
  m_planes[0].bytesused = write_size;

  return write_size;
}


void CNVV4LBuffer::Reset() 
{
  m_refCount = 0;
  for (size_t i = 0; i < m_buffer.length; ++i) 
  {
    m_planes[i].bytesused = 0;
  }
}


void CNVV4LBuffer::Update(const struct v4l2_buffer &buf)
{
 
  m_local = true;

  m_buffer.field = buf.field;
  m_buffer.flags = buf.flags;
  m_buffer.sequence = buf.sequence;
  m_buffer.timestamp = buf.timestamp;

  for (size_t i = 0; i < buf.length; ++i)
  {
    m_buffer.m.planes[i].bytesused = buf.m.planes[i].bytesused;
  }
}

void CNVV4LBuffer::GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES])
{
  for (size_t i = 0; i < m_buffer.length; ++i)
  {
    planes[i] = m_data[i];
  }
}

void CNVV4LBuffer::GetStrides(int(&strides)[YuvImage::MAX_PLANES])
{
  for (size_t i = 0; i < m_buffer.length; ++i)
  {
    strides[i] = m_format.fmt.pix_mp.plane_fmt[i].bytesperline;
  }
};


void CNVV4LBuffer::SetPts(size_t pts) {
  m_buffer.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
  m_buffer.timestamp.tv_sec = pts; 
};

size_t CNVV4LBuffer::GetPts() {
 return m_buffer.timestamp.tv_sec;
};

AVPixelFormat CNVV4LBuffer::GetFormat() 
{
  // ffmpeg doesn't have AV_PIX_FMT_NV12M which is a bit different from
  // AV_PIX_FMT_NV12. As it is v4l2_m2m format, will use CUDA for now

  return AV_PIX_FMT_CUDA;
};


AVColorSpace KODI::NVV4L::mapColorSpace(const struct v4l2_format &format)
{
  switch (format.fmt.pix_mp.colorspace) {
    case V4L2_COLORSPACE_SMPTE170M:
      return AVCOL_SPC_SMPTE170M;

    case V4L2_COLORSPACE_BT2020:
      return AVCOL_SPC_BT2020_CL;

    case V4L2_COLORSPACE_SMPTE240M:
      return AVCOL_SPC_SMPTE240M;

    case V4L2_COLORSPACE_BT878:
      return AVCOL_SPC_BT709;

    default:
      return AVCOL_SPC_UNSPECIFIED;
  }
          
};
