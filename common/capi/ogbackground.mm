// opengothic-rs (focus-fix): og_set_background (og_capi.h v21). Every game launch through
// our binary (tests, harnesses, scheduled and play runs) used to take the keyboard focus
// from whatever the user was doing: Tempest's MacOSApi constructor sets the Regular
// activation policy and calls activateIgnoringOtherApps:YES (macosapi.mm:370-371). Tempest
// is upstream's own submodule and stays untouched (as for the Tempest timers), so both
// calls are redirected here, before Tempest's Application exists: setActivationPolicy:
// keeps the policy an unbundled executable starts with, and activate does nothing.
// Redirecting only activate and forcing Accessory was not enough: AppKit still activated
// the app once at start-up (focus-check: 1 of 22 samples "gothic", ~3 s in). An app on
// the unbundled default policy is never activated; its windows still show, and
// makeKeyAndOrderFront/orderFrontRegardless (macosapi.mm:282, :305) still put the window
// on screen, which is what keeps frames coming (CLAUDE.md, "Keep the display awake").
#include <TargetConditionals.h>

#include "../../game/capi/og_capi.h"

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#include <objc/runtime.h>

namespace {

bool background = false;

void noActivateIgnoring(id, SEL, BOOL) {}
void noActivate(id, SEL) {}

BOOL keepPolicy(id, SEL, NSApplicationActivationPolicy) {
  return YES;
  }

// method_setImplementation on the class that declares `sel`; false when it has none.
bool redirect(Class cls, SEL sel, IMP to) {
  Method m = class_getInstanceMethod(cls, sel);
  if(m==nullptr)
    return false;
  method_setImplementation(m, to);
  return true;
  }

}

extern "C" int og_set_background(int on) {
  if(background)
    return 1;
  if(on==0)
    return 0;
  Class app = [NSApplication class];
  // Checked before anything is redirected, so a failure changes nothing.
  if(class_getInstanceMethod(app, @selector(activateIgnoringOtherApps:))==nullptr ||
     class_getInstanceMethod(app, @selector(setActivationPolicy:))==nullptr)
    return -1;
  redirect(app, @selector(activateIgnoringOtherApps:), reinterpret_cast<IMP>(noActivateIgnoring));
  redirect(app, @selector(setActivationPolicy:), reinterpret_cast<IMP>(keepPolicy));
  // -[NSApplication activate] exists from macOS 14 on; Tempest doesn't call it today.
  redirect(app, @selector(activate), reinterpret_cast<IMP>(noActivate));
  background = true;
  return 1;
  }

#else
// Other Apple targets (iOS): no NSApplication.
extern "C" int og_set_background(int) { return 0; }
#endif
