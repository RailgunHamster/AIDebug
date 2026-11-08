// Windows x64 stubs adapted from AIDebug Godot 84fd57b. Preserve RCX/RDX/R8/R9 and XMM0-XMM3.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include "thirdparty/minhook/include/MinHook.h"
#pragma comment(lib,"Dbghelp.lib")
struct CallLogRecord { uint64_t timestamp_msec,rcx,rdx,r8,r9,ret; };
struct NativeHookSlot {
    bool in_use=false;
    uint64_t target_addr=0;
    void* trampoline_addr=nullptr;
    uint8_t* stub_page=nullptr;
    void* exit_stub_addr=nullptr;
    volatile LONG64 call_count=0;
    std::mutex log_mutex;
    CallLogRecord logs[64]{};
    int log_head=0,log_count=0;
};
static constexpr int MAX_NATIVE_HOOK_SLOTS=256;
static constexpr int LOG_RING_CAPACITY=64;
static NativeHookSlot g_slots[MAX_NATIVE_HOOK_SLOTS];
static int nextSlot=0;
static std::mutex control;
static thread_local std::string lastError;
struct ThreadCallFrame {uint64_t slot_idx,rcx,rdx,r8,r9,orig_ret_addr;};
static thread_local std::vector<ThreadCallFrame> frames;
static void __fastcall _native_log_on_entry(uint64_t index,uint64_t* frame){
    frames.push_back({index,frame[0],frame[1],frame[2],frame[3],frame[15]});
    frame[15]=(uint64_t)g_slots[index].exit_stub_addr;
}
static uint64_t __fastcall _native_log_on_exit(uint64_t index,uint64_t result){
    if(frames.empty())return 0;
    auto call=frames.back();frames.pop_back();
    auto& slot=g_slots[index];InterlockedIncrement64(&slot.call_count);
    std::lock_guard<std::mutex> guard(slot.log_mutex);
    auto pos=(slot.log_head+slot.log_count)%64;
    if(slot.log_count==64)slot.log_head=(slot.log_head+1)%64;else slot.log_count++;
    slot.logs[pos]={GetTickCount64(),call.rcx,call.rdx,call.r8,call.r9,result};
    return call.orig_ret_addr;
}
static void _emit_bytes(uint8_t *&p, const void *data, size_t len) {
	memcpy(p, data, len);
	p += len;
}

static void _emit_u64(uint8_t *&p, uint64_t val) {
	memcpy(p, &val, 8);
	p += 8;
}

static void _build_return_const_stub(NativeHookSlot &slot, uint64_t p_const_val) {
	uint8_t *p = slot.stub_page;
	// mov rax, &slot.call_count (48 B8 <imm64>)
	const uint8_t mov_rax[] = { 0x48, 0xB8 };
	_emit_bytes(p, mov_rax, sizeof(mov_rax));
	_emit_u64(p, (uint64_t)&slot.call_count);
	// lock inc qword ptr [rax] (F0 48 FF 00)
	const uint8_t lock_inc[] = { 0xF0, 0x48, 0xFF, 0x00 };
	_emit_bytes(p, lock_inc, sizeof(lock_inc));
	// mov rax, p_const_val (48 B8 <imm64>)
	_emit_bytes(p, mov_rax, sizeof(mov_rax));
	_emit_u64(p, p_const_val);
	// ret (C3)
	*p++ = 0xC3;
	FlushInstructionCache(GetCurrentProcess(), slot.stub_page, 256);
}

static void _build_dll_stub(NativeHookSlot &slot, uint64_t p_dll_fn_addr) {
	uint8_t *p = slot.stub_page;
	// Store call_count at stub_page + 0x80? No, use RIP-relative lock inc on slot.call_count if near, or push rax / mov rax / lock inc / pop rax!
	// Wait: `push rax; mov rax, &call_count; lock inc qword ptr [rax]; pop rax; jmp [rip+0]` touches ZERO registers and preserves RAX!
	// 50                            push rax
	// 48 B8 <8-byte &call_count>    mov rax, &slot.call_count
	// F0 48 FF 00                   lock inc qword ptr [rax]
	// 58                            pop rax
	// FF 25 00 00 00 00             jmp qword ptr [rip + 0]
	// <8-byte p_dll_fn_addr>
	*p++ = 0x50; // push rax
	const uint8_t mov_rax[] = { 0x48, 0xB8 };
	_emit_bytes(p, mov_rax, sizeof(mov_rax));
	_emit_u64(p, (uint64_t)&slot.call_count);
	const uint8_t lock_inc[] = { 0xF0, 0x48, 0xFF, 0x00 };
	_emit_bytes(p, lock_inc, sizeof(lock_inc));
	*p++ = 0x58; // pop rax
	const uint8_t jmp_rip[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	_emit_bytes(p, jmp_rip, sizeof(jmp_rip));
	_emit_u64(p, p_dll_fn_addr);
	FlushInstructionCache(GetCurrentProcess(), slot.stub_page, 256);
}

static void _build_log_stubs(NativeHookSlot &slot, uint64_t p_slot_idx) {
	// Entry stub at stub_page + 0x00
	uint8_t *p = slot.stub_page;
	const uint8_t prolog[] = {
		0x48,
		0x81,
		0xEC,
		0x98,
		0x00,
		0x00,
		0x00, // sub rsp, 0x98
		0x48,
		0x89,
		0x4C,
		0x24,
		0x20, // mov [rsp+0x20], rcx
		0x48,
		0x89,
		0x54,
		0x24,
		0x28, // mov [rsp+0x28], rdx
		0x4C,
		0x89,
		0x44,
		0x24,
		0x30, // mov [rsp+0x30], r8
		0x4C,
		0x89,
		0x4C,
		0x24,
		0x38, // mov [rsp+0x38], r9
		0x48,
		0x89,
		0x44,
		0x24,
		0x40, // mov [rsp+0x40], rax
		0x4C,
		0x89,
		0x54,
		0x24,
		0x48, // mov [rsp+0x48], r10
		0x4C,
		0x89,
		0x5C,
		0x24,
		0x50, // mov [rsp+0x50], r11
		0x0F,
		0x11,
		0x44,
		0x24,
		0x58, // movups [rsp+0x58], xmm0
		0x0F,
		0x11,
		0x4C,
		0x24,
		0x68, // movups [rsp+0x68], xmm1
		0x0F,
		0x11,
		0x54,
		0x24,
		0x78, // movups [rsp+0x78], xmm2
		0x0F,
		0x11,
		0x9C,
		0x24,
		0x88,
		0x00,
		0x00,
		0x00, // movups [rsp+0x88], xmm3
	};
	_emit_bytes(p, prolog, sizeof(prolog));

	// mov rcx, p_slot_idx (48 B9 <imm64>)
	const uint8_t mov_rcx[] = { 0x48, 0xB9 };
	_emit_bytes(p, mov_rcx, sizeof(mov_rcx));
	_emit_u64(p, p_slot_idx);

	// lea rdx, [rsp + 0x20] (48 8D 54 24 20)
	const uint8_t lea_rdx[] = { 0x48, 0x8D, 0x54, 0x24, 0x20 };
	_emit_bytes(p, lea_rdx, sizeof(lea_rdx));

	// mov rax, &_native_log_on_entry (48 B8 <imm64>)
	const uint8_t mov_rax[] = { 0x48, 0xB8 };
	_emit_bytes(p, mov_rax, sizeof(mov_rax));
	_emit_u64(p, (uint64_t)&_native_log_on_entry);

	// call rax (FF D0)
	const uint8_t call_rax[] = { 0xFF, 0xD0 };
	_emit_bytes(p, call_rax, sizeof(call_rax));

	const uint8_t epilog[] = {
		0x0F,
		0x10,
		0x9C,
		0x24,
		0x88,
		0x00,
		0x00,
		0x00, // movups xmm3, [rsp+0x88]
		0x0F,
		0x10,
		0x54,
		0x24,
		0x78, // movups xmm2, [rsp+0x78]
		0x0F,
		0x10,
		0x4C,
		0x24,
		0x68, // movups xmm1, [rsp+0x68]
		0x0F,
		0x10,
		0x44,
		0x24,
		0x58, // movups xmm0, [rsp+0x58]
		0x4C,
		0x8B,
		0x5C,
		0x24,
		0x50, // mov r11, [rsp+0x50]
		0x4C,
		0x8B,
		0x54,
		0x24,
		0x48, // mov r10, [rsp+0x48]
		0x48,
		0x8B,
		0x44,
		0x24,
		0x40, // mov rax, [rsp+0x40]
		0x4C,
		0x8B,
		0x4C,
		0x24,
		0x38, // mov r9,  [rsp+0x38]
		0x4C,
		0x8B,
		0x44,
		0x24,
		0x30, // mov r8,  [rsp+0x30]
		0x48,
		0x8B,
		0x54,
		0x24,
		0x28, // mov rdx, [rsp+0x28]
		0x48,
		0x8B,
		0x4C,
		0x24,
		0x20, // mov rcx, [rsp+0x20]
		0x48,
		0x81,
		0xC4,
		0x98,
		0x00,
		0x00,
		0x00, // add rsp, 0x98
		0xFF,
		0x25,
		0x00,
		0x00,
		0x00,
		0x00, // jmp qword ptr [rip + 0]
	};
	_emit_bytes(p, epilog, sizeof(epilog));
	_emit_u64(p, (uint64_t)slot.trampoline_addr);

	// Exit stub at stub_page + 0x100
	uint8_t *ex = slot.stub_page + 0x100;
	slot.exit_stub_addr = ex;
	const uint8_t ex_prolog[] = {
		0x48,
		0x83,
		0xEC,
		0x40, // sub rsp, 0x40
		0x48,
		0x89,
		0x44,
		0x24,
		0x20, // mov [rsp+0x20], rax
		0x0F,
		0x11,
		0x44,
		0x24,
		0x28, // movups [rsp+0x28], xmm0
	};
	_emit_bytes(ex, ex_prolog, sizeof(ex_prolog));

	// mov rcx, p_slot_idx
	_emit_bytes(ex, mov_rcx, sizeof(mov_rcx));
	_emit_u64(ex, p_slot_idx);

	// mov rdx, rax (48 89 C2)
	const uint8_t mov_rdx_rax[] = { 0x48, 0x89, 0xC2 };
	_emit_bytes(ex, mov_rdx_rax, sizeof(mov_rdx_rax));

	// mov rax, &_native_log_on_exit
	_emit_bytes(ex, mov_rax, sizeof(mov_rax));
	_emit_u64(ex, (uint64_t)&_native_log_on_exit);

	// call rax
	_emit_bytes(ex, call_rax, sizeof(call_rax));

	const uint8_t ex_epilog[] = {
		0x49,
		0x89,
		0xC3, // mov r11, rax
		0x0F,
		0x10,
		0x44,
		0x24,
		0x28, // movups xmm0, [rsp+0x28]
		0x48,
		0x8B,
		0x44,
		0x24,
		0x20, // mov rax, [rsp+0x20]
		0x48,
		0x83,
		0xC4,
		0x40, // add rsp, 0x40
		0x41,
		0xFF,
		0xE3, // jmp r11
	};
	_emit_bytes(ex, ex_epilog, sizeof(ex_epilog));

	FlushInstructionCache(GetCurrentProcess(), slot.stub_page, 512);
}

extern "C" __declspec(dllexport) const char* aidbg_error(){return lastError.c_str();}
static int fail(const char* error){lastError=error;return -1;}
extern "C" __declspec(dllexport) uint64_t aidbg_resolve(const char* name){
    std::lock_guard<std::mutex> guard(control);
    std::string symbol=name?name:"";
    if(symbol.empty()){fail("Symbol is empty");return 0;}
    if(symbol.rfind("0x",0)==0){try{return std::stoull(symbol,nullptr,16);}catch(...){fail("Invalid address");return 0;}}
    auto bang=symbol.find('!');
    if(bang!=std::string::npos){auto module=GetModuleHandleA(symbol.substr(0,bang).c_str());if(module){auto address=GetProcAddress(module,symbol.substr(bang+1).c_str());if(address)return (uint64_t)address;}}
    static HANDLE symbols=nullptr;
    if(!symbols){DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&symbols,0,FALSE,DUPLICATE_SAME_ACCESS);SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);if(!SymInitialize(symbols,nullptr,TRUE)){fail("PDB symbol initialization failed");return 0;}}
    SymRefreshModuleList(symbols);
    char buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};auto info=(SYMBOL_INFO*)buffer;info->SizeOfStruct=sizeof(SYMBOL_INFO);info->MaxNameLen=MAX_SYM_NAME;
    if(SymFromName(symbols,name,info))return info->Address;
    fail("Symbol not found; use module.dll!Export, a matching PDB name, or a hexadecimal address");return 0;
}
extern "C" __declspec(dllexport) int aidbg_hook(uint64_t address,int mode,int64_t constant,uint64_t replacement){
    std::lock_guard<std::mutex> guard(control);
    if(mode<0||mode>2||!address||(mode==2&&!replacement))return fail("Invalid native hook arguments");
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery((void*)address,&region,sizeof(region))||region.State!=MEM_COMMIT||(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))==0)return fail("Target is not executable memory");
    for(int i=0;i<nextSlot;i++)if(g_slots[i].in_use&&g_slots[i].target_addr==address)return fail("Target is already hooked");
    if(nextSlot==MAX_NATIVE_HOOK_SLOTS)return fail("Native slot budget exhausted; restart this process");
    auto status=MH_Initialize();if(status!=MH_OK&&status!=MH_ERROR_ALREADY_INITIALIZED)return fail(MH_StatusToString(status));
    int id=nextSlot++;auto& slot=g_slots[id];slot.target_addr=address;
    slot.stub_page=(uint8_t*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if(!slot.stub_page)return fail("Stub allocation failed");
    status=MH_CreateHook((void*)address,slot.stub_page,&slot.trampoline_addr);
    if(status!=MH_OK)return fail(MH_StatusToString(status));
    if(mode==0)_build_log_stubs(slot,id);else if(mode==1)_build_return_const_stub(slot,(uint64_t)constant);else _build_dll_stub(slot,replacement);
    status=MH_EnableHook((void*)address);
    if(status!=MH_OK){MH_RemoveHook((void*)address);return fail(MH_StatusToString(status));}
    slot.in_use=true;return id+1;
}
extern "C" __declspec(dllexport) int aidbg_unhook(int id){
    std::lock_guard<std::mutex> guard(control);
    if(id<1||id>nextSlot||!g_slots[id-1].in_use)return fail("Native hook ID is not active");
    auto& slot=g_slots[id-1];auto status=MH_RemoveHook((void*)slot.target_addr);
    if(status!=MH_OK)return fail(MH_StatusToString(status));
    slot.in_use=false;return 0;
}
extern "C" __declspec(dllexport) int64_t aidbg_count(int id){if(id<1||id>nextSlot)return 0;return InterlockedCompareExchange64(&g_slots[id-1].call_count,0,0);}
extern "C" __declspec(dllexport) int aidbg_logs(int id,CallLogRecord* output,int capacity){
    if(id<1||id>nextSlot||capacity<0)return 0;
    auto& slot=g_slots[id-1];std::lock_guard<std::mutex> guard(slot.log_mutex);
    int count=slot.log_count<capacity?slot.log_count:capacity;
    for(int i=0;i<count;i++)output[i]=slot.logs[(slot.log_head+i)%64];return count;
}
extern "C" __declspec(dllexport) int aidbg_dump(const wchar_t* path,int full){
    std::lock_guard<std::mutex> guard(control);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return fail("Cannot create dump; destination must not already exist");
    BOOL ok=MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,full?MiniDumpWithFullMemory:MiniDumpNormal,nullptr,nullptr,nullptr);
    CloseHandle(file);if(!ok)return fail("MiniDumpWriteDump failed");return 0;
}