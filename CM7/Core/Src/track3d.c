/* TRACK3D-R2: experimental, causal 3-D angular velocity prediction.
 * Replaces track3d.c from R1. All public interfaces remain unchanged.
 * Tested offline on the 68-window Oct 28 capture. Independent validation pending.
 */
#include "track3d.h"
#include <math.h>
#include <string.h>
#include <stdint.h>
#define T3_PI 3.14159265358979323846f
#define T3_D2R (T3_PI / 180.0f)
#define T3_R2D (180.0f / T3_PI)
#define T3_GATE 28.0f
#define T3_ALPHA 0.50f
#define T3_OMEGA_ALPHA 0.25f
#define T3_OMEGA_MAX (20.0f*T3_D2R)
#define T3_COAST_DECAY 0.80f
#define T3_MAX_MISSES 5U
#define T3_CONFIRM_SEQ 3U

typedef struct { float x,y,z; } V3;
static V3 s_state,s_omega,s_pending;
static uint32_t s_last_seq,s_pending_seq,s_misses;
static uint8_t s_has_state,s_has_pending,s_have_seq;
static float clampf3(float x,float lo,float hi){return x<lo?lo:(x>hi?hi:x);}
static V3 add3(V3 a,V3 b){V3 v={a.x+b.x,a.y+b.y,a.z+b.z};return v;}
static V3 scale3(V3 a,float f){V3 v={a.x*f,a.y*f,a.z*f};return v;}
static float dot3(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static V3 cross3(V3 a,V3 b){V3 v={a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};return v;}
static float mag3(V3 a){return sqrtf(dot3(a,a));}
static V3 unit3(V3 a){float n=mag3(a);return n>1e-8f?scale3(a,1.0f/n):a;}
static V3 zero3(void){V3 z={0.0f,0.0f,0.0f};return z;}
static float wrap360(float a){a=fmodf(a,360.0f);return a<0.0f?a+360.0f:a;}
static V3 direction(float native,float elevation){
 float a=wrap360(360.0f-native)*T3_D2R,e=elevation*T3_D2R;
 V3 v={cosf(e)*sinf(a),cosf(e)*cosf(a),sinf(e)};return v;
}
static float angle3(V3 a,V3 b){return acosf(clampf3(dot3(a,b),-1.0f,1.0f))*T3_R2D;}
/* Rodrigues' formula, omega is a rotation vector in radians per window. */
static V3 predict3(V3 v,V3 omega,float dt){
 float speed=mag3(omega),theta=speed*dt;
 if(speed<1e-8f||theta<1e-8f)return v;
 V3 k=scale3(omega,1.0f/speed);
 float c=cosf(theta),s=sinf(theta);
 return unit3(add3(add3(scale3(v,c),scale3(cross3(k,v),s)),scale3(k,dot3(k,v)*(1.0f-c))));
}
static V3 angular_delta3(V3 old,V3 now){
 float theta=acosf(clampf3(dot3(old,now),-1.0f,1.0f));
 V3 axis=cross3(old,now);float n=mag3(axis);
 return n>1e-8f?scale3(axis,theta/n):zero3();
}
static void lose3(void){s_has_state=s_has_pending=0U;s_omega=zero3();}
void track3d_reset(void){
 s_state=s_pending=s_omega=zero3();s_last_seq=s_pending_seq=s_misses=0U;
 s_has_state=s_has_pending=s_have_seq=0U;
}
const char *track3d_state_text(Track3dState s){
 switch(s){
 case TRACK3D_ACQUIRED:return "ACQUIRED";
 case TRACK3D_TRACKING:return "TRACKING";
 case TRACK3D_COASTING:return "COASTING";
 case TRACK3D_REACQUIRED:return "REACQUIRED";
 case TRACK3D_LOST:return "LOST";
 default:return "NO_MEASUREMENT";
 }
}
Track3dOutput track3d_step(uint32_t seq,uint8_t toa_valid,uint8_t eligible,
                            uint8_t tetra_fit_ok,float azimuth_native_deg,float elevation_deg){
 Track3dOutput o;memset(&o,0,sizeof(o));o.seq=seq;o.state=TRACK3D_NO_MEASUREMENT;
 if(s_have_seq&&seq<=s_last_seq){
  if(s_has_state){o.valid=1U;o.x=s_state.x;o.y=s_state.y;o.z=s_state.z;
   o.elevation_deg=asinf(clampf3(s_state.z,-1,1))*T3_R2D;
   o.azimuth_physical_deg=wrap360(atan2f(s_state.x,s_state.y)*T3_R2D);
   o.azimuth_native_deg=wrap360(360.0f-o.azimuth_physical_deg);}
  return o;
 }
 uint32_t dt=s_have_seq?(seq-s_last_seq):1U;
 if(dt>1U&&s_has_state){
  uint32_t skipped=dt-1U;
  s_misses=(skipped>UINT32_MAX-s_misses)?UINT32_MAX:s_misses+skipped;
  if(s_misses>=T3_MAX_MISSES)lose3();
 }
 s_have_seq=1U;s_last_seq=seq;
 V3 predicted=s_has_state?predict3(s_state,s_omega,(float)dt):zero3();
 uint8_t has=(toa_valid&&eligible&&tetra_fit_ok&&isfinite(azimuth_native_deg)&&
              isfinite(elevation_deg)&&elevation_deg>=-90.0f&&elevation_deg<=90.0f);
 if(has){
  V3 u=direction(azimuth_native_deg,elevation_deg);
  if(!s_has_state){s_state=u;s_has_state=1U;s_omega=zero3();s_misses=0U;
   o.used=1U;o.state=TRACK3D_ACQUIRED;
  }else{
   o.innovation_deg=angle3(predicted,u);o.innovation_valid=1U;
   if(o.innovation_deg<=T3_GATE){
    V3 filtered=unit3(add3(scale3(predicted,1.0f-T3_ALPHA),scale3(u,T3_ALPHA)));
    V3 measured=angular_delta3(s_state,filtered);
    s_omega=add3(scale3(s_omega,1.0f-T3_OMEGA_ALPHA),scale3(measured,T3_OMEGA_ALPHA));
    float speed=mag3(s_omega);
    if(speed>T3_OMEGA_MAX)s_omega=scale3(s_omega,T3_OMEGA_MAX/speed);
    s_state=filtered;s_misses=0U;s_has_pending=0U;
    o.used=1U;o.state=TRACK3D_TRACKING;
   }else{
    s_misses++;o.rejected=1U;o.state=TRACK3D_COASTING;
    if(s_has_pending&&seq-s_pending_seq<=T3_CONFIRM_SEQ&&s_misses>=2U&&
       angle3(s_pending,u)<=T3_GATE){
     s_state=unit3(add3(s_pending,u));s_omega=zero3();s_misses=0U;s_has_pending=0U;
     o.used=1U;o.rejected=0U;o.state=TRACK3D_REACQUIRED;
    }else{s_pending=u;s_pending_seq=seq;s_has_pending=1U;}
   }
  }
 }else{
  s_misses++;
  if(s_has_state)o.state=TRACK3D_COASTING;
 }
 if(s_has_state&&o.state==TRACK3D_COASTING){
  s_state=predicted;s_omega=scale3(s_omega,T3_COAST_DECAY);
 }
 if(s_misses>=T3_MAX_MISSES){lose3();o.state=TRACK3D_LOST;}
 if(s_has_state){
  o.valid=1U;o.x=s_state.x;o.y=s_state.y;o.z=s_state.z;
  o.elevation_deg=asinf(clampf3(s_state.z,-1,1))*T3_R2D;
  o.azimuth_physical_deg=wrap360(atan2f(s_state.x,s_state.y)*T3_R2D);
  o.azimuth_native_deg=wrap360(360.0f-o.azimuth_physical_deg);
 }
 return o;
}
