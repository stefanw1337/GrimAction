#include "cursor_visibility_model.h"
#include <cassert>
int main() {
    using namespace gdtpc;
    CursorVisibilityOverlay p;
    assert(!p.step(false,true).has_value());
    assert(p.step(true,true)==false);
    for(int i=0;i<1000;++i) assert(!p.step(true,false).has_value());
    assert(p.step(true,true)==false); // re-hide only if something actually shows it
    assert(p.step(false,false)==true); // F8 off, menu, NPC, Alt or focus loss
    assert(!p.step(false,true).has_value());
    assert(!p.step(true,false).has_value());
    assert(p.step(false,false)==false); // preserve native hidden state
    assert(p.step(true,true)==false);
    assert(!p.native_request(true,true));
    assert(p.step(false,false)==true);
    assert(p.step(true,true)==false);
    assert(!p.native_request(false,true));
    assert(p.step(false,false)==false);
    assert(cursor_capture_fresh(true,true,true,1000,900));
    assert(!cursor_capture_fresh(false,true,true,1000,900));
    assert(!cursor_capture_fresh(true,false,true,1000,900));
    assert(!cursor_capture_fresh(true,true,false,1000,900));
    assert(!cursor_capture_fresh(true,true,true,1300,900));
    assert(!cursor_capture_fresh(true,true,true,800,900));
}
