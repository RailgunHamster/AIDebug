/**************************************************************************/
/*  ai_debug_native.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_debug_native.h"

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"

#if defined(WINDOWS_ENABLED) && defined(_WIN64)
#define WIN32_LEAN_AND_MEAN
#include "thirdparty/minhook/MinHook.h"

#include <windows.h>

#include <dbghelp.h>

#include <cstring>

namespace {

// DbgHelp function pointers loaded dynamically so we don't add link-time dependencies
typedef DWORD(WINAPI *SymSetOptionsFn)(DWORD SymOptions);
typedef BOOL(WINAPI *SymInitializeWFn)(HANDLE hProcess, PCWSTR UserSearchPath, BOOL fInvadeProcess);
typedef BOOL(WINAPI *SymRefreshModuleListFn)(HANDLE hProcess);
typedef BOOL(WINAPI *SymFromNameWFn)(HANDLE hProcess, PCWSTR Name, PSYMBOL_INFOW Symbol);
typedef BOOL(CALLBACK *PSYM_ENUMERATESYMBOLS_CALLBACKW_FN)(PSYMBOL_INFOW pSymInfo, ULONG SymbolSize, PVOID UserContext);
typedef BOOL(WINAPI *SymEnumSymbolsWFn)(HANDLE hProcess, ULONG64 BaseOfDll, PCWSTR Mask, PSYM_ENUMERATESYMBOLS_CALLBACKW_FN EnumSymbolsCallback, PVOID UserContext);

static HMODULE g_dbghelp_mod = nullptr;
static SymSetOptionsFn g_SymSetOptions = nullptr;
static SymInitializeWFn g_SymInitializeW = nullptr;
static SymRefreshModuleListFn g_SymRefreshModuleList = nullptr;
static SymFromNameWFn g_SymFromNameW = nullptr;
static SymEnumSymbolsWFn g_SymEnumSymbolsW = nullptr;
static bool g_sym_initialized = false;
static bool g_mh_initialized = false;

static bool _ensure_dbghelp_initialized(String &r_error) {
	if (g_sym_initialized) {
		if (g_SymRefreshModuleList) {
			g_SymRefreshModuleList(GetCurrentProcess());
		}
		return true;
	}

	if (!g_dbghelp_mod) {
		g_dbghelp_mod = LoadLibraryW(L"dbghelp.dll");
		if (!g_dbghelp_mod) {
			r_error = "Failed to load dbghelp.dll.";
			return false;
		}
		g_SymSetOptions = (SymSetOptionsFn)GetProcAddress(g_dbghelp_mod, "SymSetOptions");
		g_SymInitializeW = (SymInitializeWFn)GetProcAddress(g_dbghelp_mod, "SymInitializeW");
		g_SymRefreshModuleList = (SymRefreshModuleListFn)GetProcAddress(g_dbghelp_mod, "SymRefreshModuleList");
		g_SymFromNameW = (SymFromNameWFn)GetProcAddress(g_dbghelp_mod, "SymFromNameW");
		g_SymEnumSymbolsW = (SymEnumSymbolsWFn)GetProcAddress(g_dbghelp_mod, "SymEnumSymbolsW");
	}

	if (!g_SymSetOptions || !g_SymInitializeW || !g_SymFromNameW) {
		r_error = "dbghelp.dll is missing required symbol lookup functions.";
		return false;
	}

	g_SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	String commit_hash = String(GODOT_VERSION_HASH);
	String symbols_dir = exe_dir.path_join("..").path_join("..").path_join("symbols").path_join(commit_hash).simplify_path();
	String search_path = exe_dir + ";" + symbols_dir;
	if (!commit_hash.is_empty() && commit_hash.length() >= 9) {
		String short_dir = exe_dir.path_join("..").path_join("..").path_join("symbols").path_join(commit_hash.substr(0, 9)).simplify_path();
		search_path += ";" + short_dir;
	}

	Char16String wsearch = search_path.utf16();
	if (!g_SymInitializeW(GetCurrentProcess(), (PCWSTR)wsearch.get_data(), TRUE)) {
		// If SymInitialize was already called in this process (e.g. by crash handler or CLR), refresh module list
		if (g_SymRefreshModuleList) {
			g_SymRefreshModuleList(GetCurrentProcess());
		}
	}

	g_sym_initialized = true;
	return true;
}

struct EnumSymContext {
	String wanted;
	uint64_t best_addr = 0;
	String best_name;
	uint32_t best_size = 0;
};

static BOOL CALLBACK _enum_sym_callback(PSYMBOL_INFOW pSymInfo, ULONG SymbolSize, PVOID UserContext) {
	EnumSymContext *ctx = static_cast<EnumSymContext *>(UserContext);
	if (!pSymInfo || !pSymInfo->Address) {
		return TRUE;
	}
	String name = String::utf16((const char16_t *)pSymInfo->Name, pSymInfo->NameLen);
	if (name == ctx->wanted || name.ends_with("::" + ctx->wanted)) {
		ctx->best_addr = (uint64_t)pSymInfo->Address;
		ctx->best_name = name;
		ctx->best_size = SymbolSize;
		return FALSE; // Exact match found, stop enumeration
	}
	if (ctx->best_addr == 0 || name.length() < ctx->best_name.length()) {
		ctx->best_addr = (uint64_t)pSymInfo->Address;
		ctx->best_name = name;
		ctx->best_size = SymbolSize;
	}
	return TRUE;
}

static bool _resolve_native_symbol(const String &p_symbol, uint64_t &r_address, String &r_resolved_name, uint32_t &r_sym_size, String &r_error) {
	r_address = 0;
	r_resolved_name = p_symbol;
	r_sym_size = 0;

	String sym = p_symbol.strip_edges();
	if (sym.is_empty()) {
		r_error = "Symbol name cannot be empty.";
		return false;
	}

	// 1. Direct hex address (e.g. "0x7ff612345678")
	if (sym.begins_with("0x") || sym.begins_with("0X")) {
		r_address = (uint64_t)sym.hex_to_int();
		if (r_address != 0) {
			return true;
		}
	}

	// 2. Module!Export syntax (e.g. "kernel32.dll!GetTickCount64")
	if (sym.contains_char('!')) {
		String mod_part = sym.get_slicec('!', 0);
		String fn_part = sym.get_slicec('!', 1);
		HMODULE hMod = GetModuleHandleW((LPCWSTR)mod_part.utf16().get_data());
		if (hMod) {
			FARPROC proc = GetProcAddress(hMod, fn_part.utf8().get_data());
			if (proc) {
				r_address = (uint64_t)proc;
				r_resolved_name = sym;
				return true;
			}
		}
	}

	// 3. DbgHelp symbol lookup (PDB next to exe or in symbols/<commit>)
	if (!_ensure_dbghelp_initialized(r_error)) {
		return false;
	}

	alignas(SYMBOL_INFOW) uint8_t buffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)] = {};
	PSYMBOL_INFOW pSymbol = reinterpret_cast<PSYMBOL_INFOW>(buffer);
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
	pSymbol->MaxNameLen = MAX_SYM_NAME;

	Char16String wsym = sym.utf16();
	if (g_SymFromNameW(GetCurrentProcess(), (PCWSTR)wsym.get_data(), pSymbol) && pSymbol->Address != 0) {
		r_address = (uint64_t)pSymbol->Address;
		r_resolved_name = String::utf16((const char16_t *)pSymbol->Name, pSymbol->NameLen);
		r_sym_size = pSymbol->Size;
		return true;
	}

	// 4. Wildcard enumeration fallback via SymEnumSymbolsW
	if (g_SymEnumSymbolsW) {
		EnumSymContext ctx;
		ctx.wanted = sym;
		String mask = "*" + sym + "*";
		Char16String wmask = mask.utf16();
		g_SymEnumSymbolsW(GetCurrentProcess(), 0, (PCWSTR)wmask.get_data(), &_enum_sym_callback, &ctx);
		if (ctx.best_addr != 0) {
			r_address = ctx.best_addr;
			r_resolved_name = ctx.best_name;
			r_sym_size = ctx.best_size;
			return true;
		}
	}

	// 5. Export table of main executable fallback
	FARPROC main_export = GetProcAddress(GetModuleHandleW(nullptr), sym.utf8().get_data());
	if (main_export) {
		r_address = (uint64_t)main_export;
		r_resolved_name = sym;
		return true;
	}

	r_error = vformat("Symbol '%s' not found via DbgHelp or module exports. Note: if the function was inlined by the compiler, or if the .pdb file is not next to the executable, it cannot be hooked.", sym);
	return false;
}

// ============================================================================
// Native Hook Slots & x64 Machine-Code Thunks
// ============================================================================

constexpr int MAX_NATIVE_HOOK_SLOTS = 64;
constexpr int LOG_RING_CAPACITY = 64;

struct CallLogRecord {
	uint64_t timestamp_msec = 0;
	uint64_t rcx = 0;
	uint64_t rdx = 0;
	uint64_t r8 = 0;
	uint64_t r9 = 0;
	uint64_t ret = 0;
};

struct NativeHookSlot {
	bool in_use = false;
	String id;
	String symbol;
	String resolved_name;
	String mode;
	String applied_at;
	uint64_t target_addr = 0;
	void *trampoline_addr = nullptr;
	uint8_t *stub_page = nullptr;
	void *exit_stub_addr = nullptr;
	HMODULE loaded_dll = nullptr;
	String dll_path;
	String dll_export;
	int64_t return_const = 0;
	volatile int64_t call_count = 0;

	Mutex log_mutex;
	CallLogRecord logs[LOG_RING_CAPACITY];
	int log_head = 0;
	int log_count = 0;
};

static Mutex g_hooks_mutex;
static NativeHookSlot g_slots[MAX_NATIVE_HOOK_SLOTS];
static int g_next_hook_id = 1;

struct ThreadCallFrame {
	uint64_t slot_idx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t r8;
	uint64_t r9;
	uint64_t orig_ret_addr;
};

static thread_local Vector<ThreadCallFrame> t_call_stack;

static void __fastcall _native_log_on_entry(uint64_t p_slot_idx, uint64_t *p_frame) {
	if (p_slot_idx >= MAX_NATIVE_HOOK_SLOTS) {
		return;
	}
	NativeHookSlot &slot = g_slots[p_slot_idx];
	ThreadCallFrame tf;
	tf.slot_idx = p_slot_idx;
	tf.rcx = p_frame[0];
	tf.rdx = p_frame[1];
	tf.r8 = p_frame[2];
	tf.r9 = p_frame[3];
	tf.orig_ret_addr = p_frame[15];
	t_call_stack.push_back(tf);

	// Redirect function return to our exit_stub so we capture RAX on return
	p_frame[15] = (uint64_t)slot.exit_stub_addr;
}

static uint64_t __fastcall _native_log_on_exit(uint64_t p_slot_idx, uint64_t p_ret_rax) {
	uint64_t orig_ret = 0;
	uint64_t rcx = 0, rdx = 0, r8 = 0, r9 = 0;
	if (!t_call_stack.is_empty()) {
		int last = t_call_stack.size() - 1;
		ThreadCallFrame tf = t_call_stack[last];
		t_call_stack.remove_at(last);
		orig_ret = tf.orig_ret_addr;
		rcx = tf.rcx;
		rdx = tf.rdx;
		r8 = tf.r8;
		r9 = tf.r9;
	}

	if (p_slot_idx < MAX_NATIVE_HOOK_SLOTS) {
		NativeHookSlot &slot = g_slots[p_slot_idx];
		InterlockedIncrement64(&slot.call_count);
		if (slot.in_use) {
			MutexLock lock(slot.log_mutex);
			int idx = (slot.log_head + slot.log_count) % LOG_RING_CAPACITY;
			if (slot.log_count == LOG_RING_CAPACITY) {
				slot.log_head = (slot.log_head + 1) % LOG_RING_CAPACITY;
			} else {
				slot.log_count++;
			}
			CallLogRecord &rec = slot.logs[idx];
			rec.timestamp_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
			rec.rcx = rcx;
			rec.rdx = rdx;
			rec.r8 = r8;
			rec.r9 = r9;
			rec.ret = p_ret_rax;
		}
	}

	return orig_ret;
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

static Dictionary _slot_to_dict(NativeHookSlot &slot) {
	Dictionary d;
	d["id"] = slot.id;
	d["symbol"] = slot.symbol;
	d["resolved_name"] = slot.resolved_name;
	d["address"] = vformat("0x%016X", slot.target_addr);
	d["trampoline"] = vformat("0x%016X", (uint64_t)slot.trampoline_addr);
	d["mode"] = slot.mode;
	d["applied_at"] = slot.applied_at;
	d["call_count"] = (int64_t)slot.call_count;
	if (slot.mode == "return_const") {
		d["return_const"] = slot.return_const;
	} else if (slot.mode == "dll") {
		d["dll_path"] = slot.dll_path;
		d["dll_export"] = slot.dll_export;
	} else if (slot.mode == "log") {
		MutexLock lock(slot.log_mutex);
		Array recent;
		for (int i = 0; i < slot.log_count; i++) {
			int idx = (slot.log_head + i) % LOG_RING_CAPACITY;
			const CallLogRecord &r = slot.logs[idx];
			Dictionary item;
			item["timestamp_msec"] = (int64_t)r.timestamp_msec;
			item["rcx"] = (int64_t)r.rcx;
			item["rdx"] = (int64_t)r.rdx;
			item["r8"] = (int64_t)r.r8;
			item["r9"] = (int64_t)r.r9;
			item["rcx_hex"] = vformat("0x%X", r.rcx);
			item["rdx_hex"] = vformat("0x%X", r.rdx);
			item["r8_hex"] = vformat("0x%X", r.r8);
			item["r9_hex"] = vformat("0x%X", r.r9);
			item["return_value"] = (int64_t)r.ret;
			item["return_hex"] = vformat("0x%X", r.ret);
			recent.push_back(item);
		}
		d["recent_calls"] = recent;
	}
	return d;
}

} // namespace
#endif // WINDOWS_ENABLED && _WIN64

Dictionary AIDebugNative::hook_native(const Dictionary &p_args) {
	Dictionary res;
#if !defined(WINDOWS_ENABLED) || !defined(_WIN64)
	res["ok"] = false;
	res["error"] = "hook_native is only supported on Windows x86_64.";
	return res;
#else
	MutexLock lock(g_hooks_mutex);

	if (!g_mh_initialized) {
		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
			res["ok"] = false;
			res["error"] = String("MinHook initialization failed: ") + MH_StatusToString(st);
			return res;
		}
		g_mh_initialized = true;
	}

	String symbol = p_args.get("symbol", "");
	String mode = String(p_args.get("mode", "log")).strip_edges().to_lower();
	String dll_path = p_args.get("dll_path", p_args.get("dll", ""));
	String dll_export = p_args.get("export", p_args.get("function", ""));
	if (!dll_path.is_empty() && mode == "log" && !p_args.has("mode")) {
		mode = "dll";
	}

	if (mode != "log" && mode != "return_const" && mode != "dll") {
		res["ok"] = false;
		res["error"] = "Invalid mode '" + mode + "'. Expected 'log', 'return_const', or 'dll'.";
		return res;
	}

	uint64_t target_addr = 0;
	String resolved_name;
	uint32_t sym_size = 0;
	String err;
	if (!_resolve_native_symbol(symbol, target_addr, resolved_name, sym_size, err)) {
		res["ok"] = false;
		res["error"] = err;
		return res;
	}

	int slot_idx = -1;
	for (int i = 0; i < MAX_NATIVE_HOOK_SLOTS; i++) {
		if (g_slots[i].in_use && g_slots[i].target_addr == target_addr) {
			res["ok"] = false;
			res["error"] = vformat("Symbol '%s' (0x%016X) is already hooked by '%s'. Unhook it first.", resolved_name, target_addr, g_slots[i].id);
			return res;
		}
		if (!g_slots[i].in_use && slot_idx < 0) {
			slot_idx = i;
		}
	}
	if (slot_idx < 0) {
		res["ok"] = false;
		res["error"] = "Maximum number of native hooks reached.";
		return res;
	}

	NativeHookSlot &slot = g_slots[slot_idx];
	if (!slot.stub_page) {
		slot.stub_page = (uint8_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!slot.stub_page) {
			res["ok"] = false;
			res["error"] = "VirtualAlloc failed for native hook stub page.";
			return res;
		}
	}

	HMODULE hLoadedDll = nullptr;
	uint64_t dll_fn_addr = 0;
	String global_dll_path;
	if (mode == "dll") {
		if (dll_path.is_empty()) {
			res["ok"] = false;
			res["error"] = "Parameter 'dll_path' is required when mode='dll'.";
			return res;
		}
		if (dll_path.begins_with("res://") || dll_path.begins_with("user://")) {
			global_dll_path = ProjectSettings::get_singleton()->globalize_path(dll_path);
		} else {
			global_dll_path = dll_path;
		}
		hLoadedDll = LoadLibraryW((LPCWSTR)global_dll_path.utf16().get_data());
		if (!hLoadedDll) {
			res["ok"] = false;
			res["error"] = vformat("LoadLibraryW failed for '%s' (Win32 error %d).", global_dll_path, (int)GetLastError());
			return res;
		}
		if (dll_export.is_empty()) {
			dll_export = "hook_replacement";
		}
		FARPROC fn = GetProcAddress(hLoadedDll, dll_export.utf8().get_data());
		if (!fn) {
			FreeLibrary(hLoadedDll);
			res["ok"] = false;
			res["error"] = vformat("Export '%s' not found in DLL '%s'.", dll_export, global_dll_path);
			return res;
		}
		dll_fn_addr = (uint64_t)fn;
	}

	void *orig_trampoline = nullptr;
	MH_STATUS mh_st = MH_CreateHook((LPVOID)target_addr, (LPVOID)slot.stub_page, &orig_trampoline);
	if (mh_st != MH_OK) {
		if (hLoadedDll) {
			FreeLibrary(hLoadedDll);
		}
		res["ok"] = false;
		res["error"] = vformat("MH_CreateHook failed for '%s' (0x%016X): %s", resolved_name, target_addr, MH_StatusToString(mh_st));
		return res;
	}

	slot.in_use = true;
	slot.id = vformat("nh%d", g_next_hook_id++);
	slot.symbol = symbol;
	slot.resolved_name = resolved_name;
	slot.mode = mode;
	slot.applied_at = Time::get_singleton()->get_datetime_string_from_system(false, true);
	slot.target_addr = target_addr;
	slot.trampoline_addr = orig_trampoline;
	slot.loaded_dll = hLoadedDll;
	slot.dll_path = global_dll_path;
	slot.dll_export = dll_export;
	slot.return_const = p_args.get("return_const", p_args.get("value", 0));
	slot.call_count = 0;
	slot.log_head = 0;
	slot.log_count = 0;

	if (mode == "return_const") {
		_build_return_const_stub(slot, (uint64_t)slot.return_const);
	} else if (mode == "dll") {
		// If the replacement DLL exports `ai_debug_orig_fn` or `<export>_orig`, populate it with the trampoline pointer
		FARPROC orig_holder = GetProcAddress(hLoadedDll, "ai_debug_orig_fn");
		if (!orig_holder) {
			String alt = dll_export + "_orig";
			orig_holder = GetProcAddress(hLoadedDll, alt.utf8().get_data());
		}
		if (orig_holder) {
			*(void **)orig_holder = orig_trampoline;
		}
		_build_dll_stub(slot, dll_fn_addr);
	} else {
		_build_log_stubs(slot, (uint64_t)slot_idx);
	}

	mh_st = MH_EnableHook((LPVOID)target_addr);
	if (mh_st != MH_OK) {
		MH_RemoveHook((LPVOID)target_addr);
		if (hLoadedDll) {
			FreeLibrary(hLoadedDll);
		}
		slot.in_use = false;
		res["ok"] = false;
		res["error"] = vformat("MH_EnableHook failed for '%s': %s", resolved_name, MH_StatusToString(mh_st));
		return res;
	}

	res = _slot_to_dict(slot);
	res["ok"] = true;
	res["note"] = String::utf8("如果目标 C++ 函数被编译器内联（inline），已被内联的调用点不会经过此符号入口。");
	return res;
#endif
}

Dictionary AIDebugNative::unhook_native(const String &p_id) {
	Dictionary res;
#if !defined(WINDOWS_ENABLED) || !defined(_WIN64)
	res["ok"] = false;
	res["error"] = "unhook_native is only supported on Windows x86_64.";
	return res;
#else
	MutexLock lock(g_hooks_mutex);
	String target_id = p_id.strip_edges();
	Array removed;
	Array errors;

	if (target_id.is_empty() || target_id == "all" || target_id == "*") {
		for (int i = 0; i < MAX_NATIVE_HOOK_SLOTS; i++) {
			if (g_slots[i].in_use) {
				MH_STATUS status = MH_RemoveHook((LPVOID)g_slots[i].target_addr);
				if (status != MH_OK) {
					errors.push_back(g_slots[i].id + ": " + MH_StatusToString(status));
					continue;
				}
				if (g_slots[i].loaded_dll) {
					FreeLibrary(g_slots[i].loaded_dll);
					g_slots[i].loaded_dll = nullptr;
				}
				removed.push_back(g_slots[i].id);
				g_slots[i].in_use = false;
			}
		}
	} else {
		bool found = false;
		for (int i = 0; i < MAX_NATIVE_HOOK_SLOTS; i++) {
			if (g_slots[i].in_use && (g_slots[i].id == target_id || g_slots[i].symbol == target_id || g_slots[i].resolved_name == target_id)) {
				found = true;
				MH_STATUS status = MH_RemoveHook((LPVOID)g_slots[i].target_addr);
				if (status != MH_OK) {
					errors.push_back(g_slots[i].id + ": " + MH_StatusToString(status));
					continue;
				}
				if (g_slots[i].loaded_dll) {
					FreeLibrary(g_slots[i].loaded_dll);
					g_slots[i].loaded_dll = nullptr;
				}
				removed.push_back(g_slots[i].id);
				g_slots[i].in_use = false;
				break;
			}
		}
		if (!found) {
			res["ok"] = false;
			res["error"] = "Native hook '" + target_id + "' not found.";
			return res;
		}
	}

	res["ok"] = errors.is_empty();
	res["errors"] = errors;
	res["unhooked"] = removed;
	return res;
#endif
}

Dictionary AIDebugNative::list_native_hooks() {
	Dictionary res;
	Array list;
#if defined(WINDOWS_ENABLED) && defined(_WIN64)
	MutexLock lock(g_hooks_mutex);
	for (int i = 0; i < MAX_NATIVE_HOOK_SLOTS; i++) {
		if (g_slots[i].in_use) {
			list.push_back(_slot_to_dict(g_slots[i]));
		}
	}
#endif
	res["ok"] = true;
	res["native_hooks"] = list;
	return res;
}

void AIDebugNative::unhook_all() {
#if defined(WINDOWS_ENABLED) && defined(_WIN64)
	unhook_native("all");
#endif
}
