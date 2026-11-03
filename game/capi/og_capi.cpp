#include "og_capi.h"

#include <Tempest/Application>
#include <Tempest/Log>
#include <Tempest/SystemApi>

#include <atomic>
#include <exception>
#include <thread>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "../og_main.h"
#include "mainwindow.h"
#include "game/gamesession.h"
#include "gothic.h"

// Tempest's exec loop runs its timers (Tempest::Timer: chapter screen, game menu,
// inventory) after each event, through Application's private callback object;
// Application::processEvents doesn't. A host-owned loop must run them too, or a
// chapter screen (which pauses the game until its timer closes it) never closes.
// Tempest is upstream's submodule, so instead of a Tempest change: the
// explicit-instantiation access idiom (access checks don't apply to the template
// argument of an explicit instantiation) reaches the private Application::impl.
// Its type is only complete inside Tempest's application.cpp; it is
// `struct Application::Impl : SystemApi::AppCallBack` with that polymorphic class as
// its only base, so (Itanium C++ ABI) the base sits at offset 0 and the object is
// used through it. AppCallBack is protected in SystemApi: named via a derived alias.
struct OgSystemApiAccess : Tempest::SystemApi {
  using AppCallBack = Tempest::SystemApi::AppCallBack;
  };
uint32_t ogRunTempestTimers();
template<auto Impl>
struct OgTempestTimers {
  friend uint32_t ogRunTempestTimers() {
    return reinterpret_cast<OgSystemApiAccess::AppCallBack*>(Impl)->onTimer();
    }
  };
template struct OgTempestTimers<&Tempest::Application::impl>;

namespace {
og_callbacks callbacks = {};
og_ctx       ctx;
// og_main has process-wide statics: one run per process.
bool         started = false;
// Read from any thread. logUser is set before logFn is published and never cleared,
// so a racing caller that saw logFn never sees a stale-null user.
std::atomic<void(*)(void*,int,const char*,const char*)> logFn{nullptr};
std::atomic<void*>                                      logUser{nullptr};

void onFrame() {
  if(callbacks.on_frame!=nullptr) {
    ctx.inFrame = true;
    callbacks.on_frame(callbacks.user,&ctx);
    ctx.inFrame = false;
    }
  ++ctx.frame;
  }

// og_init's game, alive until og_shutdown; NULL under og_run.
OgMain::Engine* engine = nullptr;
// Events delivered per og_pump_events: Tempest's macOS backend takes one per call
// and can't tell an empty queue apart; this bounds the call while keeping up with
// mouse moves at any frame rate the game runs at.
constexpr int   MaxEventsPerPump = 64;

void attach(const og_callbacks* cb) {
  started   = true;
  callbacks = cb!=nullptr ? *cb : og_callbacks{};
  ctx       = og_ctx{};
  OgHooks::onFrame = onFrame;
  OgCapi::inputAttach(true);
  logUser.store(callbacks.user,std::memory_order_release);
  logFn.store(callbacks.on_log,std::memory_order_release);
  }

void detach() {
  OgHooks::onFrame = nullptr;
  OgCapi::inputAttach(false);
  logFn.store(nullptr,std::memory_order_release);
  callbacks        = og_callbacks{};
  }

// Runs fn, turning an escaping C++ exception into OG_RUN_EXCEPTION (logged with `what`).
template<class F>
int guarded(const char* what, F fn) {
  try {
    return fn();
    }
  catch(const std::exception& e) {
    Tempest::Log::e(what,": exception escaped the game: ",e.what());
    }
  catch(...) {
    Tempest::Log::e(what,": unknown exception escaped the game");
    }
  return OG_RUN_EXCEPTION;
  }

// The loop functions' precondition: og_init's ctx, outside every callback.
bool loopReady(const og_ctx* c, const char* what) {
  if(engine==nullptr || c!=&ctx || ctx.inFrame) {
    Tempest::Log::e(what,": no og_init game, a foreign ctx, or called from a callback");
    return false;
    }
  return true;
  }
}

int og_run(int argc, const char** argv, const og_callbacks* cb) {
  if(started) {
    Tempest::Log::e("og_run: already called in this process");
    return OG_RUN_ALREADY_RUNNING;
    }
  attach(cb);

  int ret = OG_RUN_EXCEPTION;
  try {
    ret = og_main(argc,argv);
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_run: exception escaped the game: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_run: unknown exception escaped the game");
    }

  detach();
  return ret;
  }

int og_init(int argc, const char** argv, const og_callbacks* cb, og_ctx** outCtx) {
  if(outCtx!=nullptr)
    *outCtx = nullptr;
  if(started) {
    Tempest::Log::e("og_init: the game already ran in this process");
    return OG_RUN_ALREADY_RUNNING;
    }
  if(outCtx==nullptr) {
    Tempest::Log::e("og_init: out_ctx is NULL");
    return OG_LOOP_BAD_STATE;
    }
  attach(cb);
  const int ret = guarded("og_init",[&]{
    engine = OgMain::init(argc,argv);
    OgMain::window(*engine).ogSetHostDriven(true);
    return 0;
    });
  if(ret!=0) {
    detach();
    return ret;
    }
  Tempest::Log::i("og_init: the host owns the loop");
  *outCtx = &ctx;
  return 0;
  }

int og_pump_events(og_ctx* c) {
  if(!loopReady(c,"og_pump_events"))
    return OG_LOOP_BAD_STATE;
  return guarded("og_pump_events",[]{
    for(int i=0; i<MaxEventsPerPump && Tempest::Application::isRunning(); ++i) {
      Tempest::Application::processEvents();
      ogRunTempestTimers();
      }
    return Tempest::Application::isRunning() ? 1 : 0;
    });
  }

int og_wait_display(og_ctx* c, uint32_t timeoutMs) {
  if(!loopReady(c,"og_wait_display"))
    return OG_LOOP_BAD_STATE;
  return guarded("og_wait_display",[timeoutMs]{
    auto&          wx    = OgMain::window(*engine);
    const uint64_t seen  = wx.ogDisplayRefreshes();
    const uint64_t start = Tempest::Application::tickCount();
    // Spins like Tempest's own exec loop (processEvents, then yield): the display
    // link's refresh arrives as a main-queue block, run from inside processEvents.
    while(Tempest::Application::isRunning()) {
      Tempest::Application::processEvents();
      ogRunTempestTimers();
      if(wx.ogDisplayRefreshes()!=seen)
        return 1;
      if(Tempest::Application::tickCount()-start>=timeoutMs)
        return 0;
      std::this_thread::yield();
      }
    return 0;
    });
  }

int og_step(og_ctx* c, uint32_t dtMs) {
  if(!loopReady(c,"og_step"))
    return OG_LOOP_BAD_STATE;
  return guarded("og_step",[dtMs]{
    const int64_t advanced = OgMain::window(*engine).ogStep(dtMs);
    return advanced<0 ? OG_STEP_TOO_SHORT : int(advanced);
    });
  }

int og_step_fixed(og_ctx* c, uint32_t dtMs, og_clock* clockOut) {
  if(!loopReady(c,"og_step_fixed"))
    return OG_LOOP_BAD_STATE;
  if(dtMs<1 || dtMs>OG_STEP_MAX_DT_MS) {
    Tempest::Log::e("og_step_fixed: dt_ms ",dtMs," is outside 1..",OG_STEP_MAX_DT_MS);
    return OG_STEP_BAD_DT;
    }
  return guarded("og_step_fixed",[dtMs,clockOut]{
    const uint64_t advanced = OgMain::window(*engine).ogStepFixed(dtMs);
    if(clockOut!=nullptr) {
      *clockOut = og_clock{};
      clockOut->world_generation = OgHooks::worldGeneration;
      if(const GameSession* s = Gothic::inst().gameSession()) {
        clockOut->in_session    = 1;
        clockOut->session_ticks = s->tickCount();
        clockOut->world_time_ms = s->time().toInt();
        }
      }
    return int(advanced);
    });
  }

int og_render(og_ctx* c) {
  if(!loopReady(c,"og_render"))
    return OG_LOOP_BAD_STATE;
  return guarded("og_render",[]{
    OgMain::window(*engine).ogDraw();
    return 0;
    });
  }

int og_shutdown(og_ctx* c) {
  if(!loopReady(c,"og_shutdown"))
    return OG_LOOP_BAD_STATE;
  const int ret = guarded("og_shutdown",[]{
    OgMain::shutdown(engine);
    return 0;
    });
  engine = nullptr;
  detach();
  Tempest::Log::i("og_shutdown: done, code ",ret);
  return ret;
  }

uint64_t og_ctx_frame(const og_ctx* c) {
  return c->frame;
  }

void og_request_exit(og_ctx*) {
  Tempest::Log::i("og_request_exit: exit requested by host");
  Tempest::SystemApi::exit();
  }

bool OgCapi::hasLogSink() {
  return logFn.load(std::memory_order_acquire)!=nullptr;
  }

bool OgCapi::log(int level, const char* source, const char* msg) {
  auto fn = logFn.load(std::memory_order_acquire);
  if(fn==nullptr)
    return false;
  fn(logUser.load(std::memory_order_acquire),level,source,msg);
  return true;
  }
