#include "NVRendererGLES.h"

#include "DVDCodecs/Video/NVV4LCodec.h"
#include "ServiceBroker.h"
#include "VideoRenderers/BaseRenderer.h"
#include "VideoRenderers/RenderInfo.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/GLUtils.h"
#include "utils/log.h"
#include "windowing/X11/WinSystemX11GLESContext.h"


#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GLES2/gl2.h>

using namespace KODI::NVV4L;
using namespace KODI::WINDOWING::X11;

extern "C"
{
  EGLImageKHR NvEGLImageFromFd(EGLDisplay display, int dmabuf_fd);
}

CNVV4LRenderer::CNVV4LRenderer()
{
  auto winSystemEGL = dynamic_cast<CWinSystemX11GLESContext*>(CServiceBroker::GetWinSystem());

  if (!winSystemEGL)
  {
    throw std::runtime_error("NVRenderer works only with EGL system");
  }

  m_egl = winSystemEGL->GetEGLDisplay();

  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  eglDestroyImageKHR = 
    (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");

  eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC) eglGetProcAddress("eglCreateSyncKHR");
  eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC) eglGetProcAddress("eglDestroySyncKHR");
  eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC) eglGetProcAddress("eglClientWaitSyncKHR");

  m_textureTarget = GL_TEXTURE_EXTERNAL_OES;
}

CNVV4LRenderer::~CNVV4LRenderer() 
{
  for (size_t i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteTexture(i);
  }
}

bool CNVV4LRenderer::UploadTexture(int index)
{

  CPictureBuffer& buf = m_buffers[index];

  if (buf.loaded)
    return true;

  CNVV4LBuffer* nv_buffer = buf.videoBuffer;
  if (!nv_buffer) {
    return false;
  }

  if (!nv_buffer->HasData())
    return false;

  buf.image = NvEGLImageFromFd(m_egl, nv_buffer->GetDMAFd());

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(m_textureTarget, buf.texture_id);
  glEGLImageTargetTexture2DOES(m_textureTarget, buf.image);

  buf.fence = eglCreateSyncKHR(m_egl, EGL_SYNC_FENCE_KHR, NULL);

  buf.loaded = true;

  return true;
};

void CNVV4LRenderer::DeleteTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];


  if (glIsTexture(buf.texture_id))
  {
    glDeleteTextures(1, &buf.texture_id);
    buf.texture_id = 0;
  }
};

bool CNVV4LRenderer::CreateTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];

  glGenTextures(1, &buf.texture_id);

  glActiveTexture(m_textureTarget);

  glBindTexture(m_textureTarget, buf.texture_id);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(m_textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindTexture(m_textureTarget, 0);

  VerifyGLState();

  return true;
};


bool CNVV4LRenderer::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  if (!picture.videoBuffer) 
    return false;

  if (picture.videoBuffer->GetFormat() != AV_PIX_FMT_CUDA) 
    return false;

  m_fps = fps;
  m_renderOrientation = orientation;
  m_sourceHeight = picture.iHeight;
  m_sourceWidth = picture.iWidth;

  m_clearColour = CServiceBroker::GetWinSystem()->UseLimitedColor() ? (16.0f / 0xff) : 0.0f;

  m_bConfigured = true;
  return true;
};

bool CNVV4LRenderer::ConfigChanged(const VideoPicture& picture)
{
  return picture.videoBuffer && picture.videoBuffer->GetFormat() != AV_PIX_FMT_CUDA;
};

void CNVV4LRenderer::AddVideoPicture(const VideoPicture& picture, int index)
{
  CPictureBuffer &buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CLog::LogF(LOGERROR, "NVRenderer:: unreleased video buffer with id:%d", buf.videoBuffer->GetId()) ;
    ReleaseBuffer(index);
  }

  buf.videoBuffer = dynamic_cast<CNVV4LBuffer*>(picture.videoBuffer);
  buf.loaded = false;
};

void CNVV4LRenderer::UnInit()
{
  CLog::Log(LOGDEBUG, "NVRenderer: Cleaning up GLES resources");

  glFinish();

  // YV12 textures
  for (int i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteTexture(i);
    ReleaseBuffer(i);
  }

  // cleanup framebuffer object if it was in use
  m_bValidated = false;
  m_bConfigured = false;

  CServiceBroker::GetWinSystem()->SetHDR(nullptr);
};

bool CNVV4LRenderer::Flush(bool saveBuffers)
{
  CLog::Log(LOGDEBUG, "NVRenderer:Flush cleaning resources");

  for (size_t i = 0; i < NUM_BUFFERS; ++i)
  {
    DeleteTexture(i);

    if (!saveBuffers)
      ReleaseBuffer(i);
  }

  glFinish();
  m_bValidated = false;

  return false;
};

bool CNVV4LRenderer::IsGuiLayer()
{
  return true;
};

void CNVV4LRenderer::ReleaseBuffer(int idx)
{
  CPictureBuffer &buf = m_buffers[idx];

  if (buf.image != EGL_NO_IMAGE_KHR)
  {
    buf.image = EGL_NO_IMAGE_KHR;
  }

  if (buf.fence != EGL_NO_SYNC_KHR)
  {
    buf.fence = EGL_NO_SYNC_KHR;
  }

  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
};

void CNVV4LRenderer::RenderUpdate (
    int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{

  if (!m_bConfigured)
  {
    return;
  }

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
  {
    return;
  }

  CPictureBuffer& buf = m_buffers[index];

  if (!buf.texture_id)
  {
    return;
  }

  ManageRenderArea();

  if (clear)
  {
    glClearColor(m_clearColour, m_clearColour, m_clearColour, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
  }


  if (!UploadTexture(index))
  {
    return;
  }

  glDisable(GL_BLEND);

  m_shader->Enable();


  // pos_x, pos_y, uv_u, uv_v
  float vertexTexBuf[24] = {
      -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
      -1.0f, -1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f, 1.0f,
  };

  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, vertexTexBuf);
  glEnableVertexAttribArray(m_shader->GetInPosLoc());

  glDrawArrays(GL_TRIANGLES, 0, 6);
  VerifyGLState();

  m_shader->Disable();

  glDisableVertexAttribArray(m_shader->GetInPosLoc());

  glEnable(GL_BLEND);

  eglClientWaitSyncKHR(m_egl, buf.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);

  VerifyGLState();

  eglDestroyImageKHR(m_egl, buf.image);
  eglDestroySyncKHR(m_egl, buf.fence);
};

void CNVV4LRenderer::Update()
{
  if (!m_bConfigured)
  {
    return;
  }

  ManageRenderArea();
};

bool CNVV4LRenderer::RenderCapture(CRenderCapture* capture)
{
  return false;
};

CRenderInfo CNVV4LRenderer::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = NUM_BUFFERS;

  return info;
};


void CNVV4LRenderer::LoadShaders()
{

  if (glCreateProgram())
  {

    m_shader = std::make_unique<CNV4LShader>();

    if (!m_shader->CompileAndLink())
      CLog::Log(LOGERROR, "GLES: Error enabling NV GLSL shader");
  }
}

bool CNVV4LRenderer::ValidateRenderTarget()
{
  if (!m_bValidated)
  {
    // function pointer for texture might change in
    // call to LoadShaders
    glFinish();

    for (int i = 0; i < NUM_BUFFERS; i++)
    {
      DeleteTexture(i);
    }

    LoadShaders();

    for (size_t i = 0; i < m_num_buffers; i++)
    {
      CreateTexture(i);
    }

    m_bValidated = true;

    return true;
  }

  return false;
}

bool CNVV4LRenderer::Supports(ESCALINGMETHOD method)
{
  if (method == VS_SCALINGMETHOD_NEAREST || method == VS_SCALINGMETHOD_LINEAR)
  {
    return true;
  }

  return false;
};

bool CNVV4LRenderer::SupportsMultiPassRendering()
{
  return false;
}

bool CNVV4LRenderer::Supports(ERENDERFEATURE feature)
{
  if (feature == RENDERFEATURE_GAMMA || feature == RENDERFEATURE_NOISE ||
      feature == RENDERFEATURE_SHARPNESS || feature == RENDERFEATURE_NONLINSTRETCH)
  {
    return false;
  }

  if (feature == RENDERFEATURE_STRETCH || feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_VERTICAL_SHIFT || feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_POSTPROCESS || feature == RENDERFEATURE_ROTATION ||
      feature == RENDERFEATURE_BRIGHTNESS || feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_TONEMAP)
  {
    return true;
  }

  return false;
}


CBaseRenderer* CNVV4LRenderer::Create(CVideoBuffer* buffer)
{
  return new CNVV4LRenderer();
}

bool CNVV4LRenderer::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("nv-gles", CNVV4LRenderer::Create);
  return true;
}


CNV4LShader::CNV4LShader()
{
  VertexShader()->LoadSource("nv4l.vert");
  PixelShader()->LoadSource("nv4l.frag");
};

void CNV4LShader::OnCompiledAndLinked()
{

  m_in_pos = glGetAttribLocation(ProgramHandle(), "in_pos");

  VerifyGLState();
}


bool CNV4LShader::OnEnabled()
{
  return true;
}
