#include "og_capi.h"

#include <Tempest/Application>
#include <Tempest/Log>

#include <cstring>
#include <exception>

#include "capi/oghooks.h"
#include "mainwindow.h"
#include "og_capi_internal.h"

int og_capture_frame(og_ctx* ctx, uint32_t max_width, uint8_t* buf, size_t cap, og_frame_info* info) {
  try {
    if(ctx==nullptr || !ctx->inFrame || info==nullptr) {
      Tempest::Log::e("og_capture_frame: called outside on_frame or with a NULL info");
      return OG_CAPTURE_BAD_ARGS;
      }
    *info = og_frame_info{};
    auto* wnd = OgHooks::mainWindow;
    if(wnd==nullptr) {
      Tempest::Log::e("og_capture_frame: no main window");
      return OG_CAPTURE_NO_IMAGE;
      }
    const auto sz = wnd->ogCaptureSize(max_width);
    if(sz.isEmpty())
      return OG_CAPTURE_NO_IMAGE;
    info->width  = uint32_t(sz.w);
    info->height = uint32_t(sz.h);
    const size_t need = size_t(info->width)*info->height*4;
    if(buf==nullptr || cap<need)
      return OG_CAPTURE_TOO_SMALL;

    const uint64_t t0 = Tempest::Application::tickCount();
    const auto     pm = wnd->ogCaptureFrame(max_width);
    if(pm.w()!=info->width || pm.h()!=info->height || pm.format()!=Tempest::TextureFormat::RGBA8) {
      Tempest::Log::e("og_capture_frame: readback is ",pm.w(),"x",pm.h()," format ",int(pm.format()),
                      ", expected ",info->width,"x",info->height," RGBA8");
      *info = og_frame_info{};
      return OG_CAPTURE_READBACK;
      }
    std::memcpy(buf,pm.data(),need);
    Tempest::Log::d("og_capture_frame: ",info->width,"x",info->height," in ",
                    Tempest::Application::tickCount()-t0," ms");
    return OG_CAPTURE_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_capture_frame: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_capture_frame: unknown exception");
    }
  if(info!=nullptr)
    *info = og_frame_info{};
  return OG_CAPTURE_EXCEPTION;
  }
