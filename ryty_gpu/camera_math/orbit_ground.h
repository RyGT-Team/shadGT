#ifndef ORBIT_GROUND_H
#define ORBIT_GROUND_H
// Verified SysV interface: RunWayRayCastAdapter::cast(this, plane/material output,
// ray origin, ray direction). Output begins with normalized world plane nx,ny,nz,d.
typedef int (__attribute__((sysv_abi)) *GroundRay)(void*,float*,const float*,const float*);
static inline float ground_sqrt(float x){return __builtin_sqrtf(x);}
static inline unsigned ground_finite(float x){return x==x&&x<3.4e38f&&x> -3.4e38f;}
static inline float ground_atan(float x){
 // atan for |x|<=1, error below 0.000012 rad. Reduction covers our <=65 degree orbit.
 float a=x<0?-x:x,z=a*a;
 float p=a*(0.99997726f+z*(-0.33262347f+z*(0.19354346f+z*(-0.11643287f+z*(0.05265332f+z*(-0.01172120f))))));
 return x<0?-p:p;
}
static inline void ground_restore(float* v,const float* native,unsigned char* b){
 for(unsigned i=0;i<8;++i)v[i]=native[i];
 if(b[100]){float* p=(float*)b;p[38]=p[39]=p[40]=p[41]=0;b[184]=0;}
}
static void ground_apply(float* v,unsigned char* b,const float* anchor,const float* native){
 if(!b[192])return;
 b[193]=1;((float*)b)[59]=0;
 unsigned long long global=*(unsigned long long*)(b+200);
 if(!global){ground_restore(v,native,b);return;}
 void* organizer=*(void**)global;
 void* root=organizer?*(void**)organizer:0;
 if(!root||*(unsigned long long*)root!=*(unsigned long long*)(b+208)){ground_restore(v,native,b);return;}
 void* course=*(void**)((unsigned char*)root+0x25b0);
 void* ray=course?(unsigned char*)course+0x660:0;
 GroundRay cast=*(GroundRay*)(b+224);
 if(!ray||!cast||*(unsigned long long*)ray!=*(unsigned long long*)(b+216)||!*(void**)((unsigned char*)ray+8)){
  ground_restore(v,native,b);return;
 }
 float origin[4]={v[0],(v[1]>anchor[1]?v[1]:anchor[1])+2.0f,v[2],0};
 float direction[4]={0,-1,0,0},plane[8]={0};
 ((unsigned*)b)[60]++;
 int hit=cast(ray,plane,origin,direction);
 // Reject no-hit, walls, ceilings, corrupt results and remote lower track layers.
 if(hit<0||!ground_finite(plane[0])||!ground_finite(plane[1])||!ground_finite(plane[2])||!ground_finite(plane[3])||plane[1]<0.2f||plane[1]>1.01f){ground_restore(v,native,b);return;}
 float height=-(plane[0]*v[0]+plane[2]*v[2]+plane[3])/plane[1];
 if(!ground_finite(height)||height>origin[1]+0.01f||height<anchor[1]-50){ground_restore(v,native,b);return;}
 ((unsigned*)b)[61]++;((float*)b)[58]=height;b[193]=2;
 float floor=height+((float*)b)[49]/plane[1];
 if(v[1]>=floor)return;
 float rx=v[0]-anchor[0],ry=v[1]-anchor[1],rz=v[2]-anchor[2],newY=floor-anchor[1];
 float oldLength=ground_sqrt(rx*rx+ry*ry+rz*rz),newLength=ground_sqrt(rx*rx+newY*newY+rz*rz);
 if(oldLength>0.001f&&newLength>0.001f){
  // Shortest quaternion rotates the old pivot radius to the constrained radius,
  // preserving framing while lifting the camera above the road.
  float x=rz*(ry-newY),z=rx*(newY-ry),w=oldLength*newLength+rx*rx+ry*newY+rz*rz;
  float norm=ground_sqrt(x*x+z*z+w*w);
  if(norm>0.000001f){
   x/=norm;z/=norm;w/=norm;
   float qx=v[4],qy=v[5],qz=v[6],qw=v[7];
   v[4]=w*qx+x*qw-z*qy;v[5]=w*qy+z*qx-x*qz;v[6]=w*qz+z*qw+x*qy;v[7]=w*qw-x*qx-z*qz;
   if(b[100]&&w>0.000001f){
    // Prevent input wind-up at the boundary; reversing the stick moves immediately.
    float sn=ground_sqrt(x*x+z*z),t=sn/w;
    float angle=t<=1?ground_atan(t):1.5707963267948966f-ground_atan(1/t);
    float* p=(float*)b;p[39]-=angle*114.59155902616465f;
    if(p[39]< -65)p[39]=-65;if(p[39]>65)p[39]=65;
    if(p[41]>0)p[41]=0;
   }
  }
 }
 ((float*)b)[59]=floor-v[1];v[1]=floor;b[193]=3;
}
#endif
