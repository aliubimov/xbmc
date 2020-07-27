#pragma once

#include "DVDVideoCodec.h"
#include "Process/ProcessInfo.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "utils/BitstreamConverter.h"

#include <libavutil/pixfmt.h>
#include <linux/videodev2.h>
#include <libv4l2.h>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <queue>
#include <thread>


namespace KODI {
namespace NVV4L {

constexpr const int PTS_MAX = 100;
constexpr const int INPUT_BUFFERS = 12;
constexpr const int EXTRA_OUTPUT_BUFFERS = 5;
constexpr const size_t BUFFER_SIZE = 5000000;

class CNVV4LBuffer;
class CNVV4LBufferPool;

class NVV4LCodec : public CDVDVideoCodec
{
public: 
  NVV4LCodec(CProcessInfo &processInfo);
  ~NVV4LCodec();

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  bool Reconfigure(CDVDStreamInfo &hints) override { return false; }
  bool AddData(const DemuxPacket &packet) override;
  unsigned int GetAllowedReferences() override { return 8; };

  void SetCodecControl(int flags) override { m_coder_control_flag = flags; };

  void Reset() override;
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture) override;

  const char* GetName() override { return "ndec"; }


  void Close();

  static void Register();
  static CDVDVideoCodec* Create(CProcessInfo &processInfo);

private:
  const char* m_dec_dev;
  std::string m_codec;
  std::unique_ptr<CBitstreamConverter> m_bitconverter{nullptr};
  double m_pts[PTS_MAX];
  double m_dts[PTS_MAX];
  size_t m_ipts{0};

  uint32_t m_coding_type;

  int m_coder_control_flag{0};

  int device_fd{0};
  std::shared_ptr<CNVV4LBufferPool> m_pool_output{nullptr};
  std::shared_ptr<CNVV4LBufferPool> m_pool_capture{nullptr};

  struct v4l2_format output_format;
  std::thread m_decoder_thread;

  std::mutex queue_lock;
  std::atomic_bool m_is_open{false};
  std::atomic_bool m_is_capturing{false};
  std::atomic_bool m_preroll{true};
  std::atomic_bool m_eos{false};

  bool m_flushed{false};

  bool OpenDevice();
  bool SubscribeEvent(uint32_t type, uint32_t id, uint32_t flags);
  bool DequeueEvent(struct v4l2_event *ev);
  bool QueryCaptureFormat(struct v4l2_format *format);

  bool StreamOn(uint32_t type);
  bool StreamOff(uint32_t type);

  void DecoderLoop();
  void HandleEvent();
  void HandleOutputPool();
  void HandleCapturePool();

  void DispatchOutput();
  void DispatchCapture();

  void EnableInterrupt();
  void DisabeInterrupt();
};


class CNVV4LBuffer : public CVideoBuffer
{
public:
  CNVV4LBuffer(int id);
  CNVV4LBuffer(const CNVV4LBuffer &) = delete;
  ~CNVV4LBuffer();

  void Init(int device_fd, struct v4l2_format format, v4l2_memory memory);

  bool Query();
  bool Export();
  bool Map();
  bool Enqueue();
  void Reset();
  bool UnMap();

  void Update(const struct v4l2_buffer &buf);

  int write(uint8_t* data, size_t len);


  void SetPts(size_t pts);
  size_t GetPts();

  int GetId() { return m_id; };

  virtual AVPixelFormat GetFormat() override;
  virtual uint8_t* GetMemPtr() override{ return nullptr; };
  virtual void GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES]) override;
  virtual void GetStrides(int(&strides)[YuvImage::MAX_PLANES]) override;
  virtual void SetDimensions(int width, int height, const int (&strides)[YuvImage::MAX_PLANES]) override {};
  virtual void SetDimensions(int width, int height, const int (&strides)[YuvImage::MAX_PLANES], const int (&planeOffsets)[YuvImage::MAX_PLANES]) override {};

  int GetDMAFd() { return m_fd_dma[0]; };
  int GetField() { return m_buffer.field; };
  bool HasData() { return m_data[0] != nullptr; };

private:
  int m_device_fd{-1};
  struct v4l2_format m_format;
  v4l2_memory m_memory;

  bool m_local{true};

  struct v4l2_buffer m_buffer;
  struct v4l2_plane m_planes[YuvImage::MAX_PLANES];

  uint8_t* m_data[YuvImage::MAX_PLANES];
  int m_fd_dma[YuvImage::MAX_PLANES];
};

class CNVV4LBufferPool : public IVideoBufferPool
{
public:
  CNVV4LBufferPool(int fd, uint32_t type, v4l2_memory memory);
  CNVV4LBufferPool(const CNVV4LBufferPool &) = delete;

  ~CNVV4LBufferPool();

  virtual bool IsConfigured() override { return m_size > 0; };
  virtual bool IsCompatible(AVPixelFormat format, int size) override { return false; };

  bool Init(struct v4l2_format format, size_t size);
  void Reset();
  void Dispose();

  int GetDeviceFd() { return m_fd; };
  v4l2_memory GetMmemoryModel() { return m_memory; };
  const struct v4l2_format &GetFormat() { return m_format; };


  CVideoBuffer* Get() override { return GetBuffer(); };
  CNVV4LBuffer* GetBuffer();

  CNVV4LBuffer* PeekReadyBuffer();
  CNVV4LBuffer* GetReadyBuffer();

  CNVV4LBuffer* DequeueBuffer();

  void Ready(int id);
  void Return(int id) override;

  bool HasFreeBuffers() { return !m_free.empty(); };
  bool HasReadyBuffers() { return !m_ready.empty(); };

  const size_t GetSize() { return m_size; };
  const size_t ReadyCount() { return m_ready.size(); }; 
  const size_t FreeCount() { return m_free.size(); }; 
  const size_t UsedCount() { return m_used.size(); }; 

  bool WaitForFreeBuffer(int timeout);
  bool WaitForReadyBuffer(int timeout);
  bool WaitForFullPool(int timeout);
  bool WaitForEmptyPool(int timeout);

private:
  int m_fd{0};
  uint32_t m_type;
  v4l2_memory m_memory;
  size_t m_size{0};
  struct v4l2_format m_format;

  std::mutex m_pool_mutex;
  std::condition_variable m_pool_wait_free;
  std::condition_variable m_pool_wait_ready;
  std::condition_variable m_pool_wait_full;
  
  std::vector<CNVV4LBuffer*> m_bufs;

  std::vector<int> m_free;
  std::vector<int> m_used;
  std::queue<int> m_ready;
};


AVColorSpace mapColorSpace(const struct v4l2_format &format);

}; // namespace NVV4L
}; // namespace KODI


