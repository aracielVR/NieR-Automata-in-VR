#include "../src/pure.h"
#include <cstdio>
#include <cstring>
#include <cmath>
using namespace pure;
static int fails=0;
#define EXPECT_STR(got, want) do{ if(strcmp(got,want)){printf("FAIL %s:%d got [%s] want [%s]\n",__FILE__,__LINE__,got,want);fails++;} }while(0)
#define EXPECT(c) do{ if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);fails++;} }while(0)
int main(){
  char b[256];
  format(b,sizeof b,"%d|%5d|%-5d|%05d|%u|%x|%X|%08x",-42,7,7,-7,4000000000u,255,255,0xbeef); EXPECT_STR(b,"-42|    7|7    |-0007|4000000000|ff|FF|0000beef");
  format(b,sizeof b,"%lld|%llu|%zu|%I64x",(long long)-9000000000LL,18446744073709551615ULL,(size_t)123,(unsigned long long)0x123456789aULL); EXPECT_STR(b,"-9000000000|18446744073709551615|123|123456789a");
  format(b,sizeof b,"%f|%.2f|%.0f|%.3f|%f|%6.2f",1.5,-2.456,2.5,0.0005,-0.0,3.14159); EXPECT_STR(b,"1.5000|-2.46|3|0.001|0.0000|  3.14");
  format(b,sizeof b,"%.2f|%.2f",0.999,1e20); EXPECT_STR(b,"1.00|1e15+");
  format(b,sizeof b,"[%s][%5s][%-5s][%.2s][%c][%%][%s]","ab","ab","ab","abcd",'Z',(const char*)0); EXPECT_STR(b,"[ab][   ab][ab   ][ab][Z][%][(null)]");
  format(b,sizeof b,"%S",L"héllo"); EXPECT_STR(b,"h\xc3\xa9llo");
  format(b,sizeof b,"%p",(void*)0x1234); EXPECT_STR(b,"0x0000000000001234");
  char small[5]; int n=format(small,sizeof small,"abcdefgh"); EXPECT(n==8); EXPECT_STR(small,"abcd");
  float f; EXPECT(parse_float(" -2.75",&f)&&f==-2.75f); EXPECT(parse_float("3",&f)&&f==3.f); EXPECT(!parse_float("x",&f));
  int i; EXPECT(parse_int("0x77",&i)&&i==0x77); EXPECT(parse_int("-12",&i)&&i==-12); EXPECT(parse_int(L"119",&i)&&i==119);
  const char* js="{\n \"file_format_version\": \"1.0.0\",\n \"runtime\" : {\n  \"name\":\"SteamVR\",\n  \"library_path\" : \"..\\\\bin\\\\win64\\\\vrclient_x64.dll\" ,\"functions\":{\"xrNegotiateLoaderRuntimeInterface\":\"myNeg\"}}}";
  char o[260]; EXPECT(json_find_string(js,strlen(js),"library_path",o,sizeof o)); EXPECT_STR(o,"..\\bin\\win64\\vrclient_x64.dll");
  EXPECT(json_find_string(js,strlen(js),"xrNegotiateLoaderRuntimeInterface",o,sizeof o)); EXPECT_STR(o,"myNeg");
  EXPECT(!json_find_string(js,strlen(js),"nope",o,sizeof o));
  // yaw: for a set of angles, check yaw_from_forward reproduces forward direction
  for(int k=-17;k<=18;k++){ float th=k*0.1745329f*1.0f; V3 fwd{-sinf(th)*0.8f,0.6f,-cosf(th)*0.8f}; Q q; EXPECT(yaw_from_forward(fwd,&q));
    V3 r=rotate(q,{0,0,-1}); float h=sqrtf(fwd.x*fwd.x+fwd.z*fwd.z); EXPECT(fabsf(r.x-fwd.x/h)<1e-4f&&fabsf(r.z-fwd.z/h)<1e-4f&&fabsf(r.y)<1e-5f);
    EXPECT(fabsf(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1)<1e-4f); }
  Q q; EXPECT(!yaw_from_forward({0,-1,0},&q));
  // rotate sanity: 90deg about Y maps -Z to -X
  Q r90{0,sinf(0.785398f),0,cosf(0.785398f)}; V3 v=rotate(r90,{0,0,-1}); EXPECT(fabsf(v.x+1)<1e-5f&&fabsf(v.z)<1e-5f);
  printf(fails? "%d FAILURES\n":"all pure tests passed\n",fails); return fails!=0;
}
