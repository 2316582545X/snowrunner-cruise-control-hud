#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "MinHook.h"
#include "NativeUi.h"
#if defined(SR_CC_SIGNATURE_DIAGNOSTIC) || defined(SR_CC_SIGNATURE_RESOLVER)
#include "../../compatibility/CompatibilityReport.h"
#include "../../compatibility/ExperimentalPolicy.h"
#endif
#ifdef SCALEFORM_NATIVE_AUTOPILOT
#include "SnowRunnerCCVersion.h"
#include "NativeSession.h"
#include "NativeControl.h"
#endif

namespace probe {
// Native HUD and diagnostic hooks support only the EXE whose complete
// SHA256 is checked before any hook is installed.
constexpr uintptr_t createRva = 0x10243a0, attachRva = 0x1b61880, getRva = 0x1b5d4b0;
constexpr unsigned char createPrefix[] = {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x41,0x54};
constexpr unsigned char attachPrefix[] = {0x4c,0x8b,0xdc,0x4d,0x89,0x4b,0x20,0x4d,0x89,0x43,0x18,0x55};
constexpr char expectedHash[] = "a3cb442ec138eb2eb7e335e5c80e376766550740e49e233ac9eace035e182285";

// Observed GFx value: intrusive-list links precede its managed payload.
// Never retain, release or manufacture a game-owned value in this observer.
using ObservedValue = nativeui::Value;
static_assert(sizeof(ObservedValue) == 48);
static_assert(offsetof(ObservedValue, type) == 0x18);
using Create = uintptr_t(*)(void*, const void*);
using Attach = bool(*)(void*, void*, ObservedValue*, const char*, const char*, int, const void*);
Create originalCreate = nullptr;
Attach originalAttach = nullptr;
nativeui::Get originalGet = nullptr;
nativeui::Api ui{};
bool enableTestRow = false;
// Diagnostic: when set, getHook forwards straight to the original getter with no
// string parse, snapshot or VM entry. Isolates a bad patch/trampoline from a
// callback-body bug.
bool getPassthrough = false;
#ifdef SR_CC_ALLOW_UNREVIEWED
FILE* checkpointLog=nullptr;
#endif
std::atomic<uintptr_t> attemptedPanel{0};
thread_local bool insideUi = false;
#ifdef SCALEFORM_NATIVE_AUTOPILOT
constexpr uintptr_t advanceRva=0x19960b0,destroyRva=0x19a1730;
using Advance=float(*)(void*,float,unsigned,bool);
using Destroy=uintptr_t(*)(void*,unsigned);
Advance originalAdvance=nullptr;
Destroy originalDestroy=nullptr;
// Keep the registry allocated until process exit. Normal Movie destruction
// releases its entries before the VM dies; no C++ global destructor calls it.
bool read(const void* from,void* to,size_t bytes);
nativeui::Session& sessions=*new nativeui::Session(ui,read);
std::atomic<unsigned> nativeFrames{0};
void registerRow(const ObservedValue& panel,const ObservedValue& row);
#endif

bool read(const void* from, void* to, size_t bytes) {
    SIZE_T copied = 0;
    return from && ReadProcessMemory(GetCurrentProcess(), from, to, bytes, &copied) && copied == bytes;
}
template<size_t N> void textAt(const char* address, char (&output)[N]) {
    output[0] = 0;
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
    const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    const auto available = std::min<size_t>(N - 1, end - reinterpret_cast<uintptr_t>(address));
    if (!read(address, output, available)) { output[0] = 0; return; }
    output[available] = 0;
    for (size_t i = 0; output[i]; ++i) if (static_cast<unsigned char>(output[i]) < 32) { output[i] = 0; break; }
}
template<size_t N> void gameString(const void* address, char (&output)[N]) {
    struct Header { uint32_t length, inlineCapacity; uintptr_t data; } h{};
    output[0] = 0;
    if (!read(address, &h, sizeof(h)) || h.length == 0 || h.length > 4096) return;
    const char* characters = h.inlineCapacity ? reinterpret_cast<const char*>(address) + 8 : reinterpret_cast<const char*>(h.data);
    textAt(characters, output);
}
struct Event {
    DWORD thread; unsigned kind; uintptr_t owner, parent, output;
    uint32_t type; uintptr_t interfacePointer, object;
    int depth; bool success; char symbol[128], instance[128];
};
constexpr unsigned capacity = 4096;
Event events[capacity]{};
std::atomic<bool> ready[capacity]{};
std::atomic<unsigned> claimed{0}, observed{0};
void publish(const Event& event) {
    const unsigned index = claimed.fetch_add(1, std::memory_order_relaxed);
    if (index >= capacity) return;
    events[index] = event; ready[index].store(true, std::memory_order_release);
}
void traceUi(const char* operation,const char* member,bool success) {
#ifdef SR_CC_ALLOW_UNREVIEWED
    if(checkpointLog){std::fprintf(checkpointLog,"CHECKPOINT thread=%lu operation=%s member=%s ok=%u\n",GetCurrentThreadId(),operation,member,success);std::fflush(checkpointLog);}
#endif
    Event e{}; e.kind=5; e.thread=GetCurrentThreadId(); e.success=success;
    std::snprintf(e.symbol,sizeof(e.symbol),"UI-%s",operation);
    std::snprintf(e.instance,sizeof(e.instance),"%s",member);
    publish(e);
}
#ifdef SCALEFORM_NATIVE_AUTOPILOT
void controlEvent(int state,const char* reason) {
    Event e{};e.kind=6;e.thread=GetCurrentThreadId();e.type=state;e.success=state==1;
    std::snprintf(e.symbol,sizeof(e.symbol),"CONTROL-%s",state==1?"ON":state==2?"ERROR":"OFF");
    std::snprintf(e.instance,sizeof(e.instance),"%s",reason);publish(e);
}
void registerRow(const ObservedValue& panel,const ObservedValue& row) {
    void* movie=nullptr;
    if(read(reinterpret_cast<void*>(panel.interfacePointer+8),&movie,sizeof(movie))&&movie)sessions.add(movie,panel,row);
}
float advanceHook(void* movie,float delta,unsigned catchUp,bool capture) {
    // Observe native visibility after the game's current-frame AS3 update.
    const float result=originalAdvance(movie,delta,catchUp,capture);
    if(!insideUi && sessions.has(movie)) {
        ++nativeFrames;
        insideUi=true;
        const auto label=nativecontrol::label();
        nativecontrol::hudVisible=sessions.tick(movie,nativecontrol::state.load(),label.c_str(),delta);
        nativecontrol::hudStamp=GetTickCount64();
        insideUi=false;
    }
    return result;
}
uintptr_t destroyHook(void* movie,unsigned flags) {
    if(sessions.has(movie)) {
        sessions.drop(movie);nativecontrol::hudVisible=false;nativecontrol::hudStamp=0;
    }
    return originalDestroy(movie,flags);
}
#endif
bool relevant(const char* name) {
    // Bounded sample of other symbols establishes whether the hook executes.
    const auto index = observed.fetch_add(1, std::memory_order_relaxed);
    return index < 96 || std::strstr(name, "Hud") || std::strstr(name, "hud") ||
        std::strstr(name, "brake") || std::strstr(name, "Brake") || std::strstr(name, "game");
}
void snapshot(Event& e, const ObservedValue* value) {
    ObservedValue copy{};
    if (read(value, &copy, sizeof(copy))) {
        e.type = copy.type; e.interfacePointer = copy.interfacePointer; e.object = copy.object;
    }
}
uintptr_t createHook(void* owner, const void* linkage) {
    Event e{}; e.kind = 1; e.owner = reinterpret_cast<uintptr_t>(owner); e.thread = GetCurrentThreadId();
    gameString(linkage, e.symbol);
    const auto result = originalCreate(owner, linkage);
    if (!relevant(e.symbol)) return result;
    // The original factory stores its created display object at owner + 0x58.
    snapshot(e, reinterpret_cast<const ObservedValue*>(reinterpret_cast<uintptr_t>(owner) + 0x58));
    publish(e);
    if (!std::strcmp(e.symbol,"defaultHud")) {
        attemptedPanel.store(0);
        Event test{}; test.kind = 3; test.thread = GetCurrentThreadId();
        // Creation can run on a loading worker before the UI is ready.
        // Never enter the VM here. First observe the game's own field reads.
        std::strcpy(test.symbol,enableTestRow ? "TEST-await-brake-read" : "TEST-disabled");
        publish(test);
    }
    return result;
}
bool attachHook(void* interfacePointer, void* parent, ObservedValue* result,
                const char* symbol, const char* instance, int depth, const void* init) {
    Event e{}; e.kind = 2; e.owner = reinterpret_cast<uintptr_t>(interfacePointer);
    e.parent = reinterpret_cast<uintptr_t>(parent); e.output = reinterpret_cast<uintptr_t>(result);
    e.thread = GetCurrentThreadId(); e.depth = depth;
    textAt(symbol, e.symbol); textAt(instance, e.instance);
    const bool success = originalAttach(interfacePointer, parent, result, symbol, instance, depth, init);
    if (relevant(e.symbol)) { e.success = success; snapshot(e, result); publish(e); }
    return success;
}
bool getHook(void* interfacePointer, void* parent, const char* name, ObservedValue* result, bool isDisplay) {
    if(getPassthrough) return originalGet(interfacePointer,parent,name,result,isDisplay);
    char member[128]{}; textAt(name,member);
    const bool wanted = !std::strcmp(member,"rightHudPanelContainer") || !std::strcmp(member,"brakeBtn") ||
        !std::strcmp(member,"wdBtn") || !std::strcmp(member,"diffBtn") || !std::strcmp(member,"gearBox");
    Event e{};
    if (wanted) {
        e.kind=4; e.thread=GetCurrentThreadId(); e.owner=reinterpret_cast<uintptr_t>(interfacePointer);
        e.parent=reinterpret_cast<uintptr_t>(parent); e.output=reinterpret_cast<uintptr_t>(result);
        std::strcpy(e.symbol,member);
    }
    const bool success = originalGet(interfacePointer,parent,name,result,isDisplay);
    if (wanted) { e.success=success; snapshot(e,result); publish(e); }
    if (enableTestRow && !insideUi && success && isDisplay && !std::strcmp(member,"brakeBtn")) {
        ObservedValue brake;
        const auto identity=reinterpret_cast<uintptr_t>(parent);
        if (read(result,&brake,sizeof(brake)) && nativeui::object(brake) &&
            attemptedPanel.exchange(identity)!=identity) {
            // Borrow the parent's identity only during this game-owned call.
            // No raw object is dereferenced later or retained across frames.
            ObservedValue panel;
            panel.type=0x4a; panel.interfacePointer=reinterpret_cast<uintptr_t>(interfacePointer); panel.object=identity;
            insideUi=true;
#ifdef SCALEFORM_NATIVE_AUTOPILOT
            auto language=hudlocale::Language::English;
            hudlocale::fromBrake(ui,brake,read,language);
            const std::string label=hudlocale::label(language);
            const auto keyLabel=nativecontrol::keyLabel();
            const nativeui::RowOptions options{label.c_str(),48,registerRow,keyLabel.c_str(),true,nativecontrol::gamepadToggle};
            const auto stage=nativeui::addTestRowToPanel(ui,panel,brake,options);
#else
            const auto stage=nativeui::addTestRowToPanel(ui,panel,brake);
#endif
            insideUi=false;
            Event test{}; test.kind=3; test.thread=GetCurrentThreadId();
            test.type=static_cast<unsigned>(stage); test.success=stage==nativeui::Stage::Created;
            std::strcpy(test.symbol,"TEST-result"); publish(test);
        }
    }
    return success;
}

bool hashExe(char (&hex)[65]) {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    unsigned char buffer[65536], digest[32]; DWORD bytes = 0;
    while (okay) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) { okay = false; break; }
        if (!bytes) break;
        okay = BCryptHashData(hash, buffer, bytes, 0) >= 0;
    }
    if (okay) okay = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (okay) for (unsigned i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return okay;
}
// Copy the loaded main module from its virtual layout. On a SteamStub-packed
// build the on-disk .text is encrypted; the running process decrypts it in
// memory, so signature resolution must snapshot this image rather than the file.
std::vector<uint8_t> snapshotLoadedImage(unsigned char* base) {
    uint32_t pe = 0;
    if (!read(base + 0x3c, &pe, 4)) throw std::runtime_error("cannot read in-memory PE header");
    uint32_t sizeImage = 0;
    if (!read(base + pe + 24 + 56, &sizeImage, 4) || !sizeImage || sizeImage > 0x80000000u)
        throw std::runtime_error("invalid in-memory SizeOfImage");
    std::vector<uint8_t> snapshot(sizeImage);
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base, snapshot.data(), sizeImage, &copied)) snapshot.resize(copied);
    if (copied < 0x1000) throw std::runtime_error("loaded image snapshot too small");
    return snapshot;
}
bool compatible(const unsigned char* image) {
    unsigned char actualCreate[sizeof(createPrefix)], actualAttach[sizeof(attachPrefix)];
    return read(image + createRva, actualCreate, sizeof(actualCreate)) &&
        read(image + attachRva, actualAttach, sizeof(actualAttach)) &&
        !std::memcmp(actualCreate, createPrefix, sizeof(createPrefix)) &&
        !std::memcmp(actualAttach, attachPrefix, sizeof(attachPrefix));
}
bool bindUi(unsigned char* image) {
    struct Entry { uintptr_t rva; unsigned char prefix[4]; };
    const Entry entries[] = {
        {0x1b5d4b0,{0x48,0x89,0x5c,0x24}}, {0x1b5d870,{0x48,0x89,0x5c,0x24}},
        {0x1b5dcb0,{0x4c,0x89,0x44,0x24}}, {0x1b5d0f0,{0x40,0x53,0x48,0x83}},
        {0x1b61560,{0x4c,0x8b,0xdc,0x55}}, {0x1b5d070,{0x48,0x89,0x5c,0x24}}
#ifdef SCALEFORM_NATIVE_AUTOPILOT
        ,{advanceRva,{0x44,0x88,0x4c,0x24}}, {destroyRva,{0x48,0x89,0x5c,0x24}}
#endif
    };
    for (const auto& entry:entries) {
        unsigned char bytes[4];
        if (!read(image+entry.rva,bytes,sizeof(bytes)) || std::memcmp(bytes,entry.prefix,sizeof(bytes))) return false;
    }
    ui = {reinterpret_cast<nativeui::Get>(image+0x1b5d4b0),reinterpret_cast<nativeui::Set>(image+0x1b5d870),
        reinterpret_cast<nativeui::Invoke>(image+0x1b5dcb0),reinterpret_cast<nativeui::Release>(image+0x1b5d0f0),
        reinterpret_cast<nativeui::Attach>(image+attachRva),reinterpret_cast<nativeui::Empty>(image+0x1b61560),traceUi,
        reinterpret_cast<nativeui::Retain>(image+0x1b5d070)};
    return true;
}
#ifdef SR_CC_SIGNATURE_RESOLVER
bool bindResolvedUi(unsigned char* image,const srcompat::Resolution& resolved) {
    if(!resolved.candidate())return false;
    for(const auto& symbol:srcompat::symbols) {
        const auto pattern=srcompat::pattern(symbol.pattern);
        std::vector<unsigned char> actual(pattern.size());
        if(!read(image+resolved.addresses.at(symbol.name),actual.data(),actual.size()))return false;
        for(size_t i=0;i<pattern.size();++i)if(pattern[i]>=0&&pattern[i]!=actual[i])return false;
    }
    uintptr_t destructor=0,advance=0;
    if(!read(image+resolved.vtableRva,&destructor,8)||!read(image+resolved.vtableRva+24*8,&advance,8)||
       destructor!=reinterpret_cast<uintptr_t>(image+resolved.addresses.at("destroy"))||advance!=reinterpret_cast<uintptr_t>(image+resolved.addresses.at("advance")))return false;
    ui={reinterpret_cast<nativeui::Get>(image+resolved.addresses.at("get")),reinterpret_cast<nativeui::Set>(image+resolved.addresses.at("set")),
        reinterpret_cast<nativeui::Invoke>(image+resolved.addresses.at("invoke")),reinterpret_cast<nativeui::Release>(image+resolved.addresses.at("release")),
        reinterpret_cast<nativeui::Attach>(image+resolved.addresses.at("attach")),reinterpret_cast<nativeui::Empty>(image+resolved.addresses.at("empty")),traceUi,
        reinterpret_cast<nativeui::Retain>(image+resolved.addresses.at("retain"))};
    return true;
}
#endif
HMODULE module = nullptr;

// ---- Fault-recording VEH (diagnostic; never swallows the exception) ----
// Mirrors CargoStrap's exception logging: record fault code/address and the
// owning module/RVA to our own log, then pass the exception on to the game
// and any other handlers. It does not continue execution, so it cannot mask
// a real crash.
static FILE* g_faultLog=nullptr;
static HMODULE g_asiModule=nullptr;
static uintptr_t g_exeBase=0,g_exeEnd=0,g_asiBase=0,g_asiEnd=0;
static volatile LONG g_faultReentry=0;

static bool isSevere(DWORD code) {
    return code==0xC0000005||code==0xC0000094||code==0xC0000096||code==0xC000001D||
           code==0xC00000FD||code==0xC0000409||code==0xC0000025||code==0xC0000091;
}

static void describeAddress(uintptr_t addr, char* out, size_t n) {
    if(g_exeBase && addr>=g_exeBase && addr<g_exeEnd) {
        std::snprintf(out,n,"exe+0x%llx",(unsigned long long)(addr-g_exeBase)); return;
    }
    if(g_asiBase && addr>=g_asiBase && addr<g_asiEnd) {
        std::snprintf(out,n,"asi+0x%llx",(unsigned long long)(addr-g_asiBase)); return;
    }
    HMODULE hmod=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr),&hmod)&&hmod) {
        if(hmod==g_asiModule) {
            std::snprintf(out,n,"asi+0x%llx",(unsigned long long)(addr-reinterpret_cast<uintptr_t>(g_asiModule))); return;
        }
        wchar_t name[MAX_PATH]{};
        if(GetModuleFileNameW(hmod,name,MAX_PATH)) {
            const wchar_t* slash=wcsrchr(name,L'\\'); const wchar_t* base=slash?slash+1:name;
            char ansi[128]{}; WideCharToMultiByte(CP_UTF8,0,base,-1,ansi,sizeof(ansi),nullptr,nullptr);
            std::snprintf(out,n,"%s+0x%llx",ansi,(unsigned long long)(addr-reinterpret_cast<uintptr_t>(hmod))); return;
        }
    }
    std::snprintf(out,n,"0x%llx",(unsigned long long)addr);
}

static bool looksLikeCode(uintptr_t addr) {
    if(g_exeBase&&addr>=g_exeBase&&addr<g_exeEnd) return true;
    if(g_asiBase&&addr>=g_asiBase&&addr<g_asiEnd) return true;
    HMODULE hmod=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(addr),&hmod)&&hmod!=nullptr;
}

static void dumpFaultStack(CONTEXT* c) {
    if(!c) return;
    std::fprintf(g_faultLog,"    regs: rip=%llx rsp=%llx rbp=%llx rax=%llx rcx=%llx rdx=%llx rbx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx\n",
        (unsigned long long)c->Rip,(unsigned long long)c->Rsp,(unsigned long long)c->Rbp,
        (unsigned long long)c->Rax,(unsigned long long)c->Rcx,(unsigned long long)c->Rdx,
        (unsigned long long)c->Rbx,(unsigned long long)c->Rsi,(unsigned long long)c->Rdi,
        (unsigned long long)c->R8,(unsigned long long)c->R9);
    uintptr_t slots[64]; SIZE_T got=0;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c->Rsp),slots,sizeof(slots),&got)) {
        std::fprintf(g_faultLog,"    stack read failed rsp=%llx\n",(unsigned long long)c->Rsp); return;
    }
    const int count=static_cast<int>(got/sizeof(uintptr_t));
    for(int i=0;i<count;++i) {
        const uintptr_t v=slots[i];
        char where[160]{}; describeAddress(v,where,sizeof(where));
        // Always print the top 16 slots (caller area); deeper slots only if they
        // resolve to a loaded module/code range, so heap noise does not flood the log.
        if(i<16||looksLikeCode(v))
            std::fprintf(g_faultLog,"    +0x%03x: 0x%llx %s\n",i*8,(unsigned long long)v,where);
    }
    // Standard x64 frame chain: [rbp]=saved rbp, [rbp+8]=return address.
    uintptr_t frame=c->Rbp;
    for(int f=0;f<8&&frame;++f) {
        uintptr_t saved=0, ret=0;
        SIZE_T a=0,b=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(frame),&saved,sizeof(saved),&a)) break;
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(frame+8),&ret,sizeof(ret),&b);
        char where[160]{}; describeAddress(ret,where,sizeof(where));
        std::fprintf(g_faultLog,"    rbp#%d: rbp=%llx ret=0x%llx %s\n",f,(unsigned long long)frame,
            (unsigned long long)ret,where);
        frame=saved;
    }
}

static LONG WINAPI faultRecorder(EXCEPTION_POINTERS* pointers) {
    if(!g_faultLog) return EXCEPTION_CONTINUE_SEARCH;
    if(!pointers||!pointers->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const auto& rec=*pointers->ExceptionRecord;
    if(!isSevere(rec.ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&g_faultReentry,1,0)!=0) return EXCEPTION_CONTINUE_SEARCH;
    char where[192]{};
    describeAddress(reinterpret_cast<uintptr_t>(rec.ExceptionAddress),where,sizeof(where));
    uintptr_t target=0;
    if(rec.ExceptionCode==0xC0000005&&rec.NumberParameters>=2)
        target=reinterpret_cast<uintptr_t>(rec.ExceptionInformation[1]);
    std::fprintf(g_faultLog,"VEH code=0x%08x rip=0x%llx %s target=0x%llx thread=%lu\n",
        static_cast<unsigned>(rec.ExceptionCode),
        (unsigned long long)reinterpret_cast<uintptr_t>(rec.ExceptionAddress),where,
        (unsigned long long)target,GetCurrentThreadId());
    dumpFaultStack(pointers->ContextRecord);
    std::fflush(g_faultLog);
    InterlockedExchange(&g_faultReentry,0);
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI run(void*) {
    wchar_t path[32768]{}; GetModuleFileNameW(module, path, 32768);
    std::wstring logPath(path); logPath.resize(logPath.find_last_of(L"\\/") + 1);
#ifdef SCALEFORM_NATIVE_AUTOPILOT
#ifdef SR_CC_ALLOW_UNREVIEWED
    const std::wstring configPath=logPath+L"SnowRunnerCCExperimental.ini";
    const int experimentStage=GetPrivateProfileIntW(L"Compatibility",L"Stage",0,configPath.c_str());
#else
    const std::wstring configPath=logPath+L"SnowRunnerNativeAutopilot.ini";
#endif
    enableTestRow=GetPrivateProfileIntW(L"Autopilot",L"Enabled",1,configPath.c_str())!=0;
    nativecontrol::toggleKey=GetPrivateProfileIntW(L"Autopilot",L"ToggleKey",VK_F6,configPath.c_str());
    if(nativecontrol::toggleKey<VK_F2||nativecontrol::toggleKey>VK_F12||nativecontrol::toggleKey==VK_F4)nativecontrol::toggleKey=VK_F6;
    nativecontrol::gamepadToggle=GetPrivateProfileIntW(L"Autopilot",L"GamepadToggle",1,configPath.c_str())!=0;
    // InputHook=0 disables the optional input subsystem: no background poll
    // thread, no low-level keyboard hook, no window WndProc subclass, no key
    // injection and no gamepad polling. HUD observer hooks stay installed.
    const bool inputHookOptedIn=GetPrivateProfileIntW(L"Autopilot",L"InputHook",1,configPath.c_str())!=0;
    // Hooks=0 disables every MinHook observer patch (diagnostic: leaves only the
    // fault-recording VEH and the log thread; no game function prologue is rewritten).
    const bool hooksOptedIn=GetPrivateProfileIntW(L"Autopilot",L"Hooks",1,configPath.c_str())!=0;
    // Per-hook diagnostic toggles (default follow Hooks). Used to binary-search
    // which observer patch, if any, conflicts with the external time-control module.
    const bool hookCreate=GetPrivateProfileIntW(L"Autopilot",L"HookCreate",hooksOptedIn?1:0,configPath.c_str())!=0;
    const bool hookAttach=GetPrivateProfileIntW(L"Autopilot",L"HookAttach",hooksOptedIn?1:0,configPath.c_str())!=0;
    const bool hookGet=GetPrivateProfileIntW(L"Autopilot",L"HookGet",hooksOptedIn?1:0,configPath.c_str())!=0;
    const bool hookAdvance=GetPrivateProfileIntW(L"Autopilot",L"HookAdvance",hooksOptedIn?1:0,configPath.c_str())!=0;
    const bool hookDestroy=GetPrivateProfileIntW(L"Autopilot",L"HookDestroy",hooksOptedIn?1:0,configPath.c_str())!=0;
    getPassthrough=GetPrivateProfileIntW(L"Autopilot",L"HookGetPassthrough",0,configPath.c_str())!=0;
#ifdef SR_CC_ALLOW_UNREVIEWED
    const auto stage=srcompat::stagePolicy(experimentStage,enableTestRow);
    enableTestRow=stage.hud;
    logPath+=L"SnowRunnerCCExperimental.log";
#else
    logPath+=L"SnowRunnerNativeAutopilot.log";
#endif
#elif defined(SR_CC_SIGNATURE_DIAGNOSTIC)
    enableTestRow=false;
    logPath+=L"SnowRunnerCCCompatibility.log";
#else
    const std::wstring configPath = logPath + L"SnowRunnerScaleformProbe.ini";
    enableTestRow = GetPrivateProfileIntW(L"Probe",L"EnableTestRow",0,configPath.c_str()) == 1;
    logPath += L"SnowRunnerScaleformProbe.log";
#endif
    FILE* log = _wfopen(logPath.c_str(), L"w"); if (!log) return 1;
    // Install the non-swallowing fault recorder before any hook is installed.
    g_faultLog=log; g_asiModule=module;
    g_exeBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_asiBase=reinterpret_cast<uintptr_t>(module);
    {
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS nt{};
        if(read(reinterpret_cast<void*>(g_exeBase),&dos,sizeof(dos))&&
           read(reinterpret_cast<void*>(g_exeBase+dos.e_lfanew),&nt,sizeof(nt)))
            g_exeEnd=g_exeBase+nt.OptionalHeader.SizeOfImage;
        if(read(reinterpret_cast<void*>(g_asiBase),&dos,sizeof(dos))&&
           read(reinterpret_cast<void*>(g_asiBase+dos.e_lfanew),&nt,sizeof(nt)))
            g_asiEnd=g_asiBase+nt.OptionalHeader.SizeOfImage;
    }
    AddVectoredExceptionHandler(1,faultRecorder);
#ifdef SR_CC_SIGNATURE_DIAGNOSTIC
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    try{srcompat::scanFile(exe,log);}catch(const std::exception& error){std::fprintf(log,"Result=ERROR\nReason=%s\nNo hooks or game changes.\n",error.what());}
    std::fclose(log);return 0;
#else
    bool framesEnabled=true,inputEnabled=true;
#ifdef SR_CC_ALLOW_UNREVIEWED
    checkpointLog=log;
    std::fprintf(log,"EXPERIMENTAL hash allowlist DISABLED; Stage=%d (0=observe,1=HUD,2=control)\n",experimentStage);std::fflush(log);
    if(!stage.valid){std::fprintf(log,"Invalid Stage. No hooks installed.\n");checkpointLog=nullptr;std::fclose(log);return 1;}
    framesEnabled=stage.frames;inputEnabled=stage.input;
#endif
#ifdef SCALEFORM_NATIVE_AUTOPILOT
    inputEnabled=inputHookOptedIn;
#endif
#ifdef SCALEFORM_NATIVE_AUTOPILOT
    std::fprintf(log,"SnowRunner Cruise control (SnowRunner CC) HUD " SNOWRUNNER_CC_VERSION "; enabled=%u; F-key=%u; inputHook=%u; native vector cruise icon; no overlay.\n",enableTestRow,nativecontrol::toggleKey,inputEnabled);
#else
    std::fprintf(log, "Scaleform diagnostic probe 7; TEST=%u (native row attached to library-backed panel); no AS3 Invoke or input writes.\n",enableTestRow);
#endif
    std::fflush(log);
    char hash[65]{};
    const auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
#ifdef SR_CC_SIGNATURE_RESOLVER
    srcompat::Resolution resolved;
    try {
        const bool haveHash=hashExe(hash);
        const std::string reviewed=haveHash?std::string(hash):std::string("?");
        // On a packed (SteamStub) build the on-disk .text is ciphertext and only
        // decrypted in the running process. Snapshot the loaded image and retry
        // until the stub finishes decrypting code, instead of scanning the file.
        srcompat::Image file;bool candidate=false;
        for(unsigned attempt=0;attempt<120&&!candidate;++attempt){
            try{
                auto snapshot=snapshotLoadedImage(image);
                file=srcompat::Image::parseMemory(std::move(snapshot));
                resolved=srcompat::resolve(file);candidate=resolved.candidate();
            }catch(const std::exception& error){
                std::fprintf(log,"Memory scan attempt %u: parse error %s\n",attempt,error.what());
            }
            if(!candidate){std::fprintf(log,"Memory scan attempt %u: candidates=%u; waiting for decrypted image\n",attempt,resolved.candidate());std::fflush(log);Sleep(500);}
        }
        std::fprintf(log,"Signature resolver: candidates=%u hash=%s source=memory-snapshot\n",resolved.candidate(),reviewed.c_str());
        std::fprintf(log,"StartupPhase=scan-report (scanner only; configured hooks follow)\n");
        srcompat::writeReport(log,file,resolved,reviewed);
        if(!srcompat::allowHooks(resolved.candidate())){std::fprintf(log,"Missing/ambiguous signatures. No hooks installed.\n");std::fclose(log);return 1;}
        std::fprintf(log,"StartupPhase=validate-loaded-image reviewed=%u hashAllowlist=OFF\n",srcompat::reviewedBuild(reviewed));
        if(!srcompat::reviewedBuild(reviewed))std::fprintf(log,"WARNING: unreviewed ABI; experimental compatibility run.\n");
        std::fflush(log);
        if(!bindResolvedUi(image,resolved)){std::fprintf(log,"Signature/loaded-image validation failed. No hooks installed.\n");std::fclose(log);return 1;}
    }catch(const std::exception& error){std::fprintf(log,"Resolver error: %s. No hooks installed.\n",error.what());std::fclose(log);return 1;}
#else
    if (!hashExe(hash) || std::strcmp(hash, expectedHash) || !compatible(image) || !bindUi(image)) {
        std::fprintf(log, "Unsupported EXE. SHA256=%s. No hooks installed.\n", hash); std::fclose(log); return 1;
    }
#endif
    std::fprintf(log, "Verified EXE %s\n", hash);
    std::fprintf(log,"StartupPhase=install-hooks frames=%u input=%u hud=%u\n",framesEnabled,inputEnabled,enableTestRow);std::fflush(log);
    const auto init = MH_Initialize();
#ifdef SR_CC_SIGNATURE_RESOLVER
    void* create=image+resolved.addresses.at("create");void* attach=image+resolved.addresses.at("attach");void* get=image+resolved.addresses.at("get");
#else
    void* create = image + createRva; void* attach = image + attachRva; void* get = image + getRva;
#endif
    MH_STATUS a = MH_UNKNOWN, b = MH_UNKNOWN, c = MH_UNKNOWN, enabled = MH_UNKNOWN;
#ifdef SCALEFORM_NATIVE_AUTOPILOT
#ifdef SR_CC_SIGNATURE_RESOLVER
    void* advance=image+resolved.addresses.at("advance");void* destroy=image+resolved.addresses.at("destroy");
#else
    void* advance=image+advanceRva;void* destroy=image+destroyRva;
#endif
    MH_STATUS d=MH_UNKNOWN,e=MH_UNKNOWN;
#endif
    if (!hooksOptedIn) {
        std::fprintf(log, "Hooks=0: MinHook observer patches disabled; no game function prologue rewritten.\n");
    } else if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) {
        bool anyHook=false, complete=true;
        if(hookCreate){ anyHook=true; a=MH_CreateHook(create, reinterpret_cast<void*>(createHook), reinterpret_cast<void**>(&originalCreate)); complete=complete&&a==MH_OK; }
        if(hookAttach){ anyHook=true; b=MH_CreateHook(attach, reinterpret_cast<void*>(attachHook), reinterpret_cast<void**>(&originalAttach)); complete=complete&&b==MH_OK; }
        if(hookGet){ anyHook=true; c=MH_CreateHook(get, reinterpret_cast<void*>(getHook), reinterpret_cast<void**>(&originalGet)); complete=complete&&c==MH_OK; }
#ifdef SCALEFORM_NATIVE_AUTOPILOT
        if(framesEnabled&&hookAdvance){ anyHook=true; d=MH_CreateHook(advance,reinterpret_cast<void*>(advanceHook),reinterpret_cast<void**>(&originalAdvance)); complete=complete&&d==MH_OK; }
        if(framesEnabled&&hookDestroy){ anyHook=true; e=MH_CreateHook(destroy,reinterpret_cast<void*>(destroyHook),reinterpret_cast<void**>(&originalDestroy)); complete=complete&&e==MH_OK; }
#endif
        bool queued=complete&&anyHook;
        if(queued&&hookCreate)queued=queued&&MH_QueueEnableHook(create)==MH_OK;
        if(queued&&hookAttach)queued=queued&&MH_QueueEnableHook(attach)==MH_OK;
        if(queued&&hookGet)queued=queued&&MH_QueueEnableHook(get)==MH_OK;
#ifdef SCALEFORM_NATIVE_AUTOPILOT
        if(queued&&framesEnabled&&hookAdvance)queued=queued&&MH_QueueEnableHook(advance)==MH_OK;
        if(queued&&framesEnabled&&hookDestroy)queued=queued&&MH_QueueEnableHook(destroy)==MH_OK;
#endif
        if(queued)enabled=MH_ApplyQueued();
    }
    std::fprintf(log, "Hooks init=%d create=%d attach=%d get=%d enabled=%d\n", init,a,b,c,enabled); std::fflush(log);
#ifdef SCALEFORM_NATIVE_AUTOPILOT
    std::fprintf(log,"Native frame hooks advance=%d destroy=%d\n",d,e);std::fflush(log);
#endif
    if (enabled != MH_OK) {
        if (a == MH_OK) { MH_DisableHook(create); MH_RemoveHook(create); }
        if (b == MH_OK) { MH_DisableHook(attach); MH_RemoveHook(attach); }
        if (c == MH_OK) { MH_DisableHook(get); MH_RemoveHook(get); }
#ifdef SCALEFORM_NATIVE_AUTOPILOT
        if(d==MH_OK){MH_DisableHook(advance);MH_RemoveHook(advance);}
        if(e==MH_OK){MH_DisableHook(destroy);MH_RemoveHook(destroy);}
#endif
        std::fclose(log); return 1;
    }
#ifdef SCALEFORM_NATIVE_AUTOPILOT
    if(enableTestRow&&inputEnabled) {
        nativecontrol::notify=controlEvent;
        const auto input=CreateThread(nullptr,0,nativecontrol::inputThread,module,0,nullptr);
        if(input){CloseHandle(input);std::fprintf(log,"Native input thread started.\n");}
        else std::fprintf(log,"Native input thread failed; throttle unavailable.\n");
    }
#endif
    unsigned consumed = 0, ticks = 0;
    for (;;) {
        while (consumed < capacity && ready[consumed].load(std::memory_order_acquire)) {
            const auto& e = events[consumed++];
            std::fprintf(log, "%u thread=%lu kind=%u symbol='%s' instance='%s' owner=%llx parent=%llx output=%llx type=%x iface=%llx object=%llx depth=%d ok=%u\n",
                consumed, static_cast<unsigned long>(e.thread),e.kind,e.symbol,e.instance,
                static_cast<unsigned long long>(e.owner),static_cast<unsigned long long>(e.parent),static_cast<unsigned long long>(e.output),
                e.type,static_cast<unsigned long long>(e.interfacePointer),static_cast<unsigned long long>(e.object),e.depth,e.success);
        }
        if (++ticks % 40 == 0) std::fprintf(log, "Heartbeat observed=%u queued=%u written=%u\n", observed.load(),claimed.load(),consumed);
#ifdef SCALEFORM_NATIVE_AUTOPILOT
        if(ticks%40==0)std::fprintf(log,"Native frames=%u HUD=%u state=%d\n",nativeFrames.load(),nativecontrol::hudVisible.load(),nativecontrol::state.load());
#endif
        std::fflush(log); Sleep(250);
    }
#endif // diagnostic-only build excludes the hook installation path
}
}
#ifndef SCALEFORM_PROBE_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        probe::module = instance; DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr,0,probe::run,nullptr,0,nullptr);
        if (thread) CloseHandle(thread);
    }
#ifdef SCALEFORM_NATIVE_AUTOPILOT
    else if(reason==DLL_PROCESS_DETACH)nativecontrol::shutdown();
#endif
    return TRUE;
}
#endif
