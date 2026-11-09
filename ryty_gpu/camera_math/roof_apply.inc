// Roof final-view FOV, world-height and camera-local forward distance adjustment. The original call has already built the view.
typedef unsigned long long U64;
typedef int (__attribute__((sysv_abi)) *RayCast)(void*,float*,const float*,const float*);
static int finite_value(float x){return x==x&&x<3.4e38f&&x> -3.4e38f;}
static void roof_apply(float* view,unsigned char* data){
 float* p=(float*)data;
 const float nativeY=view[1];p[2]=view[22];p[6]=nativeY;((unsigned*)data)[7]++;
 unsigned fov=data[0],position=data[12];float desiredFov=p[1],requested=p[4],distance=p[5];
 if(fov&&finite_value(desiredFov)&&desiredFov>=30&&desiredFov<=100)view[22]=desiredFov;
 data[193]=1;p[9]=0;
 if(!position)requested=distance=0;
 if(!finite_value(requested))requested=0;
 if(!finite_value(distance))distance=0;
 float backward=-distance;
 float x=view[4],y=view[5],z=view[6],w=view[7];
 float targetX=view[0]+2*(x*z+w*y)*backward;
 float baseY=nativeY+2*(y*z-w*x)*backward;
 float targetZ=view[2]+(1-2*(x*x+y*y))*backward;
 p[10]=0;
 if(!finite_value(targetX)||!finite_value(baseY)||!finite_value(targetZ))return;
 U64 global=*(U64*)(data+200);
 void* organizer=global?*(void**)global:0;
 void* root=organizer?*(void**)organizer:0;
 if(!root||*(U64*)root!=*(U64*)(data+208))return;
 void* course=*(void**)((unsigned char*)root+0x25b0);
 void* ray=course?(unsigned char*)course+0x660:nullptr;
 RayCast cast=*(RayCast*)(data+224);
 if(!ray||!cast||*(U64*)ray!=*(U64*)(data+216)||!*(void**)((unsigned char*)ray+8))return;
 // Probe the NEW X/Z after distance; ignore extra height when choosing the ray origin.
 float origin[4]={targetX,(baseY>nativeY?baseY:nativeY)+2,targetZ,0},direction[4]={0,-1,0,0},plane[8]={0};
 ((unsigned*)data)[60]++;
 int hit=cast(ray,plane,origin,direction);
 if(hit<0||!finite_value(plane[0])||!finite_value(plane[1])||!finite_value(plane[2])||!finite_value(plane[3])||plane[1]<0.2f||plane[1]>1.01f)return;
 float ground=-(plane[0]*targetX+plane[2]*targetZ+plane[3])/plane[1];
 if(!finite_value(ground)||ground>origin[1]+0.01f||ground<nativeY-50)return;
 float minimum=ground+p[49]/plane[1]-baseY;
 if(!finite_value(minimum))return;
 data[193]=2;p[8]=minimum;p[58]=ground;((unsigned*)data)[61]++;
 if(!position)return; // Read defaults/minimum without changing the native camera.
 float applied=requested<minimum?minimum:requested;
 view[0]=targetX;view[1]=baseY+applied;view[2]=targetZ;p[9]=applied;p[10]=distance;
 if(applied>requested)data[193]=3;
 // Preserve native orientation, native look-back and all unrelated fields.
}
