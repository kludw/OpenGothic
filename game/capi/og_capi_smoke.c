/* opengothic-rs: C harness for the seam. Runs the game, counts on_frame calls and
 * exits after OG_SMOKE_FRAMES frames (default 300). Exit 0 only if all frames
 * arrived in order and on_log received lines with valid levels. */
#include "og_capi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  uint64_t    calls;
  uint64_t    target;
  int         order_ok;
  atomic_uint log_lines;   /* on_log runs on any thread */
  atomic_uint log_bad;
  } smoke_state;

static void on_log(void* user, int level, const char* source, const char* msg) {
  smoke_state* st = (smoke_state*)user;
  if(level<OG_LOG_ERROR || level>OG_LOG_TRACE || source==NULL || msg==NULL)
    atomic_fetch_add(&st->log_bad,1);
  atomic_fetch_add(&st->log_lines,1);
  }

static void on_frame(void* user, og_ctx* ctx) {
  smoke_state* st = (smoke_state*)user;
  if(og_ctx_frame(ctx)!=st->calls)
    st->order_ok = 0;
  st->calls++;
  if(st->calls%100==0)
    fprintf(stderr,"og_capi_smoke: frame %llu\n",(unsigned long long)st->calls);
  if(st->calls==st->target)
    og_request_exit(ctx);
  }

int main(int argc, const char** argv) {
  const char*  env = getenv("OG_SMOKE_FRAMES");
  /* static: on_log calls may still finish after og_run returns (og_capi.h). */
  static smoke_state st;
  st.target   = env!=NULL ? strtoull(env,NULL,10) : 300;
  st.order_ok = 1;
  if(st.target==0) {
    fprintf(stderr,"og_capi_smoke: OG_SMOKE_FRAMES must be a positive number\n");
    return 2;
    }
  og_callbacks cb  = {&st, on_frame, on_log};
  int ret = og_run(argc,argv,&cb);
  unsigned lines = atomic_load(&st.log_lines), bad = atomic_load(&st.log_bad);
  fprintf(stderr,"og_capi_smoke: og_run=%d frames=%llu order_ok=%d log_lines=%u log_bad=%u\n",
          ret,(unsigned long long)st.calls,st.order_ok,lines,bad);
  return (ret==0 && st.calls>=st.target && st.order_ok && lines>0 && bad==0) ? 0 : 1;
  }
