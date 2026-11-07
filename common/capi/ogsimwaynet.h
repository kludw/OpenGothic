#pragma once

// opengothic-rs (task 5.13): the way net's og-sim delegation (OG_SIM_MOD_WAYNET in
// game/capi/og_sim.h). World's constructor hands the way net's inputs to Rust after
// WayMatrix::buildIndex (the world becomes the current one), WayMatrix::wayTo and
// findFreePoint hand their C++ results over, FpLock every use-counter change. Points are
// passed as (list, index) of the current matrix; a point outside it (another world's,
// e.g. an NPC's lock released while its world is torn down) is not the delegation's: a
// lock change on one is skipped (debug log), a query naming one keeps C++'s answer and
// logs an error. Only called while the module is on.

#include <Tempest/Vec>

#include <cstdint>
#include <functional>
#include <string_view>

#include "world/waypath.h"
#include "../../game/capi/og_sim.h"

namespace zenkit { class VfsNode; }

class World;
class WayMatrix;
class WayPoint;

struct OgSimWaynet final {
  static void            loaded(World& world, const WayMatrix& m, const zenkit::VfsNode& zen);
  static void            unloaded(const WayMatrix& m);
  static WayPath         wayTo(const WayMatrix& m, const WayPoint** begin, size_t beginSz,
                               const Tempest::Vec3& exactBegin, const WayPoint& end, WayPath cpp);
  static const WayPoint* findFreePoint(const WayMatrix& m, const Tempest::Vec3& at, std::string_view name,
                                       const std::function<bool(const WayPoint&)>& filter,
                                       const WayPoint* cpp);
  static void            lockChanged(const WayPoint& p, int32_t delta);

    // For the C-linkage filter trampoline.
    static int32_t         callFilter(void* ctx, og_sim_point p);
    // The matrix's point as (list, index); false for a pointer outside it. Also used by
    // the routine module (OgSimRoutine) for the points routines hold.
    static bool            toPoint(const WayMatrix& m, const WayPoint* p, og_sim_point& out);
    // The inverse; false for a point the matrix doesn't have. Also used by OgSaveWaypoints.
    static bool            fromPoint(const WayMatrix& m, og_sim_point p, const WayPoint*& out);

  private:
    static void            load(World& world, const WayMatrix& m, const zenkit::VfsNode& zen);
  };
