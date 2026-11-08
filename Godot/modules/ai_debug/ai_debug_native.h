/**************************************************************************/
/*  ai_debug_native.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// Phase 2: Native symbol hooking via MinHook + DbgHelp (Windows x64).
class AIDebugNative {
public:
	static Dictionary hook_native(const Dictionary &p_args);
	static Dictionary unhook_native(const String &p_id);
	static Dictionary list_native_hooks();
	static void unhook_all();
};
