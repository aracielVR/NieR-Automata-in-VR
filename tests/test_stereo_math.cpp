// Checks the stereo matrix maths against the real camera captured from NieR (capture 1).
#include "../src/pure.h"
#include <cstdio>
#include <cmath>
using namespace pure;
int fails=0;
#define EXPECT(c) do{ if(!(c)){printf("FAIL %d %s\n",__LINE__,#c);fails++;} }while(0)
static float maxdiff(const M4&a,const M4&b){float d=0;for(int i=0;i<16;i++)d=fmaxf(d,fabsf(a.m[i]-b.m[i]));return d;}
int main(){
  // NieR SceneBuffer from capture 1 (memory rows)
  M4 V={{0.12913f,0.00087f,-0.99163f,8.0973f, -0.16511f,0.98606f,-0.02063f,-155.82732f, 0.97779f,0.16639f,0.12747f,55.59021f, 0,0,0,1}};
  M4 P={{1.20628f,0,0,0, 0,2.14451f,0,0, 0,0,-1.00001f,-0.1f, 0,0,-1,0}};
  M4 C=m4_rigid_inverse(V);
  EXPECT(m4_is_rigid(V)); EXPECT(m4_is_rigid(C));
  EXPECT(fabsf(C.at(0,3)+81.129f)<0.01f && fabsf(C.at(1,3)-144.398f)<0.01f && fabsf(C.at(2,3)+2.272f)<0.01f);
  printf("V*C-I max %g, Ve-V max %g\n", maxdiff(m4_mul(V,C),m4_identity()), maxdiff(m4_rigid_inverse(m4_mul(C,m4_identity())),V));
  EXPECT(maxdiff(m4_mul(V,C),m4_identity())<5e-3f);
  // identity eye offset reproduces the game's view
  M4 Ve=m4_rigid_inverse(m4_mul(C,m4_identity())); EXPECT(maxdiff(Ve,V)<5e-3f);
  // level camera: forward keeps its heading, becomes horizontal; up becomes +Y; position kept
  M4 L=m4_level_camera(C);
  EXPECT(m4_is_rigid(L)); EXPECT(fabsf(L.at(1,2))<1e-5f); EXPECT(fabsf(L.at(1,1)-1)<1e-5f);
  float fx=-C.at(0,2), fz=-C.at(2,2), h=sqrtf(fx*fx+fz*fz);
  EXPECT(fabsf(-L.at(0,2)-fx/h)<1e-4f && fabsf(-L.at(2,2)-fz/h)<1e-4f);
  EXPECT(L.at(0,3)==C.at(0,3) && L.at(1,3)==C.at(1,3));
  // eye offset: right eye 32 mm to the right in camera space moves the camera along its +X axis
  M4 E=m4_from_pose({0,0,0,1},{0.032f,0,0}); M4 Ce=m4_mul(C,E);
  for(int k=0;k<3;k++) EXPECT(fabsf(Ce.at(k,3)-(C.at(k,3)+0.032f*C.at(k,0)))<1e-4f);
  // a point 10 m straight ahead projects to the centre; with the eye offset it shifts left in NDC
  float ahead[4]={C.at(0,3)-10*C.at(0,2),C.at(1,3)-10*C.at(1,2),C.at(2,3)-10*C.at(2,2),1};
  auto proj=[&](const M4&VV,const M4&PP,float*o){M4 VP=m4_mul(PP,VV);float c[4];for(int i=0;i<4;i++){c[i]=0;for(int k=0;k<4;k++)c[i]+=VP.at(i,k)*ahead[k];}o[0]=c[0]/c[3];o[1]=c[1]/c[3];o[2]=c[2]/c[3];};
  float n0[3],n1[3]; proj(V,P,n0); proj(m4_rigid_inverse(Ce),P,n1);
  EXPECT(fabsf(n0[0])<1e-3f && fabsf(n0[1])<1e-3f && n0[2]>0 && n0[2]<1); EXPECT(n1[0]<-1e-3f);
  // pose -> matrix -> relative pose round trip; quaternion for 90 deg yaw left
  Q q90{0,sinf(0.7853982f),0,cosf(0.7853982f)}; M4 R=m4_from_pose(q90,{1,2,3});
  EXPECT(fabsf(-R.at(0,2)+1)<1e-5f && fabsf(R.at(2,2))<1e-5f); // forward (-Z) -> -X
  M4 rel=m4_relative(R,m4_mul(R,E)); EXPECT(maxdiff(rel,E)<1e-5f);
  // symmetric projection keeps depth row and sets focal lengths
  M4 Ps=m4_symmetric_projection(P,1.5f,1.0f); EXPECT(Ps.at(2,2)==P.at(2,2)&&Ps.at(2,3)==P.at(2,3)&&Ps.at(3,2)==-1&&fabsf(Ps.at(0,0)-0.6667f)<1e-4f);
  // cover box: canted asymmetric eye (l=-1.3,r=1.0,u=1.1,d=-1.2) at 16:9
  float hw,hh; cover_fov(-1.3f,1.0f,1.1f,-1.2f,16.f/9,&hw,&hh); EXPECT(fabsf(hh-1.2f)<1e-6f&&fabsf(hw-1.2f*16/9)<1e-5f);
  cover_fov(-3.0f,1.0f,1.1f,-1.2f,1.0f,&hw,&hh); EXPECT(hw==3.0f&&hh==3.0f);
  printf(fails?"%d FAILURES\n":"stereo math tests passed\n",fails); return fails!=0;
}
