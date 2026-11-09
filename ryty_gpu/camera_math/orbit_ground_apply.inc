#include "orbit_motion.h"
#include "orbit_ground.h"
typedef long long i64;
typedef int __attribute__((ms_abi)) (*QueryClock)(i64*);
static void orbit_apply(float* v,unsigned char* b,const float* anchor,const float* native){
 if(!b[40]){ground_apply(v,b,anchor,native);return;}
 ((unsigned*)b)[21]++;
 const volatile unsigned* sequence=(const volatile unsigned*)(b+96);
 unsigned version=*sequence;if(version&1)return;
 const volatile float* p=(const volatile float*)b;
 float c=p[11],s=p[12],ch=p[13],sh=p[14],cp=p[17],sp=p[18],hc=p[19],hs=p[20];
 unsigned pitch_enabled=b[65];
 if(b[100]){
  QueryClock clock=*(QueryClock*)(b+128);i64 frequency=*(i64*)(b+136),now=0;
  if(!clock||frequency<=0||!clock(&now))return;
  i64 previous=*(i64*)(b+144),input=*(i64*)(b+176);
  float dt=previous&&now>=previous?(float)((double)(now-previous)/(double)frequency):0;
  unsigned active=b[127]&&input>0&&now>=input&&((double)(now-input)/(double)frequency)<0.2;
  MotionState state={p[38],p[39],p[40],p[41],p[42],b[184]};
  MotionConfig config={p[26],p[27],p[28],p[29],p[30],b[124],b[125],b[126]};
  motion_step(&state,&config,dt,active);
  motion_sincos(state.yaw,&s,&c);motion_sincos(state.yaw*0.5f,&sh,&ch);
  motion_sincos(state.pitch,&sp,&cp);motion_sincos(state.pitch*0.5f,&hs,&hc);pitch_enabled=1;
  if(*sequence!=version)return;
  *(i64*)(b+144)=now;
  float* writable=(float*)b;writable[38]=state.yaw;writable[39]=state.pitch;
  writable[40]=state.vx;writable[41]=state.vy;writable[42]=state.idle;b[184]=state.returning;
 }
 float x=v[0]-anchor[0],y=v[1]-anchor[1],z=v[2]-anchor[2];
 float rx=c*x+s*z,ry=y,rz=-s*x+c*z;
 float qx=v[4],qy=v[5],qz=v[6],qw=v[7];
 float nx=ch*qx+sh*qz,ny=ch*qy+sh*qw,nz=ch*qz-sh*qx,nw=ch*qw-sh*qy;
 if(pitch_enabled){
  float ax=1-2*(ny*ny+nz*nz),ay=2*(nx*ny+nw*nz),az=2*(nx*nz-nw*ny);
  float dot=ax*rx+ay*ry+az*rz;
  float px=rx*cp+(ay*rz-az*ry)*sp+ax*dot*(1-cp);
  float py=ry*cp+(az*rx-ax*rz)*sp+ay*dot*(1-cp);
  float pz=rz*cp+(ax*ry-ay*rx)*sp+az*dot*(1-cp);
  rx=px;ry=py;rz=pz;
  float tx=nx*hc+nw*hs,ty=ny*hc+nz*hs,tz=nz*hc-ny*hs,tw=nw*hc-nx*hs;
  nx=tx;ny=ty;nz=tz;nw=tw;
 }
 if(*sequence!=version)return;
 v[0]=anchor[0]+rx;v[1]=anchor[1]+ry;v[2]=anchor[2]+rz;
 v[4]=nx;v[5]=ny;v[6]=nz;v[7]=nw;
 ground_apply(v,b,anchor,native);
}
