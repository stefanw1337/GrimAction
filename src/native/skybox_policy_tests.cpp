#include "skybox_policy.h"
#include <cassert>
int main() {
 using gdtpc::skybox_fallback_allowed;
 assert(gdtpc::skybox_radius_scale(120,170,170)*170<120);
 assert(gdtpc::skybox_radius_scale(250,170,170)==1);
 assert(gdtpc::skybox_radius_scale(120,1000,150)*1000<120);
 assert(gdtpc::skybox_radius_scale(0,170,170)==0);
 constexpr char path[]="records/northshire/environment/day_sky.dbr";
 assert(gdtpc::northshire_sky_record(path,sizeof(path)-1));
 assert(gdtpc::northshire_sky_record("Records/Northshire/Environment/DAY_SKY.DBR",sizeof(path)-1));
 assert(!gdtpc::northshire_sky_record(path,sizeof(path)));
 assert(!gdtpc::northshire_sky_record(nullptr,sizeof(path)-1));
 assert(!gdtpc::northshire_sky_record("records/level art/terrain/skybox_grey01a.dbr",41));
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
