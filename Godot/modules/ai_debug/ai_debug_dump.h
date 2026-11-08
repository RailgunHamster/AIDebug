/**************************************************************************/
/*  ai_debug_dump.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

class AIDebugDump {
public:
	static Dictionary write_minidump(const String &p_mode = "mini", const String &p_custom_path = "");
};
