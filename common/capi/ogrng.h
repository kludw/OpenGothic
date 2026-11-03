#pragma once

#include <cstdint>

class Serialize;

// opengothic-rs: named, seeded random streams (task 5.4, ADR 0021). Every random draw in
// the engine goes through one of these, so a visual or audio draw never shifts gameplay
// and a seed plus a save reproduce the gameplay draws. Thread-safe.
namespace OgRng {
  // Values match OG_RNG_* in og_capi.h.
  enum Stream : uint32_t {
    Gameplay = 0,  // script Hlp_Random (game and menu VMs), damage, fight AI, triggers
    Anim     = 1,  // animation variants that change timing (G2 parade)
    Sound    = 2,  // ambient restart delays, SFX variants
    Visual   = 3,  // dialog gestures, screen shake, particles
    Count    = 4,
    };

  struct Info {
    uint64_t draws      = 0;  // sequential draws (next)
    uint64_t keyedDraws = 0;  // order-independent draws (keyed)
    uint64_t digest     = 0;  // FNV-1a 64 over every sequential draw, in order (HashStart before any)
    uint64_t keyedSum   = 0;  // wrapping sum of keyed draws (order-independent)
    uint64_t stateHash  = 0;  // FNV-1a 64 of the generator state
    };

  // Reseeds every stream from one seed; resets counters and digests. Without a call the
  // streams are seeded from std::random_device at the first draw (logged).
  void     seed(uint64_t seed);
  // Next value of the stream's mt19937; callers must draw in a fixed order.
  uint32_t next(Stream s);
  // A value that depends only on the stream's seed and key, not on draw order: for draws
  // made on worker threads (the parallel animation update reaches SFX variants).
  uint32_t keyed(Stream s, uint64_t key);
  Info     info(Stream s);
  const char* name(Stream s);

  // Save entry "opengothic-rs/rng": restores exactly the states written by save.
  void     save(Serialize& fout);
  void     load(Serialize& fin);

  // FNV-1a 64 helpers for building keys.
  uint64_t hash(uint64_t h, const void* data, uint64_t size);
  constexpr uint64_t HashStart = 0xcbf29ce484222325ull;
  }
