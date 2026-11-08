/**************************************************************************/
/*  ai_debug_export_plugin.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/export/editor_export_plugin.h"

class AIDebugExportPlugin : public EditorExportPlugin {
	GDSOFTCLASS(AIDebugExportPlugin, EditorExportPlugin);

	static String _get_csharp_project_name();

protected:
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;

public:
	virtual String get_name() const override { return "AIDebug"; }
};

#endif // TOOLS_ENABLED
