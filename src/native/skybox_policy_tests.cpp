#include "skybox_policy.h"
#include <cassert>
int main() {
 using gdtpc::skybox_fallback_allowed;
 assert(skybox_fallback_allowed(true,true,true,true,false,1000,900));
 assert(!skybox_fallback_allowed(false,true,true,true,false,1000,900));
 assert(!skybox_fallback_allowed(true,false,true,true,false,1000,900));
 assert(!skybox_fallback_allowed(true,true,false,true,false,1000,900));
 assert(!skybox_fallback_allowed(true,true,true,false,false,1000,900));
 assert(!skybox_fallback_allowed(true,true,true,true,true,1000,900));
 assert(!skybox_fallback_allowed(true,true,true,true,false,1000,0));
 assert(!skybox_fallback_allowed(true,true,true,true,false,1000,749));
 assert(!skybox_fallback_allowed(true,true,true,true,false,900,1000));
 assert(skybox_fallback_allowed(true,true,true,true,false,1000,750));
}
