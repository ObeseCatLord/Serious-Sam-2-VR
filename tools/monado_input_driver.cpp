// Interactive companion to monado_pose_driver for private simulated-headset
// testing: one packet sets the remote Index-controller state, including the
// buttons and grips the pose driver leaves neutral. Arguments are key=value:
//   head=x,y,z,yaw,pitch,roll  lpos=x,y,z  rpos=x,y,z  lrot=yaw,pitch,roll  rrot=...
//   ltrig= rtrig= lgrip= rgrip= (0..1)  lstick=x,y rstick=x,y lpad=x,y (-1..1)
//   la= lb= ra= rb= lsys= rsys= lclick= rclick= (0/1)  lforce= rforce= (trackpad force 0..1)
//   lactive= ractive= (0/1, default 1: 0 drops that controller's tracking)
// Compile against the exact private Monado headers, like monado_pose_driver.
#include "remote/r_interface.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
static_assert(sizeof(r_remote_data)==376);
using Clock=std::chrono::steady_clock;
static bool transfer(int socket,void *buffer,size_t size,bool write,Clock::time_point deadline) {
    auto *p=static_cast<unsigned char *>(buffer);
    while(size) {
        auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0)return false;
        pollfd fd{socket,static_cast<short>(write?POLLOUT:POLLIN),0};
        if(poll(&fd,1,static_cast<int>(left))<=0) { if(errno==EINTR)continue;return false; }
        auto n=write?send(socket,p,size,MSG_NOSIGNAL):recv(socket,p,size,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);
    }
    return true;
}
static xrt_quat multiply(xrt_quat a,xrt_quat b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
            a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
static xrt_quat euler(const std::vector<double> &v) {
    const xrt_quat yaw{0,float(std::sin(v[0]/2)),0,float(std::cos(v[0]/2))};
    const xrt_quat pitch{float(std::sin(v[1]/2)),0,0,float(std::cos(v[1]/2))};
    const xrt_quat roll{0,0,float(std::sin(v[2]/2)),float(std::cos(v[2]/2))};
    return multiply(multiply(yaw,pitch),roll);
}
static bool numbers(const char *text,std::vector<double> &out,size_t count) {
    out.clear();
    for(const char *p=text;;) {
        char *end=nullptr;const double v=std::strtod(p,&end);
        if(end==p||!std::isfinite(v)||std::abs(v)>10)return false;
        out.push_back(v);
        if(*end==',')p=end+1; else if(!*end)break; else return false;
    }
    return out.size()==count;
}
int main(int n,char **a) {
    long port=42431;
    std::vector<double> head{0,1.6,0,0,0,0},pos[2]{{-.25,1.3,-.5},{.25,1.3,-.5}},rot[2]{{0,0,0},{0,0,0}};
    double trig[2]{},grip[2]{},force[2]{};std::vector<double> stick[2]{{0,0},{0,0}},pad{0,0};
    bool ab[2][2]{},sys[2]{},click[2]{},active[2]{true,true};
    for(int i=1;i<n;++i) {
        const char *eq=std::strchr(a[i],'=');
        if(!eq){std::fprintf(stderr,"bad argument %s\n",a[i]);return 2;}
        const std::string key(a[i],size_t(eq-a[i]));const char *value=eq+1;std::vector<double> v;
        auto scalar=[&](double &out,double lo,double hi){if(!numbers(value,v,1)||v[0]<lo||v[0]>hi)return false;out=v[0];return true;};
        bool ok=true;double flag=0;
        if(key=="port"){port=std::strtol(value,nullptr,10);ok=port>0&&port<65536;}
        else if(key=="head")ok=numbers(value,head,6);
        else if(key=="lpos"||key=="rpos")ok=numbers(value,pos[key[0]=='r'],3);
        else if(key=="lrot"||key=="rrot")ok=numbers(value,rot[key[0]=='r'],3);
        else if(key=="ltrig"||key=="rtrig")ok=scalar(trig[key[0]=='r'],0,1);
        else if(key=="lgrip"||key=="rgrip")ok=scalar(grip[key[0]=='r'],0,1);
        else if(key=="lforce"||key=="rforce")ok=scalar(force[key[0]=='r'],0,1);
        else if(key=="lstick"||key=="rstick")ok=numbers(value,stick[key[0]=='r'],2);
        else if(key=="lpad")ok=numbers(value,pad,2);
        else if(key=="la"||key=="ra"||key=="lb"||key=="rb"){ok=scalar(flag,0,1);ab[key[0]=='r'][key[1]=='b']=flag>0;}
        else if(key=="lsys"||key=="rsys"){ok=scalar(flag,0,1);sys[key[0]=='r']=flag>0;}
        else if(key=="lclick"||key=="rclick"){ok=scalar(flag,0,1);click[key[0]=='r']=flag>0;}
        else if(key=="lactive"||key=="ractive"){ok=scalar(flag,0,1);active[key[0]=='r']=flag>0;}
        else ok=false;
        if(!ok){std::fprintf(stderr,"bad argument %s\n",a[i]);return 2;}
    }
    auto deadline=Clock::now()+std::chrono::seconds(2);
    int sock=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(sock<0)return 3;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(uint16_t(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(sock,reinterpret_cast<sockaddr *>(&address),sizeof(address))!=0) {
        if(errno!=EINPROGRESS){close(sock);return 3;}
        pollfd ready{sock,POLLOUT,0};int error=0;socklen_t size=sizeof(error);
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(left<=0||poll(&ready,1,int(left))<=0||getsockopt(sock,SOL_SOCKET,SO_ERROR,&error,&size)!=0||error){close(sock);return 3;}
    }
    r_remote_data handshake[2]{};
    for(auto &packet:handshake)
        if(!transfer(sock,&packet,sizeof(packet),false,deadline)||std::memcmp(&packet.header,"mndrmt3\0",8)!=0){close(sock);return 4;}
    r_remote_data packet{};std::memset(&packet,0,sizeof(packet));std::memcpy(&packet.header,"mndrmt3\0",8);
    packet.head.center.orientation=euler({head[3],head[4],head[5]});
    packet.head.center.position={float(head[0]),float(head[1]),float(head[2])};
    packet.head.per_view_data_valid=true;
    for(unsigned h=0;h<2;++h) {
        auto &view=packet.head.views[h];view.pose.orientation.w=1;
        view.pose.position.x=h?.032f:-.032f;
        view.fov={-.85f,.85f,.85f,-.85f};
        auto &c=h?packet.right:packet.left;
        c.active=active[h];c.hand_tracking_active=false;
        c.pose.orientation=euler(rot[h]);
        c.pose.position={float(pos[h][0]),float(pos[h][1]),float(pos[h][2])};
        c.trigger_value.x=float(trig[h]);c.trigger_click=trig[h]>.65;c.trigger_touch=trig[h]>0;
        c.squeeze_value.x=float(grip[h]);c.squeeze_force.x=float(grip[h]);
        c.thumbstick={float(stick[h][0]),float(stick[h][1])};
        c.thumbstick_touch=stick[h][0]!=0||stick[h][1]!=0||click[h];c.thumbstick_click=click[h];
        c.a_click=c.a_touch=ab[h][0];c.b_click=c.b_touch=ab[h][1];
        c.system_click=c.system_touch=sys[h];
        c.trackpad_force.x=float(force[h]);
        if(!h){c.trackpad={float(pad[0]),float(pad[1])};c.trackpad_touch=pad[0]!=0||pad[1]!=0||force[h]>0;}
    }
    bool ok=transfer(sock,&packet,sizeof(packet),true,deadline);close(sock);
    return ok?0:5;
}
