#include <android_native_app_glue.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/input.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <chrono>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"Backrooms",__VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,"Backrooms",__VA_ARGS__)

struct V3 { float x,y,z; };
static V3 add(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static V3 sub(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static V3 mul(V3 a,float s){return {a.x*s,a.y*s,a.z*s};}
static float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static V3 cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static V3 norm(V3 a){float l=std::sqrt(std::max(.00001f,dot(a,a)));return mul(a,1.f/l);}
struct M4 { float v[16]{}; };
static M4 identity(){M4 m{};m.v[0]=m.v[5]=m.v[10]=m.v[15]=1;return m;}
static M4 multiply(const M4&a,const M4&b){M4 r{};for(int c=0;c<4;c++)for(int row=0;row<4;row++)for(int k=0;k<4;k++)r.v[c*4+row]+=a.v[k*4+row]*b.v[c*4+k];return r;}
static M4 perspective(float fov,float aspect,float n,float f){M4 m{};float q=1.f/std::tan(fov*.5f);m.v[0]=q/aspect;m.v[5]=q;m.v[10]=(f+n)/(n-f);m.v[11]=-1;m.v[14]=(2*f*n)/(n-f);return m;}
static M4 lookAt(V3 eye,V3 center,V3 up){V3 f=norm(sub(center,eye)),s=norm(cross(f,up)),u=cross(s,f);M4 m=identity();m.v[0]=s.x;m.v[1]=u.x;m.v[2]=-f.x;m.v[4]=s.y;m.v[5]=u.y;m.v[6]=-f.y;m.v[8]=s.z;m.v[9]=u.z;m.v[10]=-f.z;m.v[12]=-dot(s,eye);m.v[13]=-dot(u,eye);m.v[14]=dot(f,eye);return m;}
static M4 model(V3 p,V3 s){M4 m=identity();m.v[0]=s.x;m.v[5]=s.y;m.v[10]=s.z;m.v[12]=p.x;m.v[13]=p.y;m.v[14]=p.z;return m;}

struct Vertex {float x,y,z,r,g,b;};
static const char* VS=R"(#version 300 es
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aColor;
uniform mat4 uMVP; out vec3 vColor; out vec3 vPos;
void main(){vColor=aColor;vPos=aPos;gl_Position=uMVP*vec4(aPos,1.0);})";
static const char* FS=R"(#version 300 es
precision mediump float; in vec3 vColor; in vec3 vPos; uniform float uSnow; uniform float uTime; out vec4 outColor;
float hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
void main(){float grain=(hash(gl_FragCoord.xy+uTime*71.0)-.5)*.075;vec3 c=vColor*(.93+grain);float n=hash(gl_FragCoord.xy*vec2(1.13,1.71)+uTime*97.0);c=mix(c,vec3(n),uSnow*.88);outColor=vec4(c,1.0);})";

struct Portal {int from,to;V3 pos,spawn;float color[3];};
struct Engine {
 android_app* app{}; EGLDisplay display=EGL_NO_DISPLAY;EGLSurface surface=EGL_NO_SURFACE;EGLContext context=EGL_NO_CONTEXT;
 GLuint program=0,vbo=0,vao=0; GLint mvpLoc=-1,snowLoc=-1,timeLoc=-1;
 int width=1,height=1,level=0; V3 player{0,1.65f,0};float yaw=0,pitch=0,elapsed=0,snow=0,snowTimer=0;bool running=true,flashlight=false,sprinting=false;
 bool leftDown=false,rightDown=false;float joyX=0,joyY=0,lastX=0,lastY=0;int leftId=-1,rightId=-1;bool keyW=false,keyA=false,keyS=false,keyD=false;
 std::vector<Portal> portals;
};
static GLuint shader(GLenum type,const char*src){GLuint s=glCreateShader(type);glShaderSource(s,1,&src,nullptr);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[1024];glGetShaderInfoLog(s,sizeof(log),nullptr,log);LOGE("shader: %s",log);}return s;}
static bool initGL(Engine&e){
 const EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_DEPTH_SIZE,16,EGL_NONE};
 e.display=eglGetDisplay(EGL_DEFAULT_DISPLAY);if(e.display==EGL_NO_DISPLAY||!eglInitialize(e.display,nullptr,nullptr))return false;
 EGLConfig cfg;EGLint count;eglChooseConfig(e.display,attrs,&cfg,1,&count);if(!count)return false;
 EGLint fmt;eglGetConfigAttrib(e.display,cfg,EGL_NATIVE_VISUAL_ID,&fmt);ANativeWindow_setBuffersGeometry(e.app->window,0,0,fmt);
 const EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};e.context=eglCreateContext(e.display,cfg,EGL_NO_CONTEXT,ctx);e.surface=eglCreateWindowSurface(e.display,cfg,e.app->window,nullptr);
 if(e.context==EGL_NO_CONTEXT||e.surface==EGL_NO_SURFACE||!eglMakeCurrent(e.display,e.surface,e.surface,e.context))return false;
 eglQuerySurface(e.display,e.surface,EGL_WIDTH,&e.width);eglQuerySurface(e.display,e.surface,EGL_HEIGHT,&e.height);
 GLuint vs=shader(GL_VERTEX_SHADER,VS),fs=shader(GL_FRAGMENT_SHADER,FS);e.program=glCreateProgram();glAttachShader(e.program,vs);glAttachShader(e.program,fs);glLinkProgram(e.program);glDeleteShader(vs);glDeleteShader(fs);
 GLint ok=0;glGetProgramiv(e.program,GL_LINK_STATUS,&ok);if(!ok)return false;
 e.mvpLoc=glGetUniformLocation(e.program,"uMVP");e.snowLoc=glGetUniformLocation(e.program,"uSnow");e.timeLoc=glGetUniformLocation(e.program,"uTime");
 const float cube[]={ // 36 vertices, position only; color is supplied by vertex attribute constant below
 -0.5f,-0.5f,-0.5f, 0,0,0,  .5f,-.5f,-.5f,0,0,0, .5f,.5f,-.5f,0,0,0, .5f,.5f,-.5f,0,0,0, -.5f,.5f,-.5f,0,0,0, -.5f,-.5f,-.5f,0,0,0,
 -.5f,-.5f,.5f,0,0,0, .5f,-.5f,.5f,0,0,0, .5f,.5f,.5f,0,0,0, .5f,.5f,.5f,0,0,0, -.5f,.5f,.5f,0,0,0, -.5f,-.5f,.5f,0,0,0,
 -.5f,.5f,.5f,0,0,0, -.5f,.5f,-.5f,0,0,0, -.5f,-.5f,-.5f,0,0,0, -.5f,-.5f,-.5f,0,0,0, -.5f,-.5f,.5f,0,0,0, -.5f,.5f,.5f,0,0,0,
 .5f,.5f,.5f,0,0,0, .5f,.5f,-.5f,0,0,0, .5f,-.5f,-.5f,0,0,0, .5f,-.5f,-.5f,0,0,0, .5f,-.5f,.5f,0,0,0, .5f,.5f,.5f,0,0,0,
 -.5f,-.5f,-.5f,0,0,0, .5f,-.5f,-.5f,0,0,0, .5f,-.5f,.5f,0,0,0, .5f,-.5f,.5f,0,0,0, -.5f,-.5f,.5f,0,0,0, -.5f,-.5f,-.5f,0,0,0,
 -.5f,.5f,-.5f,0,0,0, .5f,.5f,-.5f,0,0,0, .5f,.5f,.5f,0,0,0, .5f,.5f,.5f,0,0,0, -.5f,.5f,.5f,0,0,0, -.5f,.5f,-.5f,0,0,0};
 glGenVertexArrays(1,&e.vao);glBindVertexArray(e.vao);glGenBuffers(1,&e.vbo);glBindBuffer(GL_ARRAY_BUFFER,e.vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(cube),cube,GL_STATIC_DRAW);
 glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
 glDisableVertexAttribArray(1);glVertexAttrib3f(1,1,1,1);
 glEnable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glClearColor(.45f,.43f,.31f,1);
 e.portals={{0,1,{9,0,26},{-22,1.65f,28},{.45f,.9f,.5f}},{1,2,{-22,0,25},{23,1.65f,-8},{.35f,.75f,1.f}},{2,0,{23,0,-8},{0,1.65f,0},{1.f,.8f,.35f}}};
 return true;
}
static void drawBox(Engine&e,M4 vp,V3 pos,V3 size,float r,float g,float b){
 M4 mvp=multiply(vp,model(pos,size));glUniformMatrix4fv(e.mvpLoc,1,GL_FALSE,mvp.v);glVertexAttrib3f(1,r,g,b);glDrawArrays(GL_TRIANGLES,0,36);
}
static bool blocked(float x,float z){
 if(x<-35||x>35||z<-35||z>35)return true;
 const float walls[][4]={{-22,-22,18,.45f},{-10,-22,.45f,12},{-16,-12,12,.45f},{-28,-5,.45f,20},{-18,1,12,.45f},{-7,7,.45f,22},{4,-23,.45f,18},{12,-14,18,.45f},{23,-5,.45f,22},{14,7,20,.45f},{4,17,.45f,18}};
 for(auto&w:walls)if(std::abs(x-w[0])<w[2]*.5f+.35f&&std::abs(z-w[1])<w[3]*.5f+.35f)return true;
 return false;
}
static void changeLevel(Engine&e,int to){if(to==e.level)return;e.level=to;for(auto&p:e.portals)if(p.to==to){e.player=p.spawn;break;}e.yaw=0;e.pitch=0;e.snow=1;e.snowTimer=1.45f;}
static void render(Engine&e,float dt){
 e.elapsed+=dt;if(e.snowTimer>0){e.snowTimer-=dt;e.snow=std::max(0.f,e.snowTimer/.28f);}else e.snow=0;
 const float forward=(e.keyW?1.f:0.f)-(e.keyS?1.f:0.f)-e.joyY;
 const float side=(e.keyD?1.f:0.f)-(e.keyA?1.f:0.f)+e.joyX;
 const float mag=std::sqrt(forward*forward+side*side);if(mag>.08f){float sp=e.sprinting?6.1f:3.25f;float fx=std::sin(e.yaw),fz=-std::cos(e.yaw),rx=std::cos(e.yaw),rz=std::sin(e.yaw);float dx=(fx*forward+rx*side)/std::max(1.f,mag)*sp*dt,dz=(fz*forward+rz*side)/std::max(1.f,mag)*sp*dt;if(!blocked(e.player.x+dx,e.player.z))e.player.x+=dx;if(!blocked(e.player.x,e.player.z+dz))e.player.z+=dz;}
 for(auto&p:e.portals)if(p.from==e.level&&std::hypot(e.player.x-p.pos.x,e.player.z-p.pos.z)<1.35f)changeLevel(e,p.to);
 glViewport(0,0,e.width,e.height);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glUseProgram(e.program);glBindVertexArray(e.vao);
 V3 dir{std::sin(e.yaw)*std::cos(e.pitch),std::sin(e.pitch),-std::cos(e.yaw)*std::cos(e.pitch)};
 M4 proj=perspective(1.32f,(float)e.width/(float)e.height,.08f,100.f);M4 view=lookAt(e.player,add(e.player,dir),{0,1,0});M4 vp=multiply(proj,view);
 glUniform1f(e.snowLoc,e.snow);glUniform1f(e.timeLoc,e.elapsed);
 float wall[3],floor[3],lamp[3];if(e.level==0){wall[0]=.76f;wall[1]=.72f;wall[2]=.43f;floor[0]=.35f;floor[1]=.29f;floor[2]=.16f;}else if(e.level==1){wall[0]=.49f;wall[1]=.52f;wall[2]=.34f;floor[0]=.25f;floor[1]=.23f;floor[2]=.15f;}else{wall[0]=.27f;wall[1]=.39f;wall[2]=.39f;floor[0]=.16f;floor[1]=.23f;floor[2]=.23f;}
 drawBox(e,vp,{0,-.14f,0},{72,.2f,72},floor[0],floor[1],floor[2]);
 drawBox(e,vp,{0,3.7f,0},{72,.2f,72},.35f,.34f,.25f);
 auto wallBox=[&](float x,float z,float w,float d){drawBox(e,vp,{x,1.8f,z},{w,3.6f,d},wall[0],wall[1],wall[2]);};
 wallBox(0,-36,72,.4f);wallBox(0,36,72,.4f);wallBox(-36,0,.4f,72);wallBox(36,0,.4f,72);
 const float segments[][4]={{-22,-22,18,.35f},{-10,-22,.35f,12},{-16,-12,12,.35f},{-28,-5,.35f,20},{-18,1,12,.35f},{-7,7,.35f,22},{4,-23,.35f,18},{12,-14,18,.35f},{23,-5,.35f,22},{14,7,20,.35f},{4,17,.35f,18}};
 for(auto&w:segments)wallBox(w[0],w[1],w[2],w[3]);
 for(int z=-26;z<=26;z+=10)for(int x=-26;x<=26;x+=12){drawBox(e,vp,{(float)x,3.53f,(float)z},{3.2f,.045f,1.4f},.8f,.78f,.62f);drawBox(e,vp,{(float)x,3.48f,(float)z},{2.2f,.025f,.75f},1.f,.96f,.82f);}
 for(auto&p:e.portals)if(p.from==e.level){drawBox(e,vp,{p.pos.x-.8f,1.5f,p.pos.z},{.14f,3.f,.2f},.25f,.24f,.18f);drawBox(e,vp,{p.pos.x+.8f,1.5f,p.pos.z},{.14f,3.f,.2f},.25f,.24f,.18f);drawBox(e,vp,{p.pos.x,3.f,p.pos.z},{1.7f,.16f,.2f},.25f,.24f,.18f);drawBox(e,vp,{p.pos.x,1.5f,p.pos.z-.03f},{1.35f,2.75f,.035f},p.color[0],p.color[1],p.color[2]);}
 for(int i=-20;i<=20;i+=10){drawBox(e,vp,{(float)i,3.38f,(float)-20},{2.8f,.035f,1.1f},1.f,.96f,.78f);}
 eglSwapBuffers(e.display,e.surface);
}
static int32_t input(android_app*app,AInputEvent*ev){
 Engine&e=*static_cast<Engine*>(app->userData);
 if(AInputEvent_getType(ev)==AINPUT_EVENT_TYPE_KEY){int k=AKeyEvent_getKeyCode(ev);bool d=AKeyEvent_getAction(ev)==AKEY_EVENT_ACTION_DOWN;if(k==AKEYCODE_W)e.keyW=d;if(k==AKEYCODE_S)e.keyS=d;if(k==AKEYCODE_A)e.keyA=d;if(k==AKEYCODE_D)e.keyD=d;if(k==AKEYCODE_SHIFT_LEFT||k==AKEYCODE_SHIFT_RIGHT)e.sprinting=d;if(d&&k==AKEYCODE_F)e.flashlight=!e.flashlight;return 1;}
 if(AInputEvent_getType(ev)!=AINPUT_EVENT_TYPE_MOTION)return 0;
 int action=AMotionEvent_getAction(ev)&AMOTION_EVENT_ACTION_MASK;int idx=(AMotionEvent_getAction(ev)&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;int id=AMotionEvent_getPointerId(ev,idx);float x=AMotionEvent_getX(ev,idx),y=AMotionEvent_getY(ev,idx);
 if(action==AMOTION_EVENT_ACTION_DOWN||action==AMOTION_EVENT_ACTION_POINTER_DOWN){if(x<e.width*.48f){e.leftId=id;e.leftDown=true;}else{e.rightId=id;e.rightDown=true;e.lastX=x;e.lastY=y;}return 1;}
 if(action==AMOTION_EVENT_ACTION_MOVE){for(size_t i=0;i<AMotionEvent_getPointerCount(ev);++i){int pid=AMotionEvent_getPointerId(ev,i);float px=AMotionEvent_getX(ev,i),py=AMotionEvent_getY(ev,i);if(pid==e.leftId){e.joyX=std::clamp((px-e.width*.22f)/(e.width*.18f),-1.f,1.f);e.joyY=std::clamp((py-e.height*.72f)/(e.height*.18f),-1.f,1.f);}if(pid==e.rightId){e.yaw-=(px-e.lastX)*.006f;e.pitch=std::clamp(e.pitch-(py-e.lastY)*.004f,-1.25f,1.25f);e.lastX=px;e.lastY=py;}}return 1;}
 if(action==AMOTION_EVENT_ACTION_UP||action==AMOTION_EVENT_ACTION_POINTER_UP||action==AMOTION_EVENT_ACTION_CANCEL){if(id==e.leftId){e.leftId=-1;e.leftDown=false;e.joyX=e.joyY=0;}if(id==e.rightId){e.rightId=-1;e.rightDown=false;}if(y<e.height*.22f&&x>e.width*.78f)e.flashlight=!e.flashlight;if(y>e.height*.78f&&x>e.width*.78f)e.sprinting=!e.sprinting;return 1;}
 return 1;
}
static void cmd(android_app*app,int32_t c){Engine&e=*static_cast<Engine*>(app->userData);if(c==APP_CMD_INIT_WINDOW&&app->window){if(initGL(e))LOGI("OpenGL ES renderer ready");else LOGE("OpenGL initialization failed");}else if(c==APP_CMD_TERM_WINDOW){if(e.display!=EGL_NO_DISPLAY){eglMakeCurrent(e.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(e.context!=EGL_NO_CONTEXT)eglDestroyContext(e.display,e.context);if(e.surface!=EGL_NO_SURFACE)eglDestroySurface(e.display,e.surface);eglTerminate(e.display);}e.display=EGL_NO_DISPLAY;e.surface=EGL_NO_SURFACE;e.context=EGL_NO_CONTEXT;}else if(c==APP_CMD_WINDOW_RESIZED&&e.display!=EGL_NO_DISPLAY){eglQuerySurface(e.display,e.surface,EGL_WIDTH,&e.width);eglQuerySurface(e.display,e.surface,EGL_HEIGHT,&e.height);}}
void android_main(android_app*app){app_dummy();Engine e;e.app=app;app->userData=&e;app->onAppCmd=cmd;app->onInputEvent=input;int events;android_poll_source*source;while(e.running){while(ALooper_pollAll(e.display==EGL_NO_DISPLAY?-1:0,nullptr,&events,(void**)&source)>=0){if(source)source->process(app,source);if(app->destroyRequested){e.running=false;break;}}if(e.display!=EGL_NO_DISPLAY&&e.surface!=EGL_NO_SURFACE){static auto last=std::chrono::steady_clock::now();auto now=std::chrono::steady_clock::now();float dt=std::chrono::duration<float>(now-last).count();last=now;render(e,std::min(dt,.05f));}}}
