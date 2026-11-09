#ifndef ORBIT_MOTION_H
#define ORBIT_MOTION_H
typedef struct { float yaw,pitch,vx,vy,idle; unsigned returning; } MotionState;
typedef struct { float x,y,speed,smoothing_ms,delay; unsigned auto_return,invert_x,invert_y; } MotionConfig;
static inline float motion_abs(float x){return x<0?-x:x;}
static inline float motion_wrap(float x){while(x>=360)x-=360;while(x<0)x+=360;if(x>=360)x=0;return x;}
static inline float motion_alpha(float dt,float tau){
 if(tau<=0)return 1;
 float x=dt/tau;
 // Stable exponential approximation. Return smoothing always uses tau 0.25 s.
 return 1-1/(1+x+x*x*0.5f+x*x*x*(1.0f/6.0f));
}
static inline void motion_step(MotionState* s,const MotionConfig* c,float dt,unsigned active){
 if(dt<0)dt=0;if(dt>0.05f)dt=0.05f;
 if(!active){s->vx=s->vy=0;s->returning=0;return;}
 float x=c->invert_x?c->x:-c->x,y=c->invert_y?c->y:-c->y;
 unsigned moving=x!=0||y!=0;
 if(moving){s->idle=0;s->returning=0;}else s->idle+=dt;
 if(!moving&&c->auto_return&&s->idle>=c->delay){
  s->returning=1;s->vx=s->vy=0;
  float delta=s->yaw>180?360-s->yaw:-s->yaw;
  float a=motion_alpha(dt,0.25f);
  s->yaw=motion_wrap(s->yaw+delta*a);s->pitch-=s->pitch*a;
  // Sub-pixel settling threshold, after the smooth return has effectively finished.
  if(motion_abs(delta)<0.01f&&motion_abs(s->pitch)<0.01f){s->yaw=s->pitch=0;s->returning=0;}
  return;
 }
 s->returning=0;
 float a=motion_alpha(dt,c->smoothing_ms*0.001f);
 float tx=x*c->speed,ty=y*c->speed,tau=c->smoothing_ms*0.001f;
 float dx=tx*dt+(s->vx-tx)*tau*a,dy=ty*dt+(s->vy-ty)*tau*a;
 s->vx+=(tx-s->vx)*a;s->vy+=(ty-s->vy)*a;
 if(!moving&&motion_abs(s->vx)<0.01f&&motion_abs(s->vy)<0.01f)s->vx=s->vy=0;
 s->yaw=motion_wrap(s->yaw+dx);s->pitch+=dy;
 if(s->pitch>65){s->pitch=65;if(s->vy>0)s->vy=0;}
 if(s->pitch< -65){s->pitch=-65;if(s->vy<0)s->vy=0;}
}
static inline void motion_sincos(float degrees,float* sine,float* cosine){
 float d=motion_wrap(degrees);if(d>180)d-=360;
 float x=d*0.01745329251994329577f,sign=1;
 if(x>1.5707963267948966f){x=3.1415926535897932f-x;sign=-1;}
 else if(x< -1.5707963267948966f){x=-3.1415926535897932f-x;sign=-1;}
 float z=x*x;
 *sine=x*(1+z*(-1.0f/6+z*(1.0f/120+z*(-1.0f/5040+z*(1.0f/362880+z*(-1.0f/39916800))))));
 *cosine=sign*(1+z*(-0.5f+z*(1.0f/24+z*(-1.0f/720+z*(1.0f/40320+z*(-1.0f/3628800))))));
}
#endif

