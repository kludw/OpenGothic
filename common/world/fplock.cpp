#include "fplock.h"
#include "waypoint.h"

#include "game/serialize.h"
#include "capi/ogsim.h"
#include "capi/ogsimwaynet.h"

// opengothic-rs (task 5.13): every use-counter change goes to the og-sim delegation,
// which hands back the counter to keep (see capi/ogsimwaynet.h).
static void changed(const WayPoint* pt, int32_t delta) {
  if(OgSim::on(OG_SIM_MOD_WAYNET))
    OgSimWaynet::lockChanged(*pt,delta);
  }

FpLock::FpLock() {
  }

FpLock::FpLock(const WayPoint &p)
  :pt(&p){
  pt->useCount++;
  changed(pt,+1);
  }

FpLock::FpLock(const WayPoint *p)
  :pt(p){
  if(pt) {
    pt->useCount++;
    changed(pt,+1);
    }
  }

FpLock::FpLock(FpLock &&other)
  :pt(other.pt){
  other.pt=nullptr;
  }

FpLock::~FpLock() {
  if(pt) {
    pt->useCount--;
    changed(pt,-1);
    }
  }

FpLock &FpLock::operator=(FpLock &&other) {
  if(pt) {
    pt->useCount--;
    changed(pt,-1);
    }
  pt = other.pt;
  other.pt=nullptr;
  return *this;
  }

void FpLock::load(Serialize &fin) {
  fin.read(pt);
  if(pt) {
    pt->useCount++;
    changed(pt,+1);
    }
  }

void FpLock::save(Serialize &fout) const {
  fout.write(pt);
  }
