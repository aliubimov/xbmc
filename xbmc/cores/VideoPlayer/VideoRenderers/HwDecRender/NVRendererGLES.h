#pragma once

#include "DVDCodecs/Video/NVV4LCodec.h"
#include "VideoRenderers/BaseRenderer.h"
#include "guilib/Shader.h"

#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "system_gl.h"

namespace KODI
{
namespace NVV4L
{

class CNV4LShader : public Shaders::CGLSLShaderProgram
{
public:
  CNV4LShader();

  void OnCompiledAndLinked() override;
  bool OnEnabled() override;
  GLint GetInPosLoc() { return m_in_pos; };

private:
  GLint m_in_pos;
};

class CNVV4LRenderer : public CBaseRenderer
{

public:
  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

  CNVV4LRenderer();
  CNVV4LRenderer(const CNVV4LRenderer &) = delete;

  ~CNVV4LRenderer();

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  bool IsConfigured() override { return m_bConfigured; }
  void AddVideoPicture(const VideoPicture& picture, int index) override;
  void UnInit() override;
  bool Flush(bool saveBuffers) override;
  bool IsGuiLayer() override;
  void ReleaseBuffer(int idx) override;
  void RenderUpdate(
      int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  void Update() override;
  bool RenderCapture(CRenderCapture* capture) override;
  CRenderInfo GetRenderInfo() override;
  bool ConfigChanged(const VideoPicture& picture) override;
  void SetBufferSize(int numBuffers) override { m_num_buffers = numBuffers; }

  // Feature support
  bool SupportsMultiPassRendering() override;
  bool Supports(ERENDERFEATURE feature) override;
  bool Supports(ESCALINGMETHOD method) override;


private:
  size_t m_num_buffers;
  EGLDisplay m_egl;
  GLuint m_textureTarget{0};

  bool m_bConfigured{false};
  bool m_bValidated{false};
  float m_clearColour{0.0f};

  std::unique_ptr<CNV4LShader> m_shader{nullptr};

  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR{nullptr};
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR{nullptr};
  PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR{nullptr};
  PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR{nullptr};
  PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR{nullptr};
  PFNEGLGETSYNCATTRIBKHRPROC eglGetSyncAttribKHR{nullptr};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES{nullptr};


  bool UploadTexture(int index);
  void DeleteTexture(int index);
  bool CreateTexture(int index);
  void LoadShaders();

  bool ValidateRenderTarget();

  struct CPictureBuffer
  {
    GLuint texture_id{0};
    EGLImageKHR image{EGL_NO_IMAGE_KHR};
    EGLSyncKHR fence{EGL_NO_SYNC_KHR};

    CNVV4LBuffer* videoBuffer{nullptr};
    bool loaded{false};
  };

  CPictureBuffer m_buffers[NUM_BUFFERS];
};


}; // namespace NVV4L
}; // namespace KODI
