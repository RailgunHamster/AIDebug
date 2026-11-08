/**************************************************************************/
/*  ai_debug_dump.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_debug_dump.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/time.h"

#ifdef WINDOWS_ENABLED
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(
		HANDLE hProcess,
		DWORD ProcessId,
		HANDLE hFile,
		MINIDUMP_TYPE DumpType,
		PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
		PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
		PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
#endif

Dictionary AIDebugDump::write_minidump(const String &p_mode, const String &p_custom_path) {
	Dictionary res;
#ifndef WINDOWS_ENABLED
	res["ok"] = false;
	res["error"] = "MiniDumpWriteDump is only supported on Windows.";
	return res;
#else
	String mode = p_mode.strip_edges().to_lower();
	if (mode.is_empty()) {
		mode = "mini";
	}
	if (mode != "mini" && mode != "full") {
		res["ok"] = false;
		res["error"] = "Invalid dump mode '" + p_mode + "'. Expected 'mini' or 'full'.";
		return res;
	}

	String user_dir = "user://ai_debug/dumps";
	String global_dir = ProjectSettings::get_singleton()->globalize_path(user_dir);
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (da.is_valid()) {
		da->make_dir_recursive(global_dir);
	}

	String user_path = p_custom_path.strip_edges();
	String global_path;
	if (user_path.is_empty()) {
		Dictionary dt = Time::get_singleton()->get_datetime_dict_from_system();
		int64_t pid = OS::get_singleton()->get_process_id();
		String filename = vformat("dump-%d-%04d%02d%02d-%02d%02d%02d-%s.dmp",
				pid,
				(int)dt["year"], (int)dt["month"], (int)dt["day"],
				(int)dt["hour"], (int)dt["minute"], (int)dt["second"],
				mode);
		user_path = user_dir.path_join(filename);
		global_path = global_dir.path_join(filename);
	} else if (user_path.begins_with("user://") || user_path.begins_with("res://")) {
		global_path = ProjectSettings::get_singleton()->globalize_path(user_path);
		if (da.is_valid()) {
			da->make_dir_recursive(global_path.get_base_dir());
		}
	} else {
		global_path = user_path;
		if (da.is_valid()) {
			da->make_dir_recursive(global_path.get_base_dir());
		}
	}

	HMODULE dbghelp_mod = LoadLibraryW(L"dbghelp.dll");
	if (!dbghelp_mod) {
		res["ok"] = false;
		res["error"] = "Failed to load dbghelp.dll.";
		return res;
	}

	MiniDumpWriteDumpFn write_dump_fn = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp_mod, "MiniDumpWriteDump");
	if (!write_dump_fn) {
		FreeLibrary(dbghelp_mod);
		res["ok"] = false;
		res["error"] = "Failed to find MiniDumpWriteDump in dbghelp.dll.";
		return res;
	}

	Char16String wpath = global_path.utf16();
	HANDLE hFile = CreateFileW(
			(LPCWSTR)wpath.get_data(),
			GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);

	if (hFile == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		FreeLibrary(dbghelp_mod);
		res["ok"] = false;
		res["error"] = vformat("CreateFileW failed for '%s' (Win32 error %d).", global_path, (int)err);
		return res;
	}

	MINIDUMP_TYPE dump_type;
	if (mode == "full") {
		dump_type = (MINIDUMP_TYPE)(MiniDumpWithFullMemory |
				MiniDumpWithFullMemoryInfo |
				MiniDumpWithHandleData |
				MiniDumpWithThreadInfo |
				MiniDumpWithUnloadedModules);
	} else {
		dump_type = (MINIDUMP_TYPE)(MiniDumpNormal |
				MiniDumpWithDataSegs |
				MiniDumpWithHandleData |
				MiniDumpWithThreadInfo |
				MiniDumpWithUnloadedModules |
				MiniDumpWithIndirectlyReferencedMemory);
	}

	BOOL ok = write_dump_fn(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			dump_type,
			nullptr,
			nullptr,
			nullptr);

	DWORD write_err = ok ? 0 : GetLastError();
	LARGE_INTEGER file_size{};
	GetFileSizeEx(hFile, &file_size);
	CloseHandle(hFile);
	FreeLibrary(dbghelp_mod);

	if (!ok) {
		res["ok"] = false;
		res["error"] = vformat("MiniDumpWriteDump failed (Win32 error %d).", (int)write_err);
		return res;
	}

	res["ok"] = true;
	res["mode"] = mode;
	res["path"] = global_path.simplify_path();
	res["user_path"] = user_path;
	res["size_bytes"] = (int64_t)file_size.QuadPart;
	return res;
#endif
}
