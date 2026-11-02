#include "og_capi.h"

#include <Tempest/Log>
#include <Tempest/SystemApi>

#include <atomic>
#include <exception>

#include "capi/oghooks.h"
#include "og_capi_internal.h"
#include "../og_main.h"

struct og_ctx {
  uint64_t frame = 0;
  };

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
  if(callbacks.on_frame!=nullptr)
    callbacks.on_frame(callbacks.user,&ctx);
  ++ctx.frame;
  }
}

int og_run(int argc, const char** argv, const og_callbacks* cb) {
  if(started) {
    Tempest::Log::e("og_run: already called in this process");
    return OG_RUN_ALREADY_RUNNING;
    }
  started   = true;
  callbacks = cb!=nullptr ? *cb : og_callbacks{};
  ctx       = og_ctx{};
  OgHooks::onFrame = onFrame;
  logUser.store(callbacks.user,std::memory_order_release);
  logFn.store(callbacks.on_log,std::memory_order_release);

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

  OgHooks::onFrame = nullptr;
  logFn.store(nullptr,std::memory_order_release);
  callbacks        = og_callbacks{};
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
