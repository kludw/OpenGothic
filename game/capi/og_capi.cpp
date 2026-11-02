#include "og_capi.h"

#include <Tempest/Log>
#include <Tempest/SystemApi>

#include <exception>

#include "capi/oghooks.h"
#include "../og_main.h"

struct og_ctx {
  uint64_t frame = 0;
  };

namespace {
og_callbacks callbacks = {};
og_ctx       ctx;
// og_main has process-wide statics: one run per process.
bool         started = false;

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
