// opengothic-rs: og_save / og_poll_save (game/capi/og_capi.h): the quick save path
// (Gothic::save -> MainWindow::saveGame) with a file name of the host's choosing.

#include "og_capi.h"

#include <Tempest/Log>

#include <algorithm>
#include <cstring>
#include <exception>
#include <string_view>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "ui/dialogmenu.h"
#include "world/world.h"
#include "gothic.h"

namespace {

void write(char* out, size_t cap, std::string_view msg) {
  if(out==nullptr || cap==0)
    return;
  const size_t n = std::min(cap-1, msg.size());
  std::memcpy(out,msg.data(),n);
  out[n] = '\0';
  }

template<class F>
int guarded(const char* name, og_ctx* ctx, F&& f) {
  try {
    if(ctx==nullptr || !ctx->inFrame) {
      Tempest::Log::e(name,": called outside on_frame");
      return OG_CTL_BAD_ARGS;
      }
    return f();
    }
  catch(const std::exception& e) {
    Tempest::Log::e(name,": exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e(name,": unknown exception");
    }
  return OG_CTL_EXCEPTION;
  }

}

int og_save(og_ctx* ctx, const char* path, char* reason, size_t cap) {
  return guarded("og_save",ctx,[&]{
    if(path==nullptr || path[0]=='\0' || (cap>0 && reason==nullptr)) {
      Tempest::Log::e("og_save: empty path or NULL reason buffer");
      return OG_CTL_BAD_ARGS;
      }
    auto& gothic = Gothic::inst();
    if(gothic.checkLoading()!=Gothic::LoadState::Idle || gothic.world()==nullptr) {
      write(reason,cap,"no world to save (menu, loading or saving)");
      return OG_CTL_NOT_IN_GAME;
      }
    // The conditions MainWindow::saveGame refuses on (silently), named for the host.
    if(OgHooks::dialogMenu!=nullptr && OgHooks::dialogMenu->isActive()) {
      write(reason,cap,"the game cannot save during a dialog");
      Tempest::Log::i("og_save: refused (dialog) for ",path);
      return OG_CTL_REJECTED;
      }
    if(gothic.world()->currentCs()!=nullptr) {
      write(reason,cap,"the game cannot save during a cutscene");
      Tempest::Log::i("og_save: refused (cutscene) for ",path);
      return OG_CTL_REJECTED;
      }
    gothic.save(path,"OpenGothic-RS");
    if(gothic.checkLoading()!=Gothic::LoadState::Saving) {
      write(reason,cap,"the game did not start saving");
      Tempest::Log::e("og_save: Gothic::save did not start for ",path);
      return OG_CTL_REJECTED;
      }
    Tempest::Log::i("og_save: saving to ",path);
    return OG_CTL_OK;
    });
  }

int og_poll_save(og_ctx* ctx, og_save_status* out) {
  return guarded("og_poll_save",ctx,[&]{
    if(out==nullptr) {
      Tempest::Log::e("og_poll_save: NULL out");
      return OG_CTL_BAD_ARGS;
      }
    out->busy     = Gothic::inst().checkLoading()!=Gothic::LoadState::Idle ? 1 : 0;
    out->failures = OgHooks::saveFailures;
    return OG_CTL_OK;
    });
  }
