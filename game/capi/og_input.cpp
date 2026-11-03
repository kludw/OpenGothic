// opengothic-rs: og_read_input / og_set_input_replay / og_apply_input (game/capi/og_capi.h,
// v18): the input PlayerControl receives, queued for the host and injected back by exact
// replays (task 5.6).

#include "og_capi.h"

#include <Tempest/Log>

#include <cstdint>
#include <deque>
#include <exception>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "game/playercontrol.h"
#include "utils/keycodec.h"

namespace {

// A host that never reads (Gothic2Notr's stub) must not grow the queue forever.
constexpr size_t MaxQueued = 4096;

std::deque<og_input_event> queue;
uint64_t                   dropped = 0;  // since the last og_read_input

void onPlayerInput(int kind, int action, int key, int mapping, float dx, float dy) {
  if(queue.size()>=MaxQueued) {
    queue.pop_front();
    if(dropped++==0)
      Tempest::Log::e("og_input: the host reads no input events, dropping the oldest");
    }
  queue.push_back(og_input_event{uint32_t(kind),uint32_t(action),uint32_t(key),uint32_t(mapping),dx,dy});
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

void OgCapi::inputAttach(bool on) {
  queue.clear();
  dropped                = 0;
  OgHooks::inputReplay   = false;
  OgHooks::onPlayerInput = on ? onPlayerInput : nullptr;
  }

int og_read_input(og_ctx* ctx, og_input_event* out, uint32_t cap, uint32_t* count) {
  return guarded("og_read_input",ctx,[&]{
    if(count==nullptr || (cap>0 && out==nullptr)) {
      Tempest::Log::e("og_read_input: NULL out or count");
      return OG_CTL_BAD_ARGS;
      }
    uint32_t n = 0;
    while(n<cap && !queue.empty()) {
      out[n++] = queue.front();
      queue.pop_front();
      }
    *count = n;
    if(dropped>0) {
      Tempest::Log::e("og_read_input: ",dropped," events were dropped since the last read");
      dropped = 0;
      return OG_CTL_REJECTED;
      }
    return OG_CTL_OK;
    });
  }

int og_set_input_replay(og_ctx* ctx, int on) {
  return guarded("og_set_input_replay",ctx,[&]{
    if(on!=0 && on!=1) {
      Tempest::Log::e("og_set_input_replay: on must be 0 or 1, got ",on);
      return OG_CTL_BAD_ARGS;
      }
    if(OgHooks::inputReplay!=bool(on))
      Tempest::Log::i("og_set_input_replay: real input ",on ? "ignored" : "restored");
    OgHooks::inputReplay = bool(on);
    return OG_CTL_OK;
    });
  }

int og_apply_input(og_ctx* ctx, const og_input_event* e) {
  return guarded("og_apply_input",ctx,[&]{
    if(e==nullptr || OgHooks::playerControl==nullptr) {
      Tempest::Log::e("og_apply_input: NULL event or no player control");
      return OG_CTL_BAD_ARGS;
      }
    const bool keyed = e->kind==OG_INPUT_KEY_DOWN || e->kind==OG_INPUT_KEY_UP;
    if(keyed && (e->action>=KeyCodec::Last || e->mapping>=uint32_t(KeyCodec::NumMappings) ||
                 e->key>uint32_t(Tempest::Event::K_Last))) {
      Tempest::Log::e("og_apply_input: action ",e->action,", key ",e->key," or mapping ",e->mapping," out of range");
      return OG_CTL_BAD_ARGS;
      }
    auto  action  = KeyCodec::Action(e->action);
    auto  mapping = KeyCodec::Mapping(e->mapping);
    auto& pc      = *OgHooks::playerControl;
    OgMuteInput mute;
    switch(e->kind) {
      case OG_INPUT_KEY_DOWN:
        pc.onKeyPressed(action,Tempest::KeyEvent::KeyType(e->key),mapping);
        return OG_CTL_OK;
      case OG_INPUT_KEY_UP:
        pc.onKeyReleased(action,mapping);
        return OG_CTL_OK;
      case OG_INPUT_ROTATE:
        pc.onRotateMouse(e->dx,e->dy);
        return OG_CTL_OK;
      }
    Tempest::Log::e("og_apply_input: unknown kind ",e->kind);
    return OG_CTL_BAD_ARGS;
    });
  }
