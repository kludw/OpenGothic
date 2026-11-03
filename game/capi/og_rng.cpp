// opengothic-rs: og_rng_seed / og_rng_read (game/capi/og_capi.h, v16): the host's view of
// the engine's named random streams (common/capi/ogrng.h).

#include "og_capi.h"

#include <Tempest/Log>

#include <exception>

#include "capi/ogrng.h"

static_assert(OG_RNG_GAMEPLAY==OgRng::Gameplay && OG_RNG_ANIM==OgRng::Anim &&
              OG_RNG_SOUND==OgRng::Sound && OG_RNG_VISUAL==OgRng::Visual &&
              OG_RNG_STREAMS==OgRng::Count, "OG_RNG_* must match OgRng::Stream");

void og_rng_seed(uint64_t seed) {
  // Only allocation in the log line can throw; nothing may unwind into the host.
  try {
    OgRng::seed(seed);
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_rng_seed: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_rng_seed: unknown exception");
    }
  }

int og_rng_read(uint32_t stream, og_rng_info* out) {
  try {
    if(stream>=OG_RNG_STREAMS || out==nullptr) {
      Tempest::Log::e("og_rng_read: bad arguments (stream ",stream,")");
      return OG_CTL_BAD_ARGS;
      }
    const auto i = OgRng::info(OgRng::Stream(stream));
    out->draws       = i.draws;
    out->keyed_draws = i.keyedDraws;
    out->digest      = i.digest;
    out->keyed_sum   = i.keyedSum;
    out->state_hash  = i.stateHash;
    return OG_CTL_OK;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("og_rng_read: exception: ",e.what());
    }
  catch(...) {
    Tempest::Log::e("og_rng_read: unknown exception");
    }
  return OG_CTL_EXCEPTION;
  }
