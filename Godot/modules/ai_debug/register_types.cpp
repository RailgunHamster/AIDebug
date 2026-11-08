/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "register_types.h"

#include "ai_debug_server.h"

#include "core/config/project_settings.h"
#include "scene/main/scene_tree.h"

#ifdef TOOLS_ENABLED
#include "editor/ai_debug_editor_plugin.h"
#include "editor/ai_debug_export_plugin.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#endif

static AIDebugServer *ai_debug_server = nullptr;
#ifdef TOOLS_ENABLED
static Ref<AIDebugEditorDebuggerPlugin> ai_debug_debugger_plugin;
#endif

#ifdef TOOLS_ENABLED
static void _ai_debug_editor_init() {
	if (EditorExport::get_singleton()) {
		Ref<AIDebugExportPlugin> export_plugin;
		export_plugin.instantiate();
		EditorExport::get_singleton()->add_export_plugin(export_plugin);
	}
	if (EditorDebuggerNode::get_singleton()) {
		ai_debug_debugger_plugin.instantiate();
		EditorDebuggerNode::get_singleton()->add_debugger_plugin(ai_debug_debugger_plugin);
	}
}
#endif

void initialize_ai_debug_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GLOBAL_DEF("ai_debug/hotkey_enabled", true);
		GLOBAL_DEF("ai_debug/show_overlay", true);
		GLOBAL_DEF(PropertyInfo(Variant::INT, "ai_debug/auto_revert_minutes", PROPERTY_HINT_RANGE, "1,240,1"), 30);

		ai_debug_server = memnew(AIDebugServer);
		SceneTree::add_idle_callback(&AIDebugServer::on_idle_frame);

#ifdef TOOLS_ENABLED
		EditorNode::add_init_callback(&_ai_debug_editor_init);
#endif
	}
}

void uninitialize_ai_debug_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
#ifdef TOOLS_ENABLED
		if (ai_debug_debugger_plugin.is_valid()) {
			if (EditorDebuggerNode::get_singleton()) {
				EditorDebuggerNode::get_singleton()->remove_debugger_plugin(ai_debug_debugger_plugin);
			}
			ai_debug_debugger_plugin.unref();
		}
#endif
		if (ai_debug_server) {
			memdelete(ai_debug_server);
			ai_debug_server = nullptr;
		}
	}
}
