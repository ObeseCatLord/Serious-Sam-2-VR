// Private-lab observer: actual IPC images only; never creates or acknowledges frames.
#include "common/ipc.hpp"
#include "common/frame_policy.hpp"
#include "common/weapon_view.hpp"
#include "lab_window_focus.hpp"
#include <cstdio>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
using namespace ss2vr;
// Private-lab metadata only. Read without executing the helper, so a sealed but
// obsolete observer cannot pass preflight against a newer shared-memory layout.
extern "C" __declspec(dllexport) const uint32_t ss2vrObserverLayout[8] = {
    1, Magic, Abi, sizeof(Input), sizeof(Request), sizeof(Ui), sizeof(Slot), sizeof(Shared)
};
static_assert(sizeof(ss2vrObserverLayout) == 32);
static void pose(FILE *f,const Pose &p) {
    std::fprintf(f,"{\"p\":[%.9g,%.9g,%.9g],\"q\":[%.9g,%.9g,%.9g,%.9g]}",
        p.p.x,p.p.y,p.p.z,p.q.x,p.q.y,p.q.z,p.q.w);
}
static BOOL CALLBACK closeWindow(HWND window,LPARAM pid) {
    DWORD owner=0;GetWindowThreadProcessId(window,&owner);
    wchar_t title[64]{};GetWindowTextW(window,title,64);
    if(owner==static_cast<DWORD>(pid)&&wcscmp(title,L"Serious Sam 2")==0)
        PostMessageW(window,WM_CLOSE,0,0);
    return TRUE;
}
struct OwnedLoading {
    HWND window=nullptr; DWORD pid=0,thread=0; uint32_t module=0,menu=0,table=0,ready=0,stage=0,error=0;
    uint32_t simulation=0,blocked=0,exclusive=0,dispatcher=0,inputEnabled=0;
    uint32_t instance=0,running=0,simulationPresent=0,inputBlock=0,coreForeground=0;
    uint32_t hostWindow=0,hostHwnd=0,canvasWindow=0,workbenchWindow=0,keyboardWindow=0;
    bool activationRepeated=false,hostForeground=false,hostVisible=false,hostIconic=false;
    bool nativeState=false;
};
static bool loadingOwner(DWORD pid,const wchar_t *path,OwnedLoading &out) {
    HANDLE process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    out.pid=pid;out.stage=1;
    if(!process){out.error=GetLastError();return false;}
    wchar_t actual[2048]{};DWORD bytes=2048;
    const bool queried=QueryFullProcessImageNameW(process,0,actual,&bytes);
    std::wstring expected(path);
    for(auto &c:expected)if(c==L'/')c=L'\\';
    for(auto &c:actual)if(c==L'/')c=L'\\';
    bool ok=queried&&!_wcsicmp(actual,expected.c_str());
    out.stage=ok?2:1;
    HANDLE modules=ok?CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid):INVALID_HANDLE_VALUE;
    if(modules==INVALID_HANDLE_VALUE)out.error=GetLastError();
    uint32_t base=0,engine=0,core=0,exe=0;MODULEENTRY32W module{};module.dwSize=sizeof(module);
    if(modules!=INVALID_HANDLE_VALUE) {
        if(Module32FirstW(modules,&module))do {
            if(!_wcsicmp(module.szModule,L"Sam2Game.dll"))base=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module.modBaseAddr));
            if(!_wcsicmp(module.szModule,L"Engine.dll"))engine=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module.modBaseAddr));
            if(!_wcsicmp(module.szModule,L"Core.dll"))core=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module.modBaseAddr));
            if(!_wcsicmp(module.szModule,L"Sam2.exe"))exe=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module.modBaseAddr));
        }while(Module32NextW(modules,&module));
        CloseHandle(modules);
    }
    auto read=[&](uint32_t address,uint32_t &value) {
        SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void *>(uintptr_t(address)),&value,4,&got)&&got==4;
    };
    uint32_t simulation=0,blocked=0,exclusive=0,dispatcher=0,enabled=0,inputEnabled=0,againSimulation=0,againBlocked=0;
    uint32_t againProject=0,againEnabled=0,againInputEnabled=0,againExclusive=0;
    out.nativeState=base&&engine&&read(engine+0x2f1a70,simulation)&&simulation&&simulation<=UINT32_MAX-0x48&&
        read(simulation+0x48,blocked)&&read(engine+0x2e9fd4,exclusive)&&
        read(base+0x403350,dispatcher)&&dispatcher&&dispatcher<=UINT32_MAX-0x24&&read(dispatcher+0x24,enabled)&&
        read(dispatcher+0x10,inputEnabled)&&read(base+0x403350,againProject)&&againProject==dispatcher&&
        read(dispatcher+0x24,againEnabled)&&againEnabled==enabled&&read(dispatcher+0x10,againInputEnabled)&&againInputEnabled==inputEnabled&&
        read(engine+0x2e9fd4,againExclusive)&&againExclusive==exclusive&&
        read(engine+0x2f1a70,againSimulation)&&againSimulation==simulation&&read(simulation+0x48,againBlocked)&&againBlocked==blocked;
    out.simulation=simulation;out.blocked=blocked;out.exclusive=exclusive;out.dispatcher=enabled;out.inputEnabled=inputEnabled;
    auto field=[&](uint32_t pointer,uint32_t offset,uint32_t &value) {
        return pointer&&pointer<=UINT32_MAX-offset&&read(pointer+offset,value);
    };
    uint32_t instance=0,exeInstance=0,tableInstance=0,blockOwner=0,hostWindow=0,hwndPointer=0,canvas=0;
    uint32_t againInstance=0,againExeInstance=0,againSamInstance=0,againHostWindow=0,againHwndPointer=0;
    uint32_t againHwnd=0,againBlockOwner=0,againBlock=0,againForeground=0,againCanvas=0;
    uint32_t againRunning=0,againPresent=0,activationInput=0,activationDispatch=0,activationExclusive=0;
    // Three native aliases distinguish a registered current instance from the
    // Sam alias written during construction. Repeated reads are not life pins.
    out.activationRepeated=ok&&base&&engine&&core&&exe&&
        read(engine+0x2f19b0,instance)&&instance==dispatcher&&read(exe+0x849c,exeInstance)&&exeInstance==instance&&
        field(instance,0,tableInstance)&&tableInstance==base+0x29cf10&&
        field(instance,0x0c,out.running)&&field(instance,0x08,out.simulationPresent)&&
        field(instance,0x30,blockOwner)&&field(blockOwner,0xfc,out.inputBlock)&&
        read(core+0xb56f0,out.coreForeground)&&read(exe+0x8494,hostWindow)&&
        field(hostWindow,0x40,hwndPointer)&&field(hwndPointer,0,out.hostHwnd)&&
        read(core+0xc109c,out.workbenchWindow)&&read(core+0xc10d4,out.keyboardWindow)&&
        read(exe+0x8498,canvas)&&field(canvas,0x14,out.canvasWindow)&&
        read(engine+0x2f19b0,againInstance)&&againInstance==instance&&
        read(exe+0x849c,againExeInstance)&&againExeInstance==instance&&read(base+0x403350,againSamInstance)&&againSamInstance==instance&&
        read(exe+0x8494,againHostWindow)&&againHostWindow==hostWindow&&field(hostWindow,0x40,againHwndPointer)&&againHwndPointer==hwndPointer&&
        field(hwndPointer,0,againHwnd)&&againHwnd==out.hostHwnd&&field(instance,0x30,againBlockOwner)&&againBlockOwner==blockOwner&&
        field(blockOwner,0xfc,againBlock)&&againBlock==out.inputBlock&&read(core+0xb56f0,againForeground)&&againForeground==out.coreForeground&&
        read(exe+0x8498,againCanvas)&&againCanvas==canvas&&
        field(instance,0x0c,againRunning)&&againRunning==out.running&&field(instance,0x08,againPresent)&&againPresent==out.simulationPresent&&
        field(instance,0x10,activationInput)&&activationInput==inputEnabled&&field(instance,0x24,activationDispatch)&&activationDispatch==enabled&&
        read(engine+0x2e9fd4,activationExclusive)&&activationExclusive==exclusive;
    out.instance=instance;out.hostWindow=hostWindow;
    if(out.activationRepeated) {
        const HWND native=reinterpret_cast<HWND>(uintptr_t(out.hostHwnd));DWORD nativeOwner=0;
        GetWindowThreadProcessId(native,&nativeOwner);
        out.hostForeground=nativeOwner==pid&&GetForegroundWindow()==native;
        out.hostVisible=nativeOwner==pid&&IsWindowVisible(native);
        out.hostIconic=nativeOwner==pid&&IsIconic(native);
    }
    uint32_t menu=0,table=0,ready=0,again=0,tableAgain=0,readyAgain=0;
    ok=ok&&base&&read(base+0x40a270,menu)&&menu&&menu<=UINT32_MAX-0x6c&&
        read(menu,table)&&table==base+0x29f148&&read(menu+0x6c,ready)&&ready==1;
    out.module=base;out.menu=menu;out.table=table;out.ready=ready;
    if(ok)out.stage=3;
    const HWND window=GetForegroundWindow();DWORD owner=0;
    const DWORD thread=window?GetWindowThreadProcessId(window,&owner):0;
    wchar_t title[64]{};if(window)GetWindowTextW(window,title,64);
    ok=ok&&out.nativeState&&blocked==1&&exclusive&&enabled&&owner==pid&&thread&&wcscmp(title,L"Serious Sam 2")==0&&
        read(base+0x40a270,again)&&again==menu&&read(menu,tableAgain)&&tableAgain==table&&
        read(menu+0x6c,readyAgain)&&readyAgain==1;
    CloseHandle(process);
    if(ok){out.window=window;out.thread=thread;out.stage=4;}
    return ok;
}
static void writeLoadingState(FILE *output,const OwnedLoading &loading) {
    std::fprintf(output,",\"native_state_repeated\":%u,\"native_simulation\":%u,\"native_world_start_blocked\":%u,\"native_input_exclusive\":%u,\"native_input_enabled\":%u,\"native_ok_dispatch_enabled\":%u,\"native_current_menu\":%u",
        loading.nativeState,loading.simulation,loading.blocked,loading.exclusive,loading.inputEnabled,loading.dispatcher,loading.menu);
    std::fprintf(output,",\"native_activation_repeated\":%u,\"native_instance\":%u,\"native_running\":%u,\"native_simulation_present\":%u,\"native_exclusive_block\":%u,\"native_core_foreground\":%u,\"native_host_window\":%u,\"native_host_hwnd\":%u,\"native_canvas_window\":%u,\"native_workbench_window\":%u,\"native_keyboard_window\":%u,\"native_host_foreground\":%u,\"native_host_visible\":%u,\"native_host_iconic\":%u",
        loading.activationRepeated,loading.instance,loading.running,loading.simulationPresent,loading.inputBlock,loading.coreForeground,
        loading.hostWindow,loading.hostHwnd,loading.canvasWindow,loading.workbenchWindow,loading.keyboardWindow,loading.hostForeground,loading.hostVisible,loading.hostIconic);
    std::fprintf(output,",\"native_game_module\":%u,\"native_loading_menu_table\":%u",loading.module,loading.table);
}
struct OwnedStockProcess {
    HANDLE handle=nullptr; DWORD pid=0; uint64_t creation=0;
    ~OwnedStockProcess(){if(handle)CloseHandle(handle);}
};
static bool stockProcess(const wchar_t *path,OwnedStockProcess &out) {
    std::wstring expected(path);for(auto &c:expected)if(c==L'/')c=L'\\';
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return false;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool ambiguous=false;
    if(Process32FirstW(snapshot,&entry))do {
        if(_wcsicmp(entry.szExeFile,L"Sam2.exe"))continue;
        HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,entry.th32ProcessID);
        wchar_t actual[2048]{};DWORD size=2048;FILETIME created{},exited{},kernel{},user{};
        const bool match=h&&QueryFullProcessImageNameW(h,0,actual,&size)&&
            !_wcsicmp(actual,expected.c_str())&&GetProcessTimes(h,&created,&exited,&kernel,&user);
        if(match) {
            if(out.handle){ambiguous=true;CloseHandle(h);break;}
            out.handle=h;out.pid=entry.th32ProcessID;
            out.creation=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
        } else if(h)CloseHandle(h);
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return out.handle&&!ambiguous;
}
static uint32_t stockModule(DWORD pid,const wchar_t *name) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snapshot==INVALID_HANDLE_VALUE)return 0;
    uint32_t base=0;MODULEENTRY32W module{};module.dwSize=sizeof(module);
    if(Module32FirstW(snapshot,&module))do {
        if(!_wcsicmp(module.szModule,name))base=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module.modBaseAddr));
    }while(Module32NextW(snapshot,&module));
    CloseHandle(snapshot);return base;
}
static bool stockTransport(const OwnedStockProcess &process,bool &onlineNull) {
    const uint32_t engine=stockModule(process.pid,L"Engine.dll"),game=stockModule(process.pid,L"Sam2Game.dll");
    if(!engine||!game)return false;
    auto read=[&](uint32_t address,uint32_t &value) {
        SIZE_T count=0;return ReadProcessMemory(process.handle,reinterpret_cast<void *>(uintptr_t(address)),&value,4,&count)&&count==4;
    };
    uint32_t net=0,table=0,again=0,tableAgain=0,online=0,onlineAgain=0;
    const bool coherent=read(engine+0x2eb6d8,net)&&net&&read(net,table)&&table==game+0x29d270&&
        read(engine+0x2ec890,online)&&read(engine+0x2eb6d8,again)&&again==net&&
        read(net,tableAgain)&&tableAgain==table&&read(engine+0x2ec890,onlineAgain)&&onlineAgain==online;
    onlineNull=coherent&&online==0;
    // Repeated reads witness only transport identity, not ABA/lifetime or actor application.
    return coherent;
}
struct StockCameraRead {
    Pose camera{};Matrix34 view{};Matrix44 projection{};
    bool menuClear=false,equal=false,viewMatches=false,perspective=false;
    bool mouseZero=false;
};
static StockCameraRead stockCameraRead(const OwnedStockProcess &process) {
    StockCameraRead out{};
    const uint32_t engine=stockModule(process.pid,L"Engine.dll"),game=stockModule(process.pid,L"Sam2Game.dll");
    if(!engine||!game)return out;
    auto read=[&](uint32_t address,void *value,SIZE_T size) {
        SIZE_T count=0;return ReadProcessMemory(process.handle,reinterpret_cast<void *>(uintptr_t(address)),value,size,&count)&&count==size;
    };
    uint32_t menu=1,againMenu=1;
    float sensitivity=1;
    out.mouseZero=read(engine+0x2c4214,&sensitivity,sizeof(sensitivity))&&sensitivity==0;
    Pose againCamera{};Matrix34 againView{};Matrix44 againProjection{};
    const bool ok=read(game+0x40a270,&menu,4)&&
        read(game+0x403084,&out.camera,sizeof(out.camera))&&read(engine+0x2e6408,&out.view,sizeof(out.view))&&
        read(engine+0x2e6398,&out.projection,sizeof(out.projection))&&
        read(game+0x403084,&againCamera,sizeof(againCamera))&&read(engine+0x2e6408,&againView,sizeof(againView))&&
        read(engine+0x2e6398,&againProjection,sizeof(againProjection))&&read(game+0x40a270,&againMenu,4);
    if(!ok||!finite(out.camera))return {};
    for(float value:out.view.m)if(!std::isfinite(value))return {};
    for(float value:out.projection.m)if(!std::isfinite(value))return {};
    out.menuClear=ok&&menu==0&&againMenu==0;
    out.equal=ok&&!std::memcmp(&out.camera,&againCamera,sizeof(againCamera))&&
        !std::memcmp(&out.view,&againView,sizeof(againView))&&!std::memcmp(&out.projection,&againProjection,sizeof(againProjection));
    const auto &q=out.camera.q;
    const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
    out.viewMatches=out.equal&&finite(out.camera)&&norm>.99&&norm<1.01&&sameWeaponView(out.view,matrix(inverse(out.camera)));
    out.perspective=out.equal&&finiteProjection(out.projection)&&out.projection.m[0]>0&&out.projection.m[5]>0&&
        out.projection.m[14]==-1&&out.projection.m[15]==0;
    // No frame/actor owner or freshness claim: equal reads cannot exclude ABA,
    // another pass, or stale storage. These remain capture-comparison candidates.
    return out;
}
struct PrivateForegroundIdentity {
    bool enabled=false,imageQueried=false,titleQueried=false;
    unsigned role=0;uint64_t creation=0,queryTick=0;wchar_t title[64]{};
};
static PrivateForegroundIdentity privateForegroundIdentity(const lab::WindowObservation &window) {
    PrivateForegroundIdentity out{};wchar_t selected[2]{};
    out.enabled=GetEnvironmentVariableW(L"SS2VR_LAB_PRIVATE_DISPLAY",selected,2)==1&&selected[0]==L'1';
    if(!out.enabled||!window.window)return out;
    out.queryTick=GetTickCount64();
    out.titleQueried=GetWindowTextW(window.window,out.title,64)>0;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,window.owner);
    if(!process)return out;
    wchar_t path[2048]{};DWORD count=2048;FILETIME created{},exited{},kernel{},user{};
    out.imageQueried=QueryFullProcessImageNameW(process,0,path,&count)!=FALSE;
    if(count>=2048)out.imageQueried=false;
    if(out.imageQueried)path[count]=0;
    if(GetProcessTimes(process,&created,&exited,&kernel,&user))out.creation=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(out.imageQueried) {
        const wchar_t *name=path;
        for(const wchar_t *p=path;*p;++p)if(*p==L'\\'||*p==L'/')name=p+1;
        const wchar_t *roles[]={L"explorer.exe",L"steam.exe",L"conhost.exe",L"Sam2.exe",L"ss2vr_host.exe",L"wineconsole.exe"};
        for(unsigned i=0;i<6;++i)if(!_wcsicmp(name,roles[i]))out.role=i+1;
    }
    CloseHandle(process);return out;
}
static void privateIdentity(FILE *file,const char *label,const PrivateForegroundIdentity &value) {
    std::fprintf(file,",\"%s_identity\":{\"private_enabled\":%u,\"query_after_request\":true,\"query_tick_ms\":%llu,\"image_queried\":%u,\"image_role\":%u,\"process_creation\":%llu,\"title_queried\":%u,\"title\":\"",
        label,value.enabled,static_cast<unsigned long long>(value.queryTick),value.imageQueried,value.role,static_cast<unsigned long long>(value.creation),value.titleQueried);
    for(const wchar_t *p=value.title;*p;++p)std::fprintf(file,"\\u%04x",static_cast<unsigned>(*p));
    std::fputs("\"}",file);
}
static int stockFocus(int argc,wchar_t **argv) {
    // Open the optional exclusive receipt before ownership guards so rejected
    // attempts remain distinguishable. It never authorizes a window operation.
    FILE *receipt=argc==5?_wfopen(argv[4],L"wbx"):nullptr;
    if(argc==5&&!receipt)return 6;
    OwnedStockProcess process;
    OwnedLoading native{};
    lab::WindowObservation target{};
    lab::FocusObservation focus{};
    bool identityVerified=false,creationVerified=false,targetChecked=false,called=false;
    auto finish=[&](const char *stage,int code) {
        if(!receipt)return code;
        const int written=std::fprintf(receipt,
            "{\"schema\":1,\"operation\":\"stock-focus\",\"stage\":\"%s\",\"returncode\":%d,"
            "\"process_identity_verified\":%u,\"game_pid\":%u,\"process_creation\":%llu,\"creation_verified\":%u,"
            "\"activation_repeated\":%u,\"target_checked\":%u,\"native_host_hwnd\":%llu,\"target_owner\":%u,\"target_thread\":%u,"
            "\"target_title_matches\":%u,\"target_root_matches\":%u,\"visible_before\":%u,\"iconic_before\":%u,"
            "\"foreground_called\":%u,\"foreground_before_hwnd\":%llu,\"foreground_before_owner\":%u,\"foreground_before_thread\":%u,"
            "\"show_called\":%u,\"show_requested\":%u,\"foreground_requested\":%u,\"foreground_observed\":%u,"
            "\"foreground_after_hwnd\":%llu,\"foreground_after_owner\":%u,\"foreground_after_thread\":%u,\"foreground_after_title_matches\":%u",
            stage,code,identityVerified,static_cast<unsigned>(process.pid),static_cast<unsigned long long>(process.creation),creationVerified,
            native.activationRepeated,targetChecked,lab::windowValue(target.window),static_cast<unsigned>(target.owner),static_cast<unsigned>(target.thread),
            target.titleMatches,target.rootMatches,target.visible,target.iconic,called,
            lab::windowValue(focus.before.window),static_cast<unsigned>(focus.before.owner),static_cast<unsigned>(focus.before.thread),
            focus.showCalled,focus.showRequested,focus.foregroundRequested,focus.foregroundObserved,
            lab::windowValue(focus.after.window),static_cast<unsigned>(focus.after.owner),static_cast<unsigned>(focus.after.thread),focus.after.titleMatches);
        if(called) {
            privateIdentity(receipt,"foreground_before",privateForegroundIdentity(focus.before));
            privateIdentity(receipt,"foreground_after",privateForegroundIdentity(focus.after));
        }
        std::fputs("}\n",receipt);
        const bool failed=written<0||std::ferror(receipt);
        const int closed=std::fclose(receipt);receipt=nullptr;
        return failed||closed!=0?7:code;
    };
    if(!stockProcess(argv[2],process))return finish("process-identity",4);
    identityVerified=true;
    wchar_t *end=nullptr;const auto expected=std::wcstoull(argv[3],&end,10);
    if(!expected||!end||*end||expected!=process.creation)return finish("process-creation",8);
    creationVerified=true;
    loadingOwner(process.pid,argv[2],native);
    if(!native.activationRepeated)return finish("native-association",8);
    target=lab::observeWindow(reinterpret_cast<HWND>(uintptr_t(native.hostHwnd)),L"Serious Sam 2");
    targetChecked=true;
    if(target.owner!=process.pid)return finish("window-owner",8);
    if(!target.titleMatches)return finish("window-title",8);
    if(!target.rootMatches)return finish("window-root",8);
    // Same activation policy as before; process handle remains retained. No
    // waits, input attachment, foreign-window operations or native state writes.
    called=true;focus=lab::requestBorrowedForeground(target.window,target.iconic,L"Serious Sam 2");
    return finish("foreground-observed",focus.foregroundObserved?0:8);
}
static int stockCommand(int argc,wchar_t **argv) {
    if(argc!=4&&!(argc==5&&(!wcscmp(argv[1],L"stock-focus")||!wcscmp(argv[1],L"stock-background"))))return 2;
    if(!wcscmp(argv[1],L"stock-focus"))return stockFocus(argc,argv);
    OwnedStockProcess process;if(!stockProcess(argv[2],process))return 4;
    if(!wcscmp(argv[1],L"stock-status")) {
        auto *file=_wfopen(argv[3],L"wbx");if(!file)return 6;
        OwnedLoading loading{};const bool ready=loadingOwner(process.pid,argv[2],loading);
        bool onlineNull=false;const bool local=stockTransport(process,onlineNull);
        const auto camera=stockCameraRead(process);
        std::fprintf(file,"{\"game_pid\":%u,\"process_creation\":%llu,\"tick_ms\":%llu,\"local_transport\":%u,\"online_interface_null\":%u,\"loading_ready\":%u,\"loading_stage\":%u,\"loading_error\":%u,\"loading_table_rva\":%u,\"loading_native_ready\":%u,\"menu_clear\":%u,\"camera_repeated_equal\":%u,\"view_pose_match\":%u,\"perspective_projection\":%u,\"mouse_zero\":%u",
            static_cast<unsigned>(process.pid),static_cast<unsigned long long>(process.creation),static_cast<unsigned long long>(GetTickCount64()),local,onlineNull,ready,loading.stage,loading.error,
            loading.module&&loading.table?loading.table-loading.module:0,loading.ready,camera.menuClear,camera.equal,camera.viewMatches,camera.perspective,camera.mouseZero);
        writeLoadingState(file,loading);
        std::fprintf(file,",\"camera\":");
        pose(file,camera.camera);std::fprintf(file,",\"projection\":[");
        for(unsigned i=0;i<16;++i)std::fprintf(file,"%s%.9g",i?",":"",camera.projection.m[i]);
        std::fprintf(file,"]}\n");
        return std::fclose(file)==0?0:7;
    }
    wchar_t *end=nullptr;const auto expected=std::wcstoull(argv[3],&end,10);
    if(!expected||!end||*end||expected!=process.creation)return 8;
    // Retain the original process handle through posting to prevent PID reuse.
    if(!wcscmp(argv[1],L"stock-background")) {
        OwnedLoading native{};loadingOwner(process.pid,argv[2],native);
        if(!native.activationRepeated)return 8;
        const HWND window=reinterpret_cast<HWND>(uintptr_t(native.hostHwnd));DWORD owner=0;
        GetWindowThreadProcessId(window,&owner);wchar_t title[64]{};GetWindowTextW(window,title,64);
        if(owner!=process.pid||wcscmp(title,L"Serious Sam 2")||GetAncestor(window,GA_ROOT)!=window)return 8;
        FILE *receipt=argc==5?_wfopen(argv[4],L"wbx"):nullptr;
        if(argc==5&&!receipt)return 6;
        const bool visible=IsWindowVisible(window),iconic=IsIconic(window);
        if(!wcscmp(argv[1],L"stock-background")) {
            // Explicit private-lab diagnostic on the same retained process and
            // native-associated root window. No flags or foreign input injected.
            const bool requested=ShowWindowAsync(window,SW_MINIMIZE);
            if(receipt) {
                std::fprintf(receipt,"{\"native_host_hwnd\":%u,\"visible_before\":%u,\"iconic_before\":%u,\"minimize_requested\":%u}\n",
                    native.hostHwnd,visible,iconic,requested);
                if(std::fclose(receipt)!=0)return 7;
            }
            return requested?0:8;
        }
    }
    if(!wcscmp(argv[1],L"stock-close")) {
        EnumWindows(closeWindow,static_cast<LPARAM>(process.pid));return 0;
    }
    if(wcscmp(argv[1],L"stock-continue-loading"))return 2;
    OwnedLoading loading{};if(!loadingOwner(process.pid,argv[2],loading))return 8;
    const bool down=PostMessageW(loading.window,WM_KEYDOWN,VK_RETURN,0x001c0001);
    const bool up=PostMessageW(loading.window,WM_KEYUP,VK_RETURN,static_cast<LPARAM>(0xc01c0001u));
    return down&&up?0:9;
}
int wmain(int argc,wchar_t **argv) {
    if(argc<3)return 2;
    if(!wcscmp(argv[1],L"private-foreground-status")) {
        wchar_t selected[2]{};
        if(argc!=3 || argv[2][0]!=L'Z' || argv[2][1]!=L':' ||
           GetEnvironmentVariableW(L"SS2VR_LAB_PRIVATE_DISPLAY",selected,2)!=1 || selected[0]!=L'1')return 10;
        // Nongame read-only observation: no target selection/activation or game
        // memory access. A console build may observe its own foreground window.
        auto *file=_wfopen(argv[2],L"wbx");if(!file)return 6;
        const ULONGLONG entryTick=GetTickCount64();
        Sleep(250); // Fixed opportunity for independent owner-side sampling.
        const auto window=lab::observeWindow(GetForegroundWindow(),L"SS2VR private display prerequisite");
        const int written=std::fprintf(file,"{\"schema\":1,\"operation\":\"private-foreground-status\",\"entry_tick_ms\":%llu,\"tick_ms\":%llu,\"observer_pid\":%lu,\"observer_thread\":%lu,\"console_hwnd\":%llu,\"foreground_hwnd\":%llu,\"foreground_owner\":%lu,\"foreground_thread\":%lu,\"probe_title_matches\":%s,\"foreground_visible\":%s}\n",
            static_cast<unsigned long long>(entryTick),static_cast<unsigned long long>(GetTickCount64()),GetCurrentProcessId(),GetCurrentThreadId(),lab::windowValue(GetConsoleWindow()),
            lab::windowValue(window.window),window.owner,window.thread,
            window.titleMatches?"true":"false",window.visible?"true":"false");
        const int closed=std::fclose(file);return written<0||closed!=0?7:0;
    }
    if(!wcsncmp(argv[1],L"stock-",6))return stockCommand(argc,argv);
    Channel channel;if(!channel.open(argv[2],false))return 3;
    if(wcscmp(argv[1],L"status")==0) {
        Lock lock(channel,20);if(!lock)return 4;
        if(argc!=3&&argc!=5)return 2;
        FILE *output=argc==5 ? _wfopen(argv[3],L"wbx") : stdout;
        if(!output)return 6;
        const auto &s=*channel.shared;const auto &i=s.latest;
        const auto now=GetTickCount64(), age=now>=i.tickMs ? now-i.tickMs : ~uint64_t{};
        std::fprintf(output,"{\"abi\":%u,\"game_pid\":%u,\"host_pid\":%u,\"renderer\":%u,\"gameplay\":%u,\"menu\":%u,\"input_sequence\":%llu,\"input_tick_ms\":%llu,\"input_age_ms\":%llu,\"focused\":%u,\"head_valid\":%u,\"head\":",
            s.abi,s.gamePid,s.hostPid,s.rendererReady,s.ui.gameplay,s.menu.visible,
            static_cast<unsigned long long>(i.sequence),static_cast<unsigned long long>(i.tickMs),
            static_cast<unsigned long long>(age),i.focused,i.headValid);
        pose(output,i.head);std::fprintf(output,",\"hands\":[");pose(output,i.hand[0]);std::fprintf(output,",");pose(output,i.hand[1]);
        std::fprintf(output,"],\"axes\":[[%.9g,%.9g],[%.9g,%.9g]],\"grip_valid\":[%u,%u],\"grips\":[",
            i.axis[0][0],i.axis[0][1],i.axis[1][0],i.axis[1][1],i.gripValid[0],i.gripValid[1]);pose(output,i.grip[0]);std::fprintf(output,",");pose(output,i.grip[1]);
        const auto &ui=s.ui;
        std::fprintf(output,"],\"session\":%u,\"reference\":%u,\"tracking_generation\":%u,\"hand_valid\":[%u,%u],\"trigger\":[%.9g,%.9g],\"primary_active_mask\":%u,\"primary_generations\":[%u,%u],\"ui_tick_ms\":%llu,\"ui_tracking_generation\":%u,\"health\":%d,\"armor\":%d,\"current_weapon\":[%d,%d],\"current_ammo\":[%d,%d],\"fire_sequence\":[%u,%u],\"wheel_open\":[%u,%u]",
            i.session,i.reference,s.trackingGeneration,i.handValid[0],i.handValid[1],i.trigger[0],i.trigger[1],
            i.primaryActiveMask,i.primaryInputGeneration[0],i.primaryInputGeneration[1],
            static_cast<unsigned long long>(ui.tickMs),ui.trackingGeneration,ui.health,ui.armor,
            ui.currentWeapon[0],ui.currentWeapon[1],ui.currentAmmo[0],ui.currentAmmo[1],
            ui.fireSequence[0],ui.fireSequence[1],ui.wheel[0].open,ui.wheel[1].open);
        OwnedLoading loading{};
        const bool ready=argc==5&&loadingOwner(s.gamePid,argv[4],loading);
        writeLoadingState(output,loading);
        std::fprintf(output,",\"slots\":[%u,%u],\"loading_ready\":%u,\"loading_stage\":%u,\"loading_error\":%u,\"loading_table_rva\":%u,\"loading_native_ready\":%u}\n",static_cast<unsigned>(s.slot[0].state),static_cast<unsigned>(s.slot[1].state),ready,loading.stage,loading.error,loading.module&&loading.table?loading.table-loading.module:0,loading.ready);

        return output==stdout ? (std::fflush(output)==0?0:7) : (std::fclose(output)==0?0:7);
    }
    if(wcscmp(argv[1],L"continue-loading")==0) {
        if(argc!=4&&argc!=5)return 2;
        DWORD pid=0;{Lock lock(channel,100);if(!lock)return 4;pid=channel.shared->gamePid;}
        OwnedLoading loading{};if(!loadingOwner(pid,argv[3],loading))return 8;
        FILE *receipt=argc==5?_wfopen(argv[4],L"wbx"):nullptr;
        if(argc==5&&!receipt)return 6;
        const bool down=PostMessageW(loading.window,WM_KEYDOWN,VK_RETURN,0x001c0001);
        // Always attempt release; do not retain a key or retry this action.
        const bool up=PostMessageW(loading.window,WM_KEYUP,VK_RETURN,static_cast<LPARAM>(0xc01c0001u));
        if(receipt) {
            std::fprintf(receipt,"{\"game_pid\":%u,\"native_simulation\":%u,\"native_world_start_blocked\":%u,\"native_input_exclusive\":%u,\"native_ok_dispatch_enabled\":%u,\"native_current_menu\":%u,\"down_posted\":%u,\"up_posted\":%u}\n",
                static_cast<unsigned>(pid),loading.simulation,loading.blocked,loading.exclusive,loading.dispatcher,loading.menu,down,up);
            if(std::fclose(receipt)!=0)return 7;
        }
        return down&&up?0:9;
    }
    if(wcscmp(argv[1],L"close")==0) {
        DWORD pid=0;{Lock lock(channel,100);if(!lock)return 4;pid=channel.shared->gamePid;}
        EnumWindows(closeWindow,static_cast<LPARAM>(pid));return 0;
    }
    if(wcscmp(argv[1],L"capture")!=0||argc!=12)return 2;
    std::vector<uint8_t> pixels[2];for(auto &p:pixels)p.resize(EyeBytes);
    wchar_t *end=nullptr;
    const auto minimumInput=std::wcstoull(argv[4],&end,10);if(!end||*end)return 2;
    float expected[7]{};
    for(unsigned i=0;i<7;++i) {
        expected[i]=std::wcstof(argv[5+i],&end);if(!end||*end||!std::isfinite(expected[i]))return 2;
    }
    Request request{};uint32_t presentation=0;bool found=false;
    const auto deadline=GetTickCount64()+15000;
    while(GetTickCount64()<deadline&&!found) {
        {Lock lock(channel,2);if(lock) {
            const Slot *best=nullptr;
            const auto &shared=*channel.shared;
            for(const auto &s:shared.slot) {
                const auto &head=s.request.input.head;
                const double norm=double(head.q.x)*head.q.x+double(head.q.y)*head.q.y+
                    double(head.q.z)*head.q.z+double(head.q.w)*head.q.w;
                const double dot=double(head.q.x)*expected[3]+double(head.q.y)*expected[4]+
                    double(head.q.z)*expected[5]+double(head.q.w)*expected[6];
                const bool poseMatches=std::isfinite(norm)&&std::abs(norm-1)<.001&&std::isfinite(dot)&&std::abs(dot)>.99999&&
                    std::isfinite(head.p.x)&&std::isfinite(head.p.y)&&std::isfinite(head.p.z)&&
                    std::abs(head.p.x-expected[0])<.001&&std::abs(head.p.y-expected[1])<.001&&std::abs(head.p.z-expected[2])<.001;
                if(s.state==SlotState::Ready&&!s.cancelled&&s.request.input.sequence>=minimumInput&&
                    shared.rendererReady&&shared.ui.gameplay&&!shared.menu.visible&&s.request.input.focused&&s.request.input.headValid&&
                    poseMatches&&!s.request.reserved&&!s.presentationReserved&&s.presentation==NativeUiComplete&&s.request.uiRequested==1&&
                    s.request.trackingGeneration==trackingEpoch(shared)&&
                    s.request.session==shared.latest.session&&s.request.reference==shared.latest.reference&&
                    GetTickCount64()>=s.request.input.tickMs&&GetTickCount64()-s.request.input.tickMs<=FrameAgeMs&&s.request.width&&s.request.height&&
                    s.request.width<=MaxDimension&&s.request.height<=MaxDimension&&
                    (!best||s.request.sequence>best->request.sequence))best=&s;
            }
            if(best) {
                request=best->request;presentation=best->presentation;
                for(unsigned h=0;h<2;++h)std::memcpy(pixels[h].data(),best->pixels[h],size_t(request.width)*request.height*4);
                found=true;
            }
        }}
        if(!found)Sleep(1);
    }
    if(!found)return 5;
    for(unsigned h=0;h<2;++h) {
        auto name=std::wstring(argv[3])+(h?L"-right.ppm":L"-left.ppm");
        auto *f=_wfopen(name.c_str(),L"wbx");if(!f)return 6;
        std::fprintf(f,"P6\n%u %u\n255\n",request.width,request.height);
        std::vector<uint8_t> row(size_t(request.width)*3);
        for(unsigned y=0;y<request.height;++y) {
            for(unsigned x=0;x<request.width;++x) {
                const auto offset=(size_t(y)*request.width+x)*4;
                row[x*3]=pixels[h][offset+2];row[x*3+1]=pixels[h][offset+1];row[x*3+2]=pixels[h][offset];
            }
            if(std::fwrite(row.data(),1,row.size(),f)!=row.size()){std::fclose(f);return 7;}
        }
        if(std::fclose(f)!=0)return 7;
    }
    auto name=std::wstring(argv[3])+L"-metadata.json";
    auto *f=_wfopen(name.c_str(),L"wbx");if(!f)return 6;
    std::fprintf(f,"{\"sequence\":%llu,\"input_sequence\":%llu,\"input_tick_ms\":%llu,\"width\":%u,\"height\":%u,\"presentation\":%u,\"session\":%u,\"reference\":%u,\"tracking_generation\":%u,\"ui_requested\":%u,\"head\":",
        static_cast<unsigned long long>(request.sequence),static_cast<unsigned long long>(request.input.sequence),
        static_cast<unsigned long long>(request.input.tickMs),request.width,request.height,presentation,request.session,request.reference,request.trackingGeneration,request.uiRequested);
    pose(f,request.input.head);std::fprintf(f,",\"hands\":[");pose(f,request.input.hand[0]);std::fprintf(f,",");pose(f,request.input.hand[1]);
    std::fprintf(f,"],\"grip_valid\":[%u,%u],\"grips\":[",request.input.gripValid[0],request.input.gripValid[1]);pose(f,request.input.grip[0]);std::fprintf(f,",");pose(f,request.input.grip[1]);
    std::fprintf(f,"],\"eyes\":[");pose(f,request.eye[0]);std::fprintf(f,",");pose(f,request.eye[1]);
    std::fprintf(f,"],\"trigger\":[%.9g,%.9g],\"hand_valid\":[%u,%u],\"primary_active_mask\":%u,\"primary_generations\":[%u,%u],\"fov\":[",
        request.input.trigger[0],request.input.trigger[1],request.input.handValid[0],request.input.handValid[1],
        request.input.primaryActiveMask,request.input.primaryInputGeneration[0],request.input.primaryInputGeneration[1]);
    for(unsigned h=0;h<2;++h) {
        if(h)std::fprintf(f,",");
        const auto &v=request.fov[h];
        std::fprintf(f,"[%.9g,%.9g,%.9g,%.9g]",v.left,v.right,v.up,v.down);
    }
    std::fprintf(f,"]}\n");return std::fclose(f)==0?0:7;
}

// GUI subsystem avoids creating a console that can steal the owned game's focus.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,wchar_t *,int) {
    int argc=0;auto **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    const int result=wmain(argc,argv);LocalFree(argv);return result;
}
