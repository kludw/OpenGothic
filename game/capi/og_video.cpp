#include "og_capi.h"

#include <Tempest/Log>

#include <cstring>
#include <exception>

#include "capi/oghooks.h"
#include "mainwindow.h"
#include "og_capi_internal.h"

int og_video_request(og_ctx* ctx, uint32_t max_width, uint64_t tag) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e("og_video_request: called outside on_frame");
      return OG_CAPTURE_BAD_ARGS;
      }
    auto* wnd = OgHooks::mainWindow;
    if(wnd==nullptr)
      return OG_CAPTURE_NO_IMAGE;
    wnd->ogVideoRequest(max_width,tag);
    return OG_CAPTURE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_video_request: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_video_request: unknown exception");
    }
  return OG_CAPTURE_EXCEPTION;
  }

int og_video_poll(og_ctx* ctx, uint8_t* buf, size_t cap, og_video_frame* info) {
  try {
    if(ctx==nullptr || !ctx->inFrame || info==nullptr) {
      Tempest::Log::e("og_video_poll: called outside on_frame or with a NULL info");
      return OG_CAPTURE_BAD_ARGS;
      }
    *info = og_video_frame{};
    auto* wnd = OgHooks::mainWindow;
    if(wnd==nullptr)
      return OG_VIDEO_NONE;
    const auto* f = wnd->ogVideoPeek();
    if(f==nullptr)
      return OG_VIDEO_NONE;
    info->tag    = f->tag;
    info->width  = uint32_t(f->size.w);
    info->height = uint32_t(f->size.h);
    const size_t need = size_t(info->width)*info->height*4;
    if(f->rgba.size()!=need) {
      Tempest::Log::e("og_video_poll: frame tag ",f->tag," has ",f->rgba.size()," bytes, expected ",need);
      wnd->ogVideoPop();
      *info = og_video_frame{};
      return OG_CAPTURE_READBACK;
      }
    if(buf==nullptr || cap<need)
      return OG_CAPTURE_TOO_SMALL;
    std::memcpy(buf,f->rgba.data(),need);
    wnd->ogVideoPop();
    return OG_CAPTURE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_video_poll: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_video_poll: unknown exception");
    }
  if(info!=nullptr)
    *info = og_video_frame{};
  return OG_CAPTURE_EXCEPTION;
  }
