/* opengothic-rs: C harness for the seam. Runs the game, counts on_frame calls and
 * exits after OG_SMOKE_FRAMES frames (default 300). Exit 0 only if all arrived. */
#include "og_capi.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct { uint64_t calls; uint64_t target; int order_ok; } smoke_state;

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
  smoke_state  st  = {0, env!=NULL ? strtoull(env,NULL,10) : 300, 1};
  if(st.target==0) {
    fprintf(stderr,"og_capi_smoke: OG_SMOKE_FRAMES must be a positive number\n");
    return 2;
    }
  og_callbacks cb  = {&st, on_frame};
  int ret = og_run(argc,argv,&cb);
  fprintf(stderr,"og_capi_smoke: og_run=%d frames=%llu order_ok=%d\n",ret,(unsigned long long)st.calls,st.order_ok);
  return (ret==0 && st.calls>=st.target && st.order_ok) ? 0 : 1;
  }
