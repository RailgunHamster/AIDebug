/**************************************************************************/
/*  ai_debug_export_plugin.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_debug_export_plugin.h"

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

String AIDebugExportPlugin::_get_csharp_project_name() {
	String name = GLOBAL_GET("dotnet/project/assembly_name");
	if (name.is_empty()) {
		name = GLOBAL_GET("application/config/name");
		const char *invalid_chars[] = { ":", "*", "?", "\"", "<", ">", "|", "/", "\\", ";", "'", "=", ",", nullptr };
		name = name.strip_edges();
		for (int i = 0; invalid_chars[i] != nullptr; i++) {
			name = name.replace(invalid_chars[i], "-");
		}
	}
	if (name.is_empty()) {
		name = "UnnamedProject";
	}
	if (name == "GodotSharp" || name == "GodotSharpEditor" || name == "Godot.SourceGenerators") {
		name += "_";
	}
	return name;
}

void AIDebugExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	// Strictly never export Godot.AIDebug.dll or dependencies in release builds.
	if (!p_debug) {
		return;
	}

	// Only copy managed AI Debug assemblies if the exported project uses .NET / C#.
	bool has_dotnet = p_features.has("dotnet");
	if (!has_dotnet) {
		String asm_name = GLOBAL_GET("dotnet/project/assembly_name");
		if (!asm_name.is_empty()) {
			has_dotnet = true;
		}
	}
	if (!has_dotnet) {
		return;
	}

	String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	String ai_debug_dir = exe_dir.path_join("GodotSharp").path_join("AIDebug");
	if (!DirAccess::exists(ai_debug_dir)) {
		WARN_PRINT("AI Debug: Managed assemblies directory not found at '" + ai_debug_dir + "'.");
		return;
	}

	String platform = "windows";
	if (p_features.has("windows")) {
		platform = "windows";
	} else if (p_features.has("linuxbsd")) {
		platform = "linuxbsd";
	} else if (p_features.has("macos")) {
		platform = "macos";
	} else if (p_features.has("android")) {
		platform = "android";
	}

	Vector<String> archs;
	if (p_features.has("x86_64")) {
		archs.push_back("x86_64");
	}
	if (p_features.has("x86_32")) {
		archs.push_back("x86_32");
	}
	if (p_features.has("arm64")) {
		archs.push_back("arm64");
	}
	if (p_features.has("arm32")) {
		archs.push_back("arm32");
	}
	if (archs.is_empty()) {
		archs.push_back("x86_64");
	}

	String project_name = _get_csharp_project_name();
	Ref<DirAccess> da = DirAccess::open(ai_debug_dir);
	ERR_FAIL_COND(da.is_null());

	for (const String &arch : archs) {
		String target_data_dir = vformat("data_%s_%s_%s", project_name, platform, arch);
		if (platform == "macos") {
			target_data_dir = String("Contents/Resources").path_join(target_data_dir);
		}

		da->list_dir_begin();
		String item = da->get_next();
		while (!item.is_empty()) {
			if (!da->current_is_dir()) {
				String ext = item.get_extension().to_lower();
				// Copy DLLs and PDBs (skip .deps.json so isolated context resolution doesn't conflict with GodotSharp).
				if (ext == "dll" || ext == "pdb") {
					String full_path = ai_debug_dir.path_join(item);
					if (platform == "android") {
						Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(full_path);
						if (!bytes.is_empty()) {
							add_file(vformat("res://.godot/mono/publish/%s/%s", arch, item), bytes, false);
						}
					} else {
						add_shared_object(full_path, Vector<String>(), target_data_dir);
					}
				}
			}
			item = da->get_next();
		}
		da->list_dir_end();
	}
}

#endif // TOOLS_ENABLED
