#ifdef REAL_CONTROLLER
#include "Vpocket_video_test.h"
using Vpocket_video = Vpocket_video_test;
#else
#include "Vpocket_video.h"
#endif
#include "verilated.h"
#include <algorithm>
#include <vector>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <cstdint>
struct Bounds { int x,y,w,h; };
static uint32_t colour(int f,int x,int y) { return ((((x*83)^(y*37))&255)<<16) | (((x*13+y*71+f*3)&255)<<8) | ((f*17+x+y*29)&255); }
static void check(bool ok,const std::string& msg) { if (!ok) throw std::runtime_error(msg); }
// Transaction-level CellularRAM model: initialisation, burst startup, row
// boundaries, idle gaps, delayed responses and occasional command stalls.
struct Memory {
    std::vector<uint32_t> ram = std::vector<uint32_t>(524288,0xdeadbe);
    struct Response { uint64_t due; uint32_t value; };
    std::deque<Response> responses;
    uint64_t cycles=0, pause_until=0;
    int wait=0, tail=0;
    bool burst=false, write=false;
    unsigned address=0;
    void before(Vpocket_video& d) {
#ifndef REAL_CONTROLLER
        d.memory_ready=cycles>=12000 && cycles>=pause_until;
        d.m_ready=burst && !wait && !tail && (cycles%37!=0);
        d.m_idle=!burst && !tail && responses.empty();
        d.m_rvalid=!responses.empty() && responses.front().due==cycles;
#endif
        d.m_rdata=(!responses.empty() && responses.front().due==cycles)?responses.front().value:0;
    }
    void after(Vpocket_video& d,bool valid,bool ready,bool we,bool last,unsigned addr,uint32_t data) {
        if(!responses.empty() && responses.front().due==cycles) responses.pop_front();
#ifdef REAL_CONTROLLER
        if(valid && ready) {
            check(addr<ram.size(),"PSRAM address out of bounds");
            if(we) ram[addr]=data; else responses.push_back({cycles+3,ram[addr]});
        }
#else
        if (tail) { if (!--tail) burst=false; }
        else if (burst) {
            if(wait) --wait;
            else if(valid && ready) {
                check(we==write && addr==address,"broken PSRAM burst/address");
                check(addr<ram.size(),"PSRAM address out of bounds");
                if(we) ram[addr]=data;
                else responses.push_back({cycles+3,ram[addr]});
                ++address;
                if(last || (addr&127)==127) tail=5;
            }
        } else if(valid && d.memory_ready) { burst=true; write=we; address=addr; wait=5; }
#endif
        ++cycles;
    }
};
static int preset(int mode,int dock,Bounds b) {
    if(mode==0) return 0;
    if(mode==2 && b.w>=638 && b.w<=640 && b.h>0 && b.h<=240) return 0;
    if(mode==1 && b.w>=638 && b.w<=640) {
        if(b.h==240)return 1; if(b.h==224)return 2; if(b.h==216)return 3;
    }
    if(mode==3) {
        if(dock) { if(b.w==640 && b.h==240)return 0; }
        else {
            if(b.h==240 && b.w>=520)return 4;
            if((b.h==224 || b.h==216) && b.w>=480)return 5;
            if(b.h==213 && b.w>=460)return 6;
        }
    }
    return 7;
}
static const int widths[]={640,640,640,640,520,480,460,400};
static const int heights[]={240,240,224,216,234,216,207,360};
static unsigned mix(unsigned a,unsigned b,int f) { return (a*(256-f)+b*f+128)/256; }
struct Test {
    Vpocket_video d;
    Memory mem;
    unsigned pixels=0, rows=0;
    bool prev_de=false,prev_vs=false;
    int frame=-1, mode=0, dock=0, input_frame=0;
    Bounds bounds{0,0,640,240}, previous{0,0,640,240};
    uint64_t tested=0;
    bool source_valid=true, previous_valid=true, force_black=false;
    bool transitions[5][5]{};
    int expected_slot=0, active_slot=0, announced_slot=0;
    uint64_t native_pixels=0, filtered_pixels=0, hybrid_pixels=0;
    int announced_mode=0, announced_dock=0, prior_mode=0, prior_dock=0;
    void sys() {
        d.clk_sys=0; mem.before(d); d.eval();
        bool v=d.m_valid,r=d.m_ready,w=d.m_we,l=d.m_last;
        unsigned a=d.m_addr; uint32_t data=d.m_wdata;
        d.clk_sys=1; d.eval(); mem.after(d,v,r,w,l,a,data);
    }
    void tick(bool inspect=true) {
        d.clk_vid=0; d.eval();
        for(int i=0;i<2;++i) sys();
        d.clk_vid=1; d.eval();
        if(inspect) observe();
        for(int i=0;i<3;++i) sys();
    }
    uint32_t expected_pixel(int x,int y) {
        if(!mode) return colour(input_frame,x,y);
        if (!previous_valid || force_black || (active_slot!=7 && active_slot!=preset(mode,dock,previous))) return 0;
        int cw=widths[active_slot], ch=heights[active_slot];
        int64_t px,py;
        bool filter=active_slot==7;
        if(!filter) {
            // Floor division by two centres odd padding/crop differences.
            auto half=[](int v){return v>=0?v/2:-((-v+1)/2);};
            // Padding can produce negative coordinates; multiplication keeps
            // the reference defined on C++ versions where signed shifts do not.
            px=(int64_t(previous.x+(mode==1?0:half(previous.w-cw))+x)*4096)+2048;
            py=(int64_t(previous.y+half(previous.h-ch)+y)*4096)+2048;
        } else {
            int64_t stretch_x=(int64_t(previous.w)<<12)/400;
            int64_t stretch_y=(int64_t(previous.h)<<12)/360;
            // VI-X samples have half the physical width of a line's height.
            int64_t scale_y=((int64_t(previous.h)<<12)/(dock?300:360))*2;
            bool from_x=mode==2 ? stretch_x>scale_y : stretch_x<scale_y;
            int64_t ux=from_x?stretch_x:scale_y;
            int64_t uy=from_x ? (int64_t(previous.w)<<12)/(dock?960:800) : stretch_y;
            int64_t sx=mode==1?stretch_x:ux, sy=mode==1?stretch_y:uy;
            if(mode==4) {
                sx=(stretch_x+ux)/2;
                sy=(stretch_y+uy)/2;
                check(sx<=stretch_x && sy<=stretch_y,"Hybrid must not add borders");
                check(sx>=ux && sy>=uy,"Hybrid crops beyond cover");
            }
            px=(int64_t(previous.x)<<12)+(int64_t(previous.w)<<11)-sx*399/2+sx*x;
            py=(int64_t(previous.y)<<12)+(int64_t(previous.h)<<11)-sy*359/2+sy*y;
        }
        bool edge=!filter && mode==1 && px>=(int64_t(previous.x+previous.w)<<12);
        if((!edge && (px<(int64_t(previous.x)<<12)||px>=(int64_t(previous.x+previous.w)<<12)))||
           py<(int64_t(previous.y)<<12)||py>=(int64_t(previous.y+previous.h)<<12)) return 0;
        auto clamp=[](int64_t v,int a,int b){return std::max(int64_t(a)<<12,std::min(int64_t(b)<<12,v-2048));};
        px=clamp(px,previous.x,previous.x+previous.w-1);
        py=clamp(py,previous.y,previous.y+previous.h-1);
        int ix=px>>12,iy=py>>12,fx=filter?(px>>4)&255:0,fy=filter?(py>>4)&255:0;
        unsigned c00=colour(input_frame-1,ix,iy),c10=colour(input_frame-1,std::min(ix+1,639),iy);
        unsigned c01=colour(input_frame-1,ix,std::min(iy+1,239)),c11=colour(input_frame-1,std::min(ix+1,639),std::min(iy+1,239));
        unsigned c=0;
        for(int k=0;k<24;k+=8) c|=mix(mix((c00>>k)&255,(c10>>k)&255,fx),mix((c01>>k)&255,(c11>>k)&255,fx),fy)<<k;
        if(filter)++filtered_pixels; else ++native_pixels;
        if(mode==4)++hybrid_pixels;
        return c;
    }

    void observe() {
        if(d.video_vs && !prev_vs) {
            ++frame; rows=0; pixels=0;
        }
        if(d.video_de && !d.video_skip) {
            if(frame>=2) {
                uint32_t want=expected_pixel(pixels,rows);
                if(d.video_rgb!=want) {

                    throw std::runtime_error("pixel mismatch frame="+std::to_string(frame)+" mode="+std::to_string(mode)+
                        " input_frame="+std::to_string(input_frame)+" slot="+std::to_string(active_slot)+" dock="+std::to_string(dock)+" x="+std::to_string(pixels)+" y="+std::to_string(rows)+" got="+std::to_string(d.video_rgb)+" want="+std::to_string(want));
                }
                ++tested;
            }
            ++pixels;
        }
        if(prev_de && !d.video_de) {
            check(pixels==(mode?unsigned(widths[active_slot]):640u),"wrong output width"); ++rows; pixels=0;
            check(d.video_rgb==unsigned(expected_slot<<13),"incorrect EOL slot");
        } else if (!d.video_de) check(d.video_rgb==0,"nonzero blank/frame feature bits");
        check(!d.video_skip || d.video_de,"SKIP outside DE");
        prev_de=d.video_de; prev_vs=d.video_vs;
    }
    void run_frame(int f,int new_mode,int new_dock,Bounds b,bool pal,bool change_window=false,bool blank_capture=false) {
        // Announced mode becomes active after this frame. Test changes in
        // the middle of the preceding blanking interval, not at VS itself.
        previous=bounds; bounds=b; input_frame=f;
        previous_valid=source_valid; source_valid=!change_window && !blank_capture;
        int frame_cycles=pal?250000:208542;
        // Reference bookkeeping for the adapter's frame-ahead slot request.
        int next_mode=announced_mode,next_dock=announced_dock;
        // Observe the previous frame's completion before changing expected geometry.
        if(frame>=2) check(rows==(mode?unsigned(heights[active_slot]):240u),"incomplete frame");
        transitions[mode][next_mode]=true;
        mode=next_mode; dock=next_dock; active_slot=announced_slot;
        announced_mode=prior_mode; announced_dock=prior_dock;
        prior_mode=new_mode; prior_dock=new_dock;
        announced_slot=preset(announced_mode,announced_dock,previous);
        expected_slot=announced_slot;
        d.screen_mode=new_mode; d.docked=new_dock;
        d.source_x=b.x; d.source_y=b.y; d.source_w=b.w; d.source_h=b.h;
        // Settings were allowed to cross the two synchronisers before VS.
        for(int t=0;t<frame_cycles;++t) {
            d.vs=t<8;
            if(change_window && t==100000) d.source_w=b.w-1;
            // Stress the shared bounds register with a much shorter blank
            // interval than upstream scan-out's >18,000 video clocks.
            int active_t=t-(std::getenv("NATIVE_EARLY_CAPTURE")?2048:18000);
            int iy=active_t/790, ix=active_t%790;
            bool active=active_t>=0 && iy<240 && (active_t%790)<640;
            d.de=active && !blank_capture; d.skip=0; d.hs=active_t>=0 && active_t%790==0;
            d.rgb=active?colour(f,ix,iy):0;
            // Set the next menu values well before the next frame starts.
            if(t==frame_cycles-20) { d.screen_mode=new_mode; d.docked=new_dock; }
            tick();
        }
    }
};
int main(int argc,char**argv) {
    Verilated::commandArgs(argc,argv);
    try {
        Test t; t.d.rst_sys=1; t.d.rst_vid=1;
        for(int i=0;i<20;++i)t.tick(false);
        t.d.rst_sys=0;t.d.rst_vid=0;
        if(std::getenv("NATIVE_EARLY_CAPTURE")) {
            for(int f=0;f<30;++f)
                t.run_frame(f,f/6,0,f%6<3?Bounds{0,0,640,240}:Bounds{20,9,597,222},false);
            std::cout<<"PASS early capture and changing bounds\n";return 0;
        }
        if(std::getenv("NATIVE_TRANSITION")) {
            for(int f=0;f<12;++f) t.run_frame(f,f<6?3:0,f<6?0:1,f<6?Bounds{0,0,0,0}:Bounds{0,0,640,240},false);
            std::cout<<"PASS transition\n";return 0;
        }
        if(std::getenv("NATIVE_DROPPED_FRAME")) {
            for(int f=0;f<28;++f)
                t.run_frame(f,f<14?2:4,0,{0,0,640,240},false,false,f==7 || f==8 || f==21 || f==22);
            std::cout<<"PASS missing capture and recovery\n";return 0;
        }
        // Set settings in advance; each block includes two settling frames.
        std::vector<Bounds> windows;
        for(int cv: {0,8,12}) for(int cr: {0,1}) {
            windows.push_back({0,cv,640-2*cr,240-2*cv});
            windows.push_back({28,13+cv,584-2*cr,213-2*cv});
        }
        windows.push_back({120,30,240,180});
        windows.push_back({64,45,192,16});
        windows.push_back({20,9,597,222}); // non-integer VI scale/window
        windows.push_back({1,0,639,240}); // odd native width and centring
        windows.push_back({639,239,1,1}); // last pixel in both axes
        windows.push_back({0,0,0,0});
        int f=0;
        for(int dock=0;dock<2;++dock) for(auto b:windows) for(int mode=0;mode<5;++mode) {
            // Menu changes occur while running and cross the synchronisers
            // before they are announced; only the following frame uses them.
            for(int n=0;n<3;++n) { t.run_frame(f,mode,dock,b,((f/3)&1)!=0); ++f; }
        }
        // Every directed pair of modes, including Dock -> handheld.
        for(int dock=0;dock<2;++dock) for(int a=0;a<5;++a) for(int b=0;b<5;++b)
            for(int mode: {a,b}) for(int n=0;n<3;++n) {
                t.run_frame(f,mode,dock,{28,21,582,197},false); ++f;
            }
        for(int a=0;a<5;++a) for(int b=0;b<5;++b)
            check(t.transitions[a][b],"missing mode transition");
        // A window changing within capture invalidates just that source
        // frame, instead of mixing its geometry with the next picture.
        for(int n=0;n<6;++n) { t.run_frame(f,2,0,{28,21,582,197},false,n==3); ++f; }
#ifndef REAL_CONTROLLER
        // A memory outage fills the capture FIFO. The incomplete capture
        // must be discarded and the next complete frame must recover.
        t.mem.pause_until=t.mem.cycles+208542*5;
        t.force_black=true;
        t.run_frame(f++,2,0,{28,21,582,197},false);
        t.force_black=false; t.source_valid=false;
        for(int n=0;n<3;++n) { t.run_frame(f++,2,0,{28,21,582,197},false); }
#endif
        check(t.hybrid_pixels>1000000,"insufficient Hybrid coverage");
        check(t.native_pixels>1000000 && t.filtered_pixels>1000000,"insufficient native/filter coverage");
        // Reset after live video must clear all FIFO, buffer and slot state.
        t.d.rst_sys=1; t.d.rst_vid=1;
        for(int n=0;n<20;++n) t.tick(false);
        check(!t.d.video_de && !t.d.video_skip && !t.d.video_vs && t.d.video_rgb==0,"reset outputs");
        std::cout<<"PASS: "<<f<<" PAL/NTSC frames, "<<t.tested<<" exact pixels; "<<t.native_pixels<<" native, "<<t.filtered_pixels<<" bilinear; "<<t.hybrid_pixels<<" Hybrid; five modes, Dock, windows, crops and transitions\n";
    } catch(const std::exception&e) { std::cerr<<"FAIL: "<<e.what()<<"\n"; return 1; }
}
