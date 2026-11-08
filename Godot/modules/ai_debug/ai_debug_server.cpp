/**************************************************************************/
/*  ai_debug_server.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_debug_server.h"

#include "ai_debug_dump.h"
#include "ai_debug_native.h"
#include "ai_debug_tools.gen.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/debugger/engine_debugger.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/math/expression.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/variant/variant_utility.h"
#include "core/version.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/main/canvas_layer.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/style_box_flat.h"
#include "scene/resources/texture.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif

#ifdef MODULE_MONO_ENABLED
#include "modules/mono/mono_gd/gd_mono.h"
#endif

#ifdef TOOLS_ENABLED
#include "editor/ai_debug_editor_plugin.h"
#endif

#include <cstdio>

// ============================================================================
// AIDebugLogger
// ============================================================================

AIDebugLogger::AIDebugLogger(int p_capacity) {
	capacity = MAX(p_capacity, 64);
	ring.resize(capacity);
}

void AIDebugLogger::_push_entry(const String &p_level, const String &p_msg) {
	String trimmed = p_msg.strip_edges();
	if (trimmed.is_empty()) {
		return;
	}
	MutexLock lock(mutex);
	int idx = (head + count) % capacity;
	if (count == capacity) {
		head = (head + 1) % capacity;
	} else {
		count++;
	}
	Entry &e = ring.write[idx];
	e.timestamp_msec = OS::get_singleton() ? OS::get_singleton()->get_ticks_msec() : 0;
	e.time_str = Time::get_singleton() ? Time::get_singleton()->get_time_string_from_system() : "";
	e.level = p_level;
	e.message = trimmed;
}

void AIDebugLogger::logv(const char *p_format, va_list p_list, bool p_err) {
	if (!should_log(p_err)) {
		return;
	}
	char buf[4096];
	va_list copy;
	va_copy(copy, p_list);
	int len = vsnprintf(buf, sizeof(buf), p_format, copy);
	va_end(copy);
	if (len <= 0) {
		return;
	}
	String msg = String::utf8(buf);
	_push_entry(p_err ? "error" : "info", msg);
}

void AIDebugLogger::log_error(const char *p_function, const char *p_file, int p_line, const char *p_code, const char *p_rationale, bool p_editor_notify, ErrorType p_type, const Vector<Ref<ScriptBacktrace>> &p_script_backtraces) {
	if (!should_log(true)) {
		return;
	}
	String level = (p_type == ERR_WARNING) ? "warning" : "error";
	String reason = (p_rationale && *p_rationale) ? String::utf8(p_rationale) : String::utf8(p_code ? p_code : "");
	String text = vformat("[%s] %s:%d (%s): %s", error_type_string(p_type), p_file ? p_file : "", p_line, p_function ? p_function : "", reason);
	_push_entry(level, text);
}

Array AIDebugLogger::get_recent(int p_limit, const String &p_level_filter) {
	MutexLock lock(mutex);
	Array out;
	int lim = (p_limit <= 0) ? 100 : p_limit;
	String filter = p_level_filter.strip_edges().to_lower();

	Vector<Entry> matched;
	for (int i = 0; i < count; i++) {
		int idx = (head + i) % capacity;
		const Entry &e = ring[idx];
		if (!filter.is_empty() && filter != "all" && e.level != filter) {
			continue;
		}
		matched.push_back(e);
	}

	int start = MAX(0, matched.size() - lim);
	for (int i = start; i < matched.size(); i++) {
		Dictionary d;
		d["timestamp_msec"] = (int64_t)matched[i].timestamp_msec;
		d["time"] = matched[i].time_str;
		d["level"] = matched[i].level;
		d["message"] = matched[i].message;
		out.push_back(d);
	}
	return out;
}

void AIDebugLogger::clear() {
	MutexLock lock(mutex);
	head = 0;
	count = 0;
}

// ============================================================================
// AIDebugServer Lifecycle & Activation
// ============================================================================

AIDebugServer *AIDebugServer::singleton = nullptr;

AIDebugServer::AIDebugServer() {
	singleton = this;
	_check_startup_activation();
}

AIDebugServer::~AIDebugServer() {
	deactivate();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void AIDebugServer::on_idle_frame() {
	if (singleton) {
		singleton->poll();
	}
#ifdef TOOLS_ENABLED
	if (AIDebugEditorDebuggerPlugin::get_singleton()) {
		AIDebugEditorDebuggerPlugin::get_singleton()->poll();
	}
#endif
}

static String _generate_random_hex(int p_bytes) {
	Vector<uint8_t> buf;
	buf.resize(p_bytes);
	if (CryptoCore::generate_random(buf.ptrw(), p_bytes) != OK) {
		uint64_t t = OS::get_singleton()->get_ticks_usec();
		for (int i = 0; i < p_bytes; i++) {
			buf.write[i] = (uint8_t)((t >> ((i % 8) * 8)) ^ (i * 37));
		}
	}
	return String::hex_encode_buffer(buf.ptr(), p_bytes);
}

static String _sha256_hex(const String &p_str) {
	CharString utf8 = p_str.utf8();
	unsigned char hash[32];
	if (CryptoCore::sha256((const uint8_t *)utf8.get_data(), utf8.length(), hash) == OK) {
		return String::hex_encode_buffer(hash, 32);
	}
	return "";
}

void AIDebugServer::_check_startup_activation() {
	bool want_activate = false;
	int port = 7651;
	String bind = "127.0.0.1";
	String tok;

	String env_enable = OS::get_singleton()->get_environment("GODOT_AI_DEBUG").strip_edges().to_lower();
	if (env_enable == "1" || env_enable == "true" || env_enable == "yes") {
		want_activate = true;
	}
	String env_port = OS::get_singleton()->get_environment("GODOT_AI_DEBUG_PORT").strip_edges();
	if (!env_port.is_empty() && env_port.is_valid_int()) {
		port = env_port.to_int();
	}
	String env_token = OS::get_singleton()->get_environment("GODOT_AI_DEBUG_TOKEN").strip_edges();
	if (!env_token.is_empty()) {
		tok = env_token;
	}
	String env_bind = OS::get_singleton()->get_environment("GODOT_AI_DEBUG_BIND").strip_edges();
	if (!env_bind.is_empty()) {
		bind = env_bind;
	}

	auto parse_args = [&](const List<String> &p_args) {
		for (const List<String>::Element *E = p_args.front(); E; E = E->next()) {
			const String &a = E->get();
			if (a == "--ai-debug") {
				want_activate = true;
			} else if (a.begins_with("--ai-debug-port=")) {
				want_activate = true;
				port = a.get_slicec('=', 1).to_int();
			} else if (a == "--ai-debug-port" && E->next()) {
				want_activate = true;
				port = E->next()->get().to_int();
				E = E->next();
			} else if (a.begins_with("--ai-debug-token=")) {
				want_activate = true;
				tok = a.get_slicec('=', 1);
			} else if (a == "--ai-debug-token" && E->next()) {
				want_activate = true;
				tok = E->next()->get();
				E = E->next();
			} else if (a.begins_with("--ai-debug-bind=")) {
				want_activate = true;
				bind = a.get_slicec('=', 1);
			} else if (a == "--ai-debug-bind" && E->next()) {
				want_activate = true;
				bind = E->next()->get();
				E = E->next();
			} else if (a.begins_with("--ai-debug-timeout=")) {
				int mins = a.get_slicec('=', 1).to_int();
				if (mins > 0) {
					auto_revert_timeout_msec = (uint64_t)mins * 60ULL * 1000ULL;
				}
			}
		}
	};

	parse_args(OS::get_singleton()->get_cmdline_args());
	parse_args(OS::get_singleton()->get_cmdline_user_args());

	if (want_activate) {
		activate(port, bind, tok, false);
	}
}

bool AIDebugServer::activate(int p_port, const String &p_bind_ip, const String &p_token, bool p_from_hotkey) {
	if (active) {
		return true;
	}

	String bind = p_bind_ip.strip_edges();
	if (bind.is_empty()) {
		bind = "127.0.0.1";
	}

	bool is_local_bind = (bind == "127.0.0.1" || bind == "::1" || bind == "localhost");
	if (!is_local_bind && p_token.strip_edges().is_empty()) {
		ERR_PRINT("AI Debug: Binding to non-loopback address '" + bind + "' requires an explicit --ai-debug-token=<token>. Refusing to start.");
		return false;
	}

	String tok = p_token.strip_edges();
	if (tok.is_empty()) {
		tok = _generate_random_hex(8); // 16-char hex one-time token
	}

	tcp_server.instantiate();
	IPAddress bind_addr(bind);
	if (!bind_addr.is_valid()) {
		ERR_PRINT("AI Debug: Invalid bind IP address '" + bind + "'.");
		tcp_server.unref();
		return false;
	}

	int port = (p_port > 0 && p_port <= 65535) ? p_port : 7651;
	Error err = tcp_server->listen((uint16_t)port, bind_addr);
	if (err != OK) {
		ERR_PRINT(vformat("AI Debug: Failed to listen on %s:%d (Error %d).", bind, port, (int)err));
		tcp_server.unref();
		return false;
	}

	listen_port = tcp_server->get_local_port();
	bind_ip = bind;
	token = tok;
	session_id = _generate_random_hex(8);
	activated_via_hotkey = p_from_hotkey;
	activated_ticks_msec = OS::get_singleton()->get_ticks_msec();
	last_request_ticks_msec = activated_ticks_msec;
	activated_time_iso = Time::get_singleton()->get_datetime_string_from_system(false, true);

	if (ProjectSettings::get_singleton()) {
		show_overlay = GLOBAL_GET("ai_debug/show_overlay");
		int mins = GLOBAL_GET("ai_debug/auto_revert_minutes");
		if (mins > 0) {
			auto_revert_timeout_msec = (uint64_t)mins * 60ULL * 1000ULL;
		}
	}

	initial_time_scale = Engine::get_singleton()->get_time_scale();
	initial_paused = SceneTree::get_singleton() ? SceneTree::get_singleton()->is_paused() : false;

	if (!logger) {
		logger = memnew(AIDebugLogger(2000));
		OS::get_singleton()->add_logger(logger);
	}

	active = true;

	// Write connection info file to user://ai_debug/session.json for easy local tool discovery
	{
		String user_dir = ProjectSettings::get_singleton()->globalize_path("user://ai_debug");
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid()) {
			da->make_dir_recursive(user_dir);
		}
		Ref<FileAccess> fa = FileAccess::open("user://ai_debug/session.json", FileAccess::WRITE);
		if (fa.is_valid()) {
			Dictionary sess;
			sess["pid"] = (int64_t)OS::get_singleton()->get_process_id();
			sess["port"] = listen_port;
			sess["bind"] = bind_ip;
			sess["token"] = token;
			sess["url"] = vformat("http://%s:%d/mcp", bind_ip, listen_port);
			sess["activated_at"] = activated_time_iso;
			fa->store_line(JSON::stringify(sess));
		}
	}

	// Load managed Godot.AIDebug.dll on activation if .NET runtime is active
#ifdef MODULE_MONO_ENABLED
	if (GDMono::get_singleton() && GDMono::get_singleton()->is_initialized()) {
		(void)_invoke_managed("start", Dictionary());
	}
#endif

	print_line(vformat("[AI Debug] Activated on http://%s:%d/mcp | Token: %s | Mode: %s",
			bind_ip, listen_port, token, p_from_hotkey ? "hotkey" : "startup"));

	return true;
}

void AIDebugServer::deactivate() {
	if (debugger_capture_registered) {
		if (EngineDebugger::is_active() && EngineDebugger::has_capture("ai_debug")) {
			EngineDebugger::unregister_message_capture("ai_debug");
		}
		debugger_capture_registered = false;
	}

	if (!active) {
		return;
	}

	revert_all("deactivate");
	_remove_overlay();

	for (int i = 0; i < clients.size(); i++) {
		if (clients[i].peer.is_valid()) {
			clients.write[i].peer->disconnect_from_host();
		}
	}
	clients.clear();

	if (tcp_server.is_valid()) {
		tcp_server->stop();
		tcp_server.unref();
	}

	active = false;
	AIDebugHTTP::clear_session_file(token);
}

void AIDebugServer::revert_all(const String &p_reason) {
	last_reset_result.clear();
	last_reset_result["ok"] = true;
	last_reset_result["script_state_reset"] = true;
	last_reset_result["patches_cleared"] = true;
	last_reset_result["execution_may_continue"] = false;
#ifdef MODULE_MONO_ENABLED
	if (managed_loaded && GDMono::get_singleton() && GDMono::get_singleton()->is_runtime_initialized()) {
		Variant response = JSON::parse_string(GDMono::get_singleton()->ai_debug_invoke("reset", "{}"));
		if (response.get_type() == Variant::DICTIONARY) {
			last_reset_result = response;
		} else {
			last_reset_result["ok"] = false;
			last_reset_result["patches_cleared"] = false;
			last_reset_result["error"] = "Invalid managed reset response.";
		}
	}
#endif
	for (PendingEvaluation *task : evaluations) {
		Dictionary cancelled;
		cancelled["ok"] = false;
		cancelled["cancelled"] = true;
		cancelled["error"] = "Evaluation cancelled by " + p_reason + ". User code cancellation is cooperative.";
		cancelled["execution_may_continue"] = last_reset_result.get("execution_may_continue", false);
		_respond_evaluation(task, _mcp_json(cancelled, true));
		memdelete(task);
	}
	evaluations.clear();
	Dictionary native_cleanup = AIDebugNative::unhook_native("all");
	// Unsupported platforms have no native hooks to remove.
	bool hooks_cleared = native_cleanup.get("ok", false);
#if !defined(WINDOWS_ENABLED) || !defined(_WIN64)
	hooks_cleared = true;
#endif
	last_reset_result["native_hooks_cleared"] = hooks_cleared;
	last_reset_result["native_cleanup"] = native_cleanup;
	last_reset_result["ok"] = bool(last_reset_result.get("ok", false)) && hooks_cleared;
	if (modified_state) {
		if (Engine::get_singleton()) {
			Engine::get_singleton()->set_time_scale(initial_time_scale);
		}
		if (SceneTree::get_singleton()) {
			SceneTree::get_singleton()->set_pause(initial_paused);
		}
		modified_state = false;
	}
	step_frames_remaining = 0;
	step_restore_pause = false;
	last_reset_result["property_changes_reverted"] = false;
}

void AIDebugServer::_check_hotkey() {
#if defined(DEBUG_ENABLED) || defined(TOOLS_ENABLED)
	if (active) {
		return;
	}
	Input *inp = Input::get_singleton();
	if (!inp) {
		return;
	}
	if (ProjectSettings::get_singleton() && !bool(GLOBAL_GET("ai_debug/hotkey_enabled"))) {
		return;
	}

	bool pressed = inp->is_key_pressed(Key::CTRL) &&
			inp->is_key_pressed(Key::ALT) &&
			inp->is_key_pressed(Key::SHIFT) &&
			inp->is_key_pressed(Key::F12);

	if (pressed && !hotkey_pressed_prev) {
		activate(7651, "127.0.0.1", "", true);
	}
	hotkey_pressed_prev = pressed;
#endif
}

void AIDebugServer::poll() {
	_ensure_debugger_capture();

	if (!active) {
		_check_hotkey();
		return;
	}

	// Ensure managed assembly is loaded if .NET initialized after AIDebugServer constructor
#ifdef MODULE_MONO_ENABLED
	if (!managed_loaded && GDMono::get_singleton() && GDMono::get_singleton()->is_initialized()) {
		(void)_invoke_managed("start", Dictionary());
	}
#endif

	_update_stepping();
	_poll_workers();
	_poll_network();
	_update_overlay();

	// Check inactivity auto-revert guardrail
	uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (has_ever_connected && modified_state && auto_revert_timeout_msec > 0) {
		if (now - last_request_ticks_msec > auto_revert_timeout_msec) {
			revert_all("inactivity_timeout");
			has_ever_connected = false;
		}
	}
}

void AIDebugServer::_ensure_debugger_capture() {
	if (debugger_capture_registered) {
		return;
	}
	if (!EngineDebugger::is_active() || Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	EngineDebugger::register_message_capture("ai_debug", EngineDebugger::Capture(this, &AIDebugServer::_debugger_capture_static));
	debugger_capture_registered = true;

	Array hello_args;
	hello_args.push_back((int64_t)OS::get_singleton()->get_process_id());
	hello_args.push_back(GLOBAL_GET("application/config/name"));
	EngineDebugger::get_singleton()->send_message("ai_debug:hello", hello_args);
}

void AIDebugServer::_activate_for_debugger() {
	if (active) {
		return;
	}
	listen_port = 0;
	bind_ip = "editor_debugger";
	activated_via_debugger = true;
	activated_ticks_msec = OS::get_singleton()->get_ticks_msec();
	last_request_ticks_msec = activated_ticks_msec;
	activated_time_iso = Time::get_singleton()->get_datetime_string_from_system(false, true);

	if (ProjectSettings::get_singleton()) {
		show_overlay = GLOBAL_GET("ai_debug/show_overlay");
		int mins = GLOBAL_GET("ai_debug/auto_revert_minutes");
		if (mins > 0) {
			auto_revert_timeout_msec = (uint64_t)mins * 60ULL * 1000ULL;
		}
	}

	initial_time_scale = Engine::get_singleton()->get_time_scale();
	initial_paused = SceneTree::get_singleton() ? SceneTree::get_singleton()->is_paused() : false;

	if (!logger) {
		logger = memnew(AIDebugLogger(2000));
		OS::get_singleton()->add_logger(logger);
	}

	active = true;
#ifdef MODULE_MONO_ENABLED
	if (GDMono::get_singleton() && GDMono::get_singleton()->is_initialized()) {
		(void)_invoke_managed("start", Dictionary());
	}
#endif
}

Error AIDebugServer::_debugger_capture_static(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured) {
	AIDebugServer *srv = static_cast<AIDebugServer *>(p_user);
	if (!srv) {
		r_captured = false;
		return OK;
	}
	return srv->_on_debugger_message(p_msg, p_args, r_captured);
}

Error AIDebugServer::_on_debugger_message(const String &p_msg, const Array &p_args, bool &r_captured) {
	if (p_msg == "rpc" && p_args.size() >= 2) {
		r_captured = true;
		_activate_for_debugger();
		has_ever_connected = true;
		last_request_ticks_msec = OS::get_singleton()->get_ticks_msec();

		int64_t req_id = p_args[0];
		String body = p_args[1];
		Variant parsed = JSON::parse_string(body);
		if (parsed.get_type() == Variant::NIL) {
			Dictionary err_res;
			err_res["jsonrpc"] = "2.0";
			err_res["id"] = Variant();
			Dictionary eobj;
			eobj["code"] = -32700;
			eobj["message"] = "Parse error";
			err_res["error"] = eobj;
			Array resp;
			resp.push_back(req_id);
			resp.push_back(JSON::stringify(err_res));
			EngineDebugger::get_singleton()->send_message("ai_debug:rpc_response", resp);
			return OK;
		}

		bool is_async = false;
		current_debugger_request_id = req_id;
		Variant out_v = _dispatch_rpc(-1, parsed, is_async);
		current_debugger_request_id = 0;
		if (!is_async) {
			Array resp;
			resp.push_back(req_id);
			resp.push_back(out_v.get_type() == Variant::NIL ? String() : JSON::stringify(out_v));
			EngineDebugger::get_singleton()->send_message("ai_debug:rpc_response", resp);
		}
		return OK;
	}

	if (p_msg == "disconnect") {
		r_captured = true;
		revert_all("editor_debugger_disconnect");
		has_ever_connected = false;
		return OK;
	}

	r_captured = false;
	return OK;
}

void AIDebugServer::_update_stepping() {
	if (step_frames_remaining > 0) {
		step_frames_remaining--;
		if (step_frames_remaining == 0 && step_restore_pause) {
			if (SceneTree::get_singleton()) {
				SceneTree::get_singleton()->set_pause(true);
			}
		}
	}
}

void AIDebugServer::_update_overlay() {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return;
	}
	if (OS::get_singleton()->get_main_loop() == nullptr || DisplayServer::get_singleton()->get_name() == "headless") {
		return;
	}
	if (!show_overlay) {
		_remove_overlay();
		return;
	}

	CanvasLayer *layer = ObjectDB::get_instance<CanvasLayer>(overlay_layer_id);
	Label *label = ObjectDB::get_instance<Label>(overlay_label_id);

	if (!layer || !label) {
		layer = memnew(CanvasLayer);
		layer->set_name("_AIDebugOverlayLayer");
		layer->set_layer(128);
		layer->set_process_mode(Node::PROCESS_MODE_ALWAYS);

		PanelContainer *panel = memnew(PanelContainer);
		panel->set_name("_AIDebugOverlayPanel");
		panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		panel->set_position(Point2(10, 10));

		Ref<StyleBoxFlat> sb;
		sb.instantiate();
		sb->set_bg_color(Color(0.08f, 0.09f, 0.12f, 0.85f));
		sb->set_border_color(Color(0.2f, 0.75f, 0.45f, 0.9f));
		sb->set_border_width_all(1);
		sb->set_corner_radius_all(4);
		sb->set_content_margin_individual(8, 4, 8, 4);
		panel->add_theme_style_override("panel", sb);

		label = memnew(Label);
		label->set_name("_AIDebugOverlayLabel");
		label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		label->add_theme_color_override("font_color", Color(0.55f, 0.95f, 0.7f, 1.0f));
		label->add_theme_font_size_override("font_size", 12);
		panel->add_child(label);
		layer->add_child(panel);

		tree->get_root()->add_child(layer);
		overlay_layer_id = layer->get_instance_id();
		overlay_label_id = label->get_instance_id();
	}

	String text;
	if (has_ever_connected) {
		text = String::utf8("AI Debug 已连接 (:") + itos(listen_port) + ")";
	} else {
		text = vformat("AI Debug :%d | Token: %s", listen_port, token);
	}
	if (label->get_text() != text) {
		label->set_text(text);
	}
}

void AIDebugServer::_remove_overlay() {
	CanvasLayer *layer = ObjectDB::get_instance<CanvasLayer>(overlay_layer_id);
	if (layer) {
		if (layer->get_parent()) {
			layer->get_parent()->remove_child(layer);
		}
		memdelete(layer);
	}
	overlay_layer_id = ObjectID();
	overlay_label_id = ObjectID();
}

// ============================================================================
// HTTP + JSON-RPC (MCP Streamable HTTP) Server
// ============================================================================

void AIDebugServer::_poll_network() {
	if (tcp_server.is_null() || !tcp_server->is_listening()) {
		return;
	}

	uint64_t now = OS::get_singleton()->get_ticks_msec();
	while (tcp_server->is_connection_available()) {
		Ref<StreamPeerTCP> peer = tcp_server->take_connection();
		if (peer.is_valid()) {
			if (clients.size() >= AIDebugHTTP::MAX_CLIENTS) {
				peer->disconnect_from_host();
				continue;
			}
			peer->set_no_delay(true);
			ClientConn c;
			c.peer = peer;
			c.born_msec = now;
			clients.push_back(c);
		}
	}

	for (int i = clients.size() - 1; i >= 0; i--) {
		ClientConn &c = clients.write[i];
		Ref<StreamPeerTCP> peer = c.peer;
		if (peer.is_null()) {
			clients.remove_at(i);
			continue;
		}

		peer->poll();
		StreamPeerTCP::Status st = peer->get_status();

		if (c.close_at_msec > 0) {
			if (now >= c.close_at_msec || st != StreamPeerTCP::STATUS_CONNECTED) {
				peer->disconnect_from_host();
				clients.remove_at(i);
			}
			continue;
		}

		if (st == StreamPeerTCP::STATUS_ERROR || st == StreamPeerTCP::STATUS_NONE) {
			clients.remove_at(i);
			continue;
		}

		if (c.busy) {
			continue;
		}

		int avail = peer->get_available_bytes();
		if (avail > AIDebugHTTP::MAX_REQUEST_BYTES - c.buf.size()) {
			_respond_raw(c, 413, "Payload Too Large");
			continue;
		}
		if (avail > 0) {
			int old_size = c.buf.size();
			c.buf.resize(old_size + avail);
			int read_bytes = 0;
			Error err = peer->get_partial_data(c.buf.ptrw() + old_size, avail, read_bytes);
			if (err == OK && read_bytes > 0) {
				c.buf.resize(old_size + read_bytes);
			} else {
				c.buf.resize(old_size);
			}
		}

		HttpRequest req;
		if (!_try_parse_http(c, req)) {
			if (now - c.born_msec > AIDebugHTTP::REQUEST_TIMEOUT_MSEC) {
				peer->disconnect_from_host();
				clients.remove_at(i);
			}
			continue;
		}

		c.busy = true;
		c.busy_since_msec = now;
		_serve_http(i, req);
	}
}

bool AIDebugServer::_try_parse_http(ClientConn &c, HttpRequest &r_req) {
	return AIDebugHTTP::try_parse(c.buf, c.head_end, r_req);
}

void AIDebugServer::_serve_http(int p_client_idx, const HttpRequest &p_req) {
	ClientConn &c = clients.write[p_client_idx];

	int access_error = AIDebugHTTP::check_access(p_req, token);
	if (access_error != 0) {
		_write_audit_log("http_rejected", Dictionary(), true, 0, itos(access_error));
		_respond_raw(c, access_error, "Request rejected");
		return;
	}
	if (p_req.method == "OPTIONS") {
		Dictionary headers;
		headers["Allow"] = "POST, GET, DELETE, OPTIONS";
		_respond_raw(c, 204, "", headers);
		return;
	}

	last_request_ticks_msec = OS::get_singleton()->get_ticks_msec();
	has_ever_connected = true;

	if (p_req.method == "GET") {
		Dictionary h;
		h["Allow"] = "POST, DELETE";
		_respond_raw(c, 405, "", h);
		return;
	}

	if (p_req.method == "DELETE") {
		// Session disconnect via MCP HTTP DELETE -> auto revert all patches & state!
		_write_audit_log("session_disconnect", Dictionary(), false, 0, "Client sent HTTP DELETE /mcp");
		revert_all("client_disconnect");
		has_ever_connected = false;
		_respond_raw(c, 200, "");
		return;
	}

	if (p_req.method == "POST") {
		_serve_post(p_client_idx, p_req.body);
		return;
	}

	Dictionary h;
	h["Allow"] = "POST, GET, DELETE, OPTIONS";
	_respond_raw(c, 405, "", h);
}

void AIDebugServer::_serve_post(int p_client_idx, const String &p_body) {
	Variant parsed = JSON::parse_string(p_body);
	if (parsed.get_type() == Variant::NIL) {
		Dictionary err_res;
		err_res["jsonrpc"] = "2.0";
		err_res["id"] = Variant();
		Dictionary err_obj;
		err_obj["code"] = -32700;
		err_obj["message"] = "Parse error";
		err_res["error"] = err_obj;
		_respond_json(clients.write[p_client_idx], err_res);
		return;
	}

	if (parsed.get_type() == Variant::ARRAY) {
		Dictionary error;
		error["jsonrpc"] = "2.0";
		error["id"] = Variant();
		Dictionary detail;
		detail["code"] = -32600;
		detail["message"] = "Batch requests are not supported. Send one JSON-RPC message per HTTP request.";
		error["error"] = detail;
		_respond_json(clients.write[p_client_idx], error);
		return;
	}

	bool is_async = false;
	Variant out_v = _dispatch_rpc(p_client_idx, parsed, is_async);
	if (is_async) {
		// Worker thread will send the response when finished
		return;
	}

	if (out_v.get_type() == Variant::NIL) {
		_respond_raw(clients.write[p_client_idx], 202, "");
	} else {
		_respond_json(clients.write[p_client_idx], out_v);
	}
}

Variant AIDebugServer::_dispatch_rpc(int p_client_idx, const Variant &p_msg, bool &r_async) {
	r_async = false;
	if (p_msg.get_type() != Variant::DICTIONARY) {
		Dictionary err_res;
		err_res["jsonrpc"] = "2.0";
		err_res["id"] = Variant();
		Dictionary err_obj;
		err_obj["code"] = -32600;
		err_obj["message"] = "Invalid Request";
		err_res["error"] = err_obj;
		return err_res;
	}

	Dictionary msg = p_msg;
	bool has_id = msg.has("id");
	Variant id = msg.get("id", Variant());
	String method = msg.get("method", "");
	Dictionary prms = msg.get("params", Dictionary());

	auto make_ok = [&](const Variant &p_res) -> Variant {
		if (!has_id) {
			return Variant();
		}
		Dictionary d;
		d["jsonrpc"] = "2.0";
		d["id"] = id;
		d["result"] = p_res;
		return d;
	};

	if (method == "initialize") {
		String want = prms.get("protocolVersion", "2025-06-18");
		Dictionary res;
		res["protocolVersion"] = want == "2025-03-26" ? want : String("2025-06-18");
		Dictionary caps;
		Dictionary tools_cap;
		tools_cap["listChanged"] = false;
		caps["tools"] = tools_cap;
		res["capabilities"] = caps;
		Dictionary srv;
		srv["name"] = "godot-ai-debug";
		srv["title"] = "Godot AI Debug";
		srv["version"] = "0.2.0";
		res["serverInfo"] = srv;
		res["instructions"] = "Godot in-process runtime debugger (ai_debug). Inspect scene tree, read/write properties, evaluate GDScript/C#, apply Harmony runtime method patches, capture screenshots, logs, and minidumps.";
		return make_ok(res);
	}

	if (method == "ping") {
		return make_ok(Dictionary());
	}

	if (method == "tools/list") {
		Dictionary res;
		res["tools"] = _get_tool_definitions();
		return make_ok(res);
	}

	if (method == "tools/call") {
		String tool_name = prms.get("name", "");
		Dictionary args = prms.get("arguments", Dictionary());
		Dictionary result = _execute_tool(p_client_idx, id, tool_name, args, r_async);
		if (r_async) {
			return Variant();
		}
		return make_ok(result);
	}

	if (method == "resources/list") {
		Dictionary res;
		res["resources"] = Array();
		return make_ok(res);
	}

	if (method == "resources/templates/list") {
		Dictionary res;
		res["resourceTemplates"] = Array();
		return make_ok(res);
	}

	if (method == "prompts/list") {
		Dictionary res;
		res["prompts"] = Array();
		return make_ok(res);
	}

	if (method == "notifications/cancelled" || method.begins_with("notifications/")) {
		return Variant();
	}

	Dictionary err_res;
	err_res["jsonrpc"] = "2.0";
	err_res["id"] = id;
	Dictionary err_obj;
	err_obj["code"] = -32601;
	err_obj["message"] = "Method not found: " + method;
	err_res["error"] = err_obj;
	return err_res;
}

void AIDebugServer::_respond_json(ClientConn &c, const Variant &p_data) {
	Dictionary extra;
	extra["Content-Type"] = "application/json; charset=utf-8";
	extra["Mcp-Session-Id"] = session_id;
	_respond_raw(c, 200, JSON::stringify(p_data), extra);
}

void AIDebugServer::_respond_raw(ClientConn &c, int p_code, const String &p_body, const Dictionary &p_headers) {
	if (c.peer.is_null()) {
		return;
	}
	const char *status_text = "OK";
	switch (p_code) {
		case 200:
			status_text = "OK";
			break;
		case 202:
			status_text = "Accepted";
			break;
		case 204:
			status_text = "No Content";
			break;
		case 400:
			status_text = "Bad Request";
			break;
		case 403:
			status_text = "Forbidden";
			break;
		case 404:
			status_text = "Not Found";
			break;
		case 405:
			status_text = "Method Not Allowed";
			break;
		case 413:
			status_text = "Payload Too Large";
			break;
		case 500:
			status_text = "Internal Server Error";
			break;
	}

	CharString body_utf8 = p_body.utf8();
	int body_len = body_utf8.length();

	String head = vformat("HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n", p_code, status_text, body_len);
	if (!p_headers.has("Content-Type") && body_len > 0) {
		head += "Content-Type: text/plain; charset=utf-8\r\n";
	}
	Array keys = p_headers.keys();
	for (int i = 0; i < keys.size(); i++) {
		String k = keys[i];
		String v = p_headers[k];
		head += k + ": " + v + "\r\n";
	}
	head += "\r\n";

	CharString head_utf8 = head.utf8();
	if (c.peer->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
		c.peer->put_data((const uint8_t *)head_utf8.get_data(), head_utf8.length());
		if (body_len > 0) {
			c.peer->put_data((const uint8_t *)body_utf8.get_data(), body_len);
		}
	}
	c.close_at_msec = OS::get_singleton()->get_ticks_msec() + 50;
}

// ============================================================================
// Audit Log, Guardrails & Worker Dispatch
// ============================================================================

void AIDebugServer::_write_audit_log(const String &p_tool, const Dictionary &p_args, bool p_is_error, uint64_t p_elapsed_usec, const String &p_note) {
	Dictionary dt = Time::get_singleton()->get_datetime_dict_from_system();
	String ymd = vformat("%04d%02d%02d", (int)dt["year"], (int)dt["month"], (int)dt["day"]);

	String user_dir = ProjectSettings::get_singleton()->globalize_path("user://ai_debug");
	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (da.is_valid()) {
		da->make_dir_recursive(user_dir);
	}

	String log_path = vformat("user://ai_debug/audit-%s.jsonl", ymd);
	Ref<FileAccess> fa = FileAccess::exists(log_path)
			? FileAccess::open(log_path, FileAccess::READ_WRITE)
			: FileAccess::open(log_path, FileAccess::WRITE);
	if (fa.is_null()) {
		return;
	}
	fa->seek_end();

	Dictionary summary;
	Array keys = p_args.keys();
	for (int i = 0; i < keys.size(); i++) {
		String k = keys[i];
		Variant v = p_args[k];
		if ((k == "code" || k == "expression" || k == "source") && v.get_type() == Variant::STRING) {
			String code_str = v;
			Dictionary code_info;
			code_info["sha256"] = _sha256_hex(code_str);
			code_info["length"] = code_str.length();
			code_info["preview"] = code_str.substr(0, 200);
			summary[k] = code_info;
		} else {
			String s = v.stringify();
			if (s.length() > 200) {
				summary[k] = s.substr(0, 200) + "...";
			} else {
				summary[k] = v;
			}
		}
	}

	Dictionary entry;
	entry["time"] = Time::get_singleton()->get_datetime_string_from_system(false, true);
	entry["tool"] = p_tool;
	entry["args"] = summary;
	entry["status"] = p_is_error ? "error" : "ok";
	entry["elapsed_ms"] = (double)p_elapsed_usec / 1000.0;
	if (!p_note.is_empty()) {
		entry["note"] = p_note;
	}

	fa->store_line(JSON::stringify(entry));
}

Dictionary AIDebugServer::_mcp_text(const String &p_text, bool p_is_error) {
	Dictionary item;
	item["type"] = "text";
	item["text"] = p_text;
	Array content;
	content.push_back(item);
	Dictionary res;
	res["content"] = content;
	res["isError"] = p_is_error;
	return res;
}

Dictionary AIDebugServer::_mcp_json(const Variant &p_data, bool p_is_error) {
	return _mcp_text(JSON::stringify(p_data, "  "), p_is_error);
}

String AIDebugServer::_invoke_managed(const String &p_cmd, const Dictionary &p_args) {
#ifdef MODULE_MONO_ENABLED
	if (!GDMono::get_singleton() || !GDMono::get_singleton()->is_initialized()) {
		return "{\"ok\":false,\"error\":\".NET runtime is not initialized in this project.\"}";
	}
	String response = GDMono::get_singleton()->ai_debug_invoke(p_cmd, JSON::stringify(p_args));
	Variant status = JSON::parse_string(response);
	if (status.get_type() == Variant::DICTIONARY && bool(Dictionary(status).get("ok", false))) {
		managed_loaded = true;
	}
	return response;
#else
	return "{\"ok\":false,\"error\":\"Engine was built without module_mono_enabled.\"}";
#endif
}

Dictionary AIDebugServer::_execute_tool(int p_client_idx, const Variant &p_rpc_id, const String &p_name, const Dictionary &p_args, bool &r_async) {
	r_async = false;
	uint64_t t0 = OS::get_singleton()->get_ticks_usec();

	// Guardrail: single code payload size limit (default 256 KB)
	const char *code_keys[] = { "code", "expression", "source", nullptr };
	for (int i = 0; code_keys[i] != nullptr; i++) {
		if (p_args.has(code_keys[i])) {
			String s = p_args[code_keys[i]];
			int bytes = s.utf8().length();
			if (bytes > max_code_bytes) {
				uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - t0;
				String msg = vformat("Code size (%d bytes) exceeds maximum allowed limit (%d bytes).", bytes, max_code_bytes);
				_write_audit_log(p_name, p_args, true, elapsed, msg);
				return _mcp_text(msg, true);
			}
		}
	}

	Dictionary res;
	if (p_name == "info") {
		res = _tool_info(p_args);
	} else if (p_name == "tree") {
		res = _tool_tree(p_args);
	} else if (p_name == "get") {
		res = _tool_get(p_args);
	} else if (p_name == "set") {
		res = _tool_set(p_args);
	} else if (p_name == "call") {
		res = _tool_call(p_args);
	} else if (p_name == "eval_gd") {
		res = _tool_eval_gd(p_args);
	} else if (p_name == "run_gd") {
		res = _tool_run_gd(p_args);
	} else if (p_name == "eval_cs") {
		res = _tool_eval_cs(p_client_idx, p_rpc_id, p_args, r_async);
		if (r_async) {
			return Dictionary();
		}
	} else if (p_name == "patch_cs") {
		res = _tool_patch_cs(p_args);
	} else if (p_name == "unpatch") {
		res = _tool_unpatch(p_args);
	} else if (p_name == "list_patches") {
		res = _tool_list_patches(p_args);
	} else if (p_name == "pause") {
		res = _tool_pause(p_args);
	} else if (p_name == "step") {
		res = _tool_step(p_args);
	} else if (p_name == "time_scale") {
		res = _tool_time_scale(p_args);
	} else if (p_name == "screenshot") {
		res = _tool_screenshot(p_args);
	} else if (p_name == "logs") {
		res = _tool_logs(p_args);
	} else if (p_name == "dump") {
		res = _tool_dump(p_args);
	} else if (p_name == "hook_native") {
		res = _tool_hook_native(p_args);
	} else if (p_name == "unhook_native") {
		res = _tool_unhook_native(p_args);
	} else if (p_name == "list_native_hooks") {
		res = _tool_list_native_hooks(p_args);
	} else if (p_name == "reset") {
		res = _tool_reset(p_args);
	} else {
		res = _mcp_text("Unknown tool: " + p_name, true);
	}

	uint64_t elapsed = OS::get_singleton()->get_ticks_usec() - t0;
	bool is_err = res.get("isError", false);
	_write_audit_log(p_name, p_args, is_err, elapsed);
	return res;
}

void AIDebugServer::_respond_evaluation(PendingEvaluation *p_task, const Dictionary &p_result) {
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_task->rpc_id;
	response["result"] = p_result;
	if (p_task->peer.is_valid()) {
		for (int i = 0; i < clients.size(); i++) {
			if (clients[i].peer == p_task->peer) {
				_respond_json(clients.write[i], response);
				return;
			}
		}
	} else if (p_task->debugger_request_id > 0 && EngineDebugger::is_active()) {
		Array data;
		data.push_back(p_task->debugger_request_id);
		data.push_back(JSON::stringify(response));
		EngineDebugger::get_singleton()->send_message("ai_debug:rpc_response", data);
	}
}

void AIDebugServer::_poll_workers() {
	const uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
	for (List<PendingEvaluation *>::Element *entry = evaluations.front(); entry;) {
		auto *next = entry->next();
		PendingEvaluation *task = entry->get();
		Dictionary args;
		args["job_id"] = task->managed_job_id;
		Variant value = JSON::parse_string(_invoke_managed("eval_cs_poll", args));
		Dictionary poll_result;
		if (value.get_type() == Variant::DICTIONARY) {
			poll_result = value;
		}
		uint64_t elapsed = now_usec - task->start_usec;
		bool disconnected = task->peer.is_valid() && task->peer->get_status() != StreamPeerTCP::STATUS_CONNECTED;
		bool timed_out = elapsed > task->timeout_msec * 1000ULL;
		bool done = poll_result.get("done", false);
		bool failed = !bool(poll_result.get("ok", false));
		if (done || failed || timed_out || disconnected) {
			Dictionary result;
			if (done) {
				result = poll_result.get("result", Dictionary());
			} else if (failed) {
				result = poll_result;
				result["ok"] = false;
			} else {
				Variant cancelled = JSON::parse_string(_invoke_managed("eval_cs_cancel", args));
				result["ok"] = false;
				result["timed_out"] = timed_out;
				result["execution_may_continue"] = true;
				if (cancelled.get_type() == Variant::DICTIONARY) {
					Dictionary status = cancelled;
					result["execution_may_continue"] = status.get("execution_may_continue", true);
				}
				result["error"] = disconnected ? "Client disconnected; cancellation requested." : "C# evaluation response deadline exceeded. Cancellation is cooperative; user code may still be running.";
			}
			bool is_error = !bool(result.get("ok", false));
			_write_audit_log("eval_cs", task->args, is_error, elapsed);
			if (!disconnected) {
				_respond_evaluation(task, _mcp_json(result, is_error));
			}
			memdelete(task);
			evaluations.erase(entry);
		}
		entry = next;
	}
}

// ============================================================================
// Node Resolution & Variant Helpers
// ============================================================================

Node *AIDebugServer::_resolve_node(const String &p_path, String &r_error) const {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		r_error = "SceneTree or root window is not available.";
		return nullptr;
	}

	Window *root = tree->get_root();
	Node *current = tree->get_current_scene();
#ifdef TOOLS_ENABLED
	if (!current && Engine::get_singleton()->is_editor_hint()) {
		current = tree->get_edited_scene_root();
	}
#endif

	String path = p_path.strip_edges();
	if (path.is_empty() || path == ".") {
		return current ? current : (Node *)root;
	}
	if (path == "/root" || path == "root") {
		return root;
	}
	if (path.begins_with("#")) {
		String id_str = path.substr(1);
		if (id_str.is_valid_int()) {
			Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)id_str.to_int()));
			Node *n = Object::cast_to<Node>(obj);
			if (n) {
				return n;
			}
		}
		r_error = "Invalid or non-existent node ObjectID: " + path;
		return nullptr;
	}

	if (path.begins_with("/")) {
		Node *n = root->get_node_or_null(NodePath(path));
		if (n) {
			return n;
		}
	}

	if (current) {
		if (path == String(current->get_name())) {
			return current;
		}
		Node *n = current->get_node_or_null(NodePath(path));
		if (n) {
			return n;
		}
	}

	Node *from_root = root->get_node_or_null(NodePath(path));
	if (from_root) {
		return from_root;
	}

	// Fallback: search by node name recursively
	Node *found = root->find_child(path, true, false);
	if (found) {
		return found;
	}

	r_error = "Node not found: " + p_path;
	return nullptr;
}

Variant AIDebugServer::_json_to_variant(const Variant &p_val, Variant::Type p_expected_type) {
	if (p_val.get_type() == Variant::STRING) {
		String s = p_val;
		String trimmed = s.strip_edges();
		if (p_expected_type != Variant::STRING && p_expected_type != Variant::STRING_NAME && p_expected_type != Variant::NODE_PATH) {
			Variant parsed = VariantUtilityFunctions::str_to_var(trimmed);
			if (parsed.get_type() != Variant::NIL) {
				return parsed;
			}
		} else if (trimmed.begins_with("Vector") || trimmed.begins_with("Color(") || trimmed.begins_with("Rect2") || trimmed.begins_with("Transform") || trimmed.begins_with("Basis(") || trimmed.begins_with("Quaternion(")) {
			Variant parsed = VariantUtilityFunctions::str_to_var(trimmed);
			if (parsed.get_type() != Variant::NIL) {
				return parsed;
			}
		}
	}

	if (p_val.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_val;
		if (p_expected_type == Variant::COLOR || (d.has("r") && d.has("g") && d.has("b"))) {
			return Color(d.get("r", 0.0), d.get("g", 0.0), d.get("b", 0.0), d.get("a", 1.0));
		}
		if (p_expected_type == Variant::VECTOR2 || (d.size() == 2 && d.has("x") && d.has("y"))) {
			return Vector2(d.get("x", 0.0), d.get("y", 0.0));
		}
		if (p_expected_type == Variant::VECTOR2I) {
			return Vector2i(d.get("x", 0), d.get("y", 0));
		}
		if (p_expected_type == Variant::VECTOR3 || (d.size() == 3 && d.has("x") && d.has("y") && d.has("z"))) {
			return Vector3(d.get("x", 0.0), d.get("y", 0.0), d.get("z", 0.0));
		}
		if (p_expected_type == Variant::VECTOR3I) {
			return Vector3i(d.get("x", 0), d.get("y", 0), d.get("z", 0));
		}
	}

	if (p_val.get_type() == Variant::ARRAY) {
		Array a = p_val;
		if (p_expected_type == Variant::VECTOR2 && a.size() == 2) {
			return Vector2(a[0], a[1]);
		}
		if (p_expected_type == Variant::VECTOR2I && a.size() == 2) {
			return Vector2i(a[0], a[1]);
		}
		if (p_expected_type == Variant::VECTOR3 && a.size() == 3) {
			return Vector3(a[0], a[1], a[2]);
		}
		if (p_expected_type == Variant::VECTOR3I && a.size() == 3) {
			return Vector3i(a[0], a[1], a[2]);
		}
		if (p_expected_type == Variant::COLOR && (a.size() == 3 || a.size() == 4)) {
			return Color(a[0], a[1], a[2], a.size() == 4 ? (double)a[3] : 1.0);
		}
	}

	return p_val;
}

Variant AIDebugServer::_variant_to_json(const Variant &p_val, int p_depth) {
	switch (p_val.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_val;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_val);
		case Variant::VECTOR2: {
			Vector2 v = p_val;
			Dictionary d;
			d["_type"] = "Vector2";
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR2I: {
			Vector2i v = p_val;
			Dictionary d;
			d["_type"] = "Vector2i";
			d["x"] = v.x;
			d["y"] = v.y;
			return d;
		}
		case Variant::VECTOR3: {
			Vector3 v = p_val;
			Dictionary d;
			d["_type"] = "Vector3";
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::VECTOR3I: {
			Vector3i v = p_val;
			Dictionary d;
			d["_type"] = "Vector3i";
			d["x"] = v.x;
			d["y"] = v.y;
			d["z"] = v.z;
			return d;
		}
		case Variant::COLOR: {
			Color c = p_val;
			Dictionary d;
			d["_type"] = "Color";
			d["r"] = c.r;
			d["g"] = c.g;
			d["b"] = c.b;
			d["a"] = c.a;
			d["html"] = "#" + c.to_html();
			return d;
		}
		case Variant::ARRAY: {
			if (p_depth > 4) {
				return p_val.stringify();
			}
			Array a = p_val;
			Array out;
			for (int i = 0; i < MIN(a.size(), 200); i++) {
				out.push_back(_variant_to_json(a[i], p_depth + 1));
			}
			return out;
		}
		case Variant::DICTIONARY: {
			if (p_depth > 4) {
				return p_val.stringify();
			}
			Dictionary d = p_val;
			Dictionary out;
			Array keys = d.keys();
			for (int i = 0; i < keys.size(); i++) {
				out[keys[i].stringify()] = _variant_to_json(d[keys[i]], p_depth + 1);
			}
			return out;
		}
		case Variant::OBJECT: {
			Object *obj = p_val.get_validated_object();
			if (!obj) {
				return Variant();
			}
			Dictionary d;
			d["_class"] = obj->get_class();
			d["_id"] = (int64_t)obj->get_instance_id();
			if (Node *n = Object::cast_to<Node>(obj)) {
				d["name"] = String(n->get_name());
				if (n->is_inside_tree()) {
					d["path"] = String(n->get_path());
				}
			} else if (Resource *r = Object::cast_to<Resource>(obj)) {
				d["resource_path"] = r->get_path();
			}
			return d;
		}
		default:
			return VariantUtilityFunctions::var_to_str(p_val);
	}
}

// ============================================================================
// Phase 1 Tools Implementation
// ============================================================================

Dictionary AIDebugServer::_tool_info(const Dictionary &p_args) {
	Dictionary info;
	info["engine_version"] = GODOT_VERSION_FULL_NAME;
	info["engine_commit"] = String(GODOT_VERSION_HASH);
	info["project_name"] = GLOBAL_GET("application/config/name");
	info["project_path"] = ProjectSettings::get_singleton()->globalize_path("res://");

#ifdef TOOLS_ENABLED
	info["build_type"] = Engine::get_singleton()->is_editor_hint() ? "editor" : "editor_game";
#elif defined(DEBUG_ENABLED)
	info["build_type"] = "template_debug";
#else
	info["build_type"] = "template_release";
#endif

	info["pid"] = (int64_t)OS::get_singleton()->get_process_id();
	info["activated_at"] = activated_time_iso;
	info["activation_mode"] = activated_via_debugger ? "editor_debugger" : (activated_via_hotkey ? "hotkey" : "startup");
	info["port"] = listen_port;
	info["bind"] = bind_ip;
	info["time_scale"] = Engine::get_singleton()->get_time_scale();
	info["paused"] = SceneTree::get_singleton() ? SceneTree::get_singleton()->is_paused() : false;
	info["native_hooks"] = AIDebugNative::list_native_hooks().get("native_hooks", Array());

#ifdef MODULE_MONO_ENABLED
	if (GDMono::get_singleton() && GDMono::get_singleton()->is_initialized()) {
		info["jit_no_inline"] = GDMono::get_singleton()->ai_debug_is_jit_no_inline();
		String m_raw = _invoke_managed("info", Dictionary());
		Variant m_parsed = JSON::parse_string(m_raw);
		if (m_parsed.get_type() == Variant::DICTIONARY) {
			Dictionary md = m_parsed;
			info["dotnet_runtime"] = md.get("dotnet_runtime", "");
			info["project_assemblies"] = md.get("project_assemblies", Array());
			info["patches"] = md.get("patches", Array());
		} else {
			info["dotnet_runtime"] = "initialized";
			info["patches"] = Array();
		}
	} else {
		info["dotnet_runtime"] = "not_initialized";
		info["patches"] = Array();
	}
#else
	info["dotnet_runtime"] = "disabled";
	info["patches"] = Array();
#endif

	return _mcp_json(info);
}

void AIDebugServer::_walk_tree(Node *p_root, Node *p_node, int p_depth, int p_max_depth, bool p_include_props, const Vector<String> &p_prop_filter, Vector<String> &r_lines, Array &r_nodes) const {
	if (!p_node || r_lines.size() > 2500) {
		return;
	}
	if (String(p_node->get_name()).begins_with("_AIDebugOverlay") || String(p_node->get_name()).begins_with("_AIDebugInput")) {
		return;
	}

	String rel_path = (p_node == p_root) ? String(p_node->get_path()) : String(p_root->get_path_to(p_node));
	String indent = String("  ").repeat(p_depth);
	String line = vformat("%s%s (%s) [#%d]", indent, rel_path, p_node->get_class(), (int64_t)p_node->get_instance_id());

	Dictionary node_dict;
	node_dict["name"] = String(p_node->get_name());
	node_dict["path"] = String(p_node->get_path());
	node_dict["class"] = p_node->get_class();
	node_dict["id"] = (int64_t)p_node->get_instance_id();

	Ref<Script> scr = p_node->get_script();
	if (scr.is_valid()) {
		String sp = scr->get_path();
		if (!sp.is_empty()) {
			line += " script=" + sp;
			node_dict["script"] = sp;
		}
	}

	if (p_include_props || !p_prop_filter.is_empty()) {
		Dictionary props;
		Vector<String> default_summary = { "position", "global_position", "rotation_degrees", "scale", "visible", "text", "modulate", "self_modulate" };
		const Vector<String> &check_list = p_prop_filter.is_empty() ? default_summary : p_prop_filter;
		for (int i = 0; i < check_list.size(); i++) {
			const String &pname = check_list[i];
			bool valid = false;
			Variant val = p_node->get(pname, &valid);
			if (valid && val.get_type() != Variant::NIL) {
				String vs = val.stringify();
				if (vs.length() > 60) {
					vs = vs.substr(0, 60) + "...";
				}
				line += vformat(" %s=%s", pname, vs);
				props[pname] = _variant_to_json(val, 1);
			}
		}
		if (!props.is_empty()) {
			node_dict["properties"] = props;
		}
	}

	r_lines.push_back(line);

	if (p_depth >= p_max_depth) {
		int cc = p_node->get_child_count();
		if (cc > 0) {
			r_lines.push_back(vformat("%s  ... (%d children)", indent, cc));
		}
		r_nodes.push_back(node_dict);
		return;
	}

	Array children_arr;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *ch = p_node->get_child(i);
		if (String(ch->get_name()).begins_with("_AIDebugOverlay") || String(ch->get_name()).begins_with("_AIDebugInput")) {
			continue;
		}
		_walk_tree(p_root, ch, p_depth + 1, p_max_depth, p_include_props, p_prop_filter, r_lines, children_arr);
	}
	if (!children_arr.is_empty()) {
		node_dict["children"] = children_arr;
	}
	r_nodes.push_back(node_dict);
}

Dictionary AIDebugServer::_tool_tree(const Dictionary &p_args) {
	String root_path = p_args.get("root", p_args.get("path", ""));
	int max_depth = p_args.get("depth", 6);
	bool include_props = p_args.get("include_properties", p_args.get("properties_summary", false));

	Vector<String> prop_filter;
	if (p_args.has("properties") && p_args["properties"].get_type() == Variant::ARRAY) {
		Array pa = p_args["properties"];
		for (int i = 0; i < pa.size(); i++) {
			prop_filter.push_back(pa[i].stringify());
		}
	}

	String err;
	Node *start_node = nullptr;
	if (root_path.is_empty()) {
		start_node = SceneTree::get_singleton() ? (Node *)SceneTree::get_singleton()->get_root() : nullptr;
	} else {
		start_node = _resolve_node(root_path, err);
	}
	if (!start_node) {
		return _mcp_text(err.is_empty() ? "Root node not available." : err, true);
	}

	Vector<String> lines;
	Array nodes;
	_walk_tree(start_node, start_node, 0, max_depth, include_props, prop_filter, lines, nodes);

	Dictionary out;
	out["outline"] = String("\n").join(lines);
	out["tree"] = nodes;
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_get(const Dictionary &p_args) {
	String node_path = p_args.get("node", p_args.get("path", ""));
	String prop = p_args.get("property", p_args.get("name", ""));
	if (prop.is_empty()) {
		return _mcp_text("Parameter 'property' is required.", true);
	}

	String err;
	Node *node = _resolve_node(node_path, err);
	if (!node) {
		return _mcp_text(err, true);
	}

	bool valid = false;
	Variant val;
	if (prop.contains_char(':')) {
		val = node->get_indexed(NodePath(":" + prop).get_subnames(), &valid);
	} else {
		val = node->get(prop, &valid);
	}

	if (!valid) {
		return _mcp_text(vformat("Property '%s' not found on node '%s' (%s).", prop, String(node->get_path()), node->get_class()), true);
	}

	Dictionary out;
	out["ok"] = true;
	out["node"] = String(node->get_path());
	out["class"] = node->get_class();
	out["property"] = prop;
	out["type"] = Variant::get_type_name(val.get_type());
	out["value"] = _variant_to_json(val);
	out["value_str"] = VariantUtilityFunctions::var_to_str(val);
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_set(const Dictionary &p_args) {
	String node_path = p_args.get("node", p_args.get("path", ""));
	String prop = p_args.get("property", p_args.get("name", ""));
	if (prop.is_empty()) {
		return _mcp_text("Parameter 'property' is required.", true);
	}
	if (!p_args.has("value")) {
		return _mcp_text("Parameter 'value' is required.", true);
	}

	String err;
	Node *node = _resolve_node(node_path, err);
	if (!node) {
		return _mcp_text(err, true);
	}

	bool had_prev = false;
	Variant prev_val = prop.contains_char(':') ? node->get_indexed(NodePath(":" + prop).get_subnames(), &had_prev) : node->get(prop, &had_prev);
	Variant::Type expected_type = had_prev ? prev_val.get_type() : Variant::NIL;
	Variant new_val = _json_to_variant(p_args["value"], expected_type);

	bool valid = false;
	if (prop.contains_char(':')) {
		node->set_indexed(NodePath(":" + prop).get_subnames(), new_val, &valid);
	} else {
		node->set(prop, new_val, &valid);
	}

	if (!valid) {
		return _mcp_text(vformat("Failed to set property '%s' on node '%s' (%s).", prop, String(node->get_path()), node->get_class()), true);
	}

	Variant updated = prop.contains_char(':') ? node->get_indexed(NodePath(":" + prop).get_subnames()) : node->get(prop);
	Dictionary out;
	out["ok"] = true;
	out["node"] = String(node->get_path());
	out["property"] = prop;
	out["previous"] = _variant_to_json(prev_val);
	out["value"] = _variant_to_json(updated);
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_call(const Dictionary &p_args) {
	String node_path = p_args.get("node", p_args.get("path", ""));
	String method = p_args.get("method", p_args.get("name", ""));
	if (method.is_empty()) {
		return _mcp_text("Parameter 'method' is required.", true);
	}

	String err;
	Node *node = _resolve_node(node_path, err);
	if (!node) {
		return _mcp_text(err, true);
	}

	Array raw_args = p_args.get("args", p_args.get("arguments", Array()));
	Vector<Variant> converted_args;
	Vector<const Variant *> arg_ptrs;
	converted_args.resize(raw_args.size());
	arg_ptrs.resize(raw_args.size());
	for (int i = 0; i < raw_args.size(); i++) {
		converted_args.write[i] = _json_to_variant(raw_args[i]);
		arg_ptrs.write[i] = &converted_args[i];
	}

	Callable::CallError call_err;
	Variant ret = node->callp(StringName(method), arg_ptrs.ptrw(), arg_ptrs.size(), call_err);
	if (call_err.error != Callable::CallError::CALL_OK) {
		return _mcp_text(vformat("Call to '%s' on '%s' failed (error %d, argument %d).", method, String(node->get_path()), (int)call_err.error, call_err.argument), true);
	}

	Dictionary out;
	out["ok"] = true;
	out["node"] = String(node->get_path());
	out["method"] = method;
	out["result"] = _variant_to_json(ret);
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_eval_gd(const Dictionary &p_args) {
	String expr_str = p_args.get("expression", p_args.get("code", ""));
	if (expr_str.strip_edges().is_empty()) {
		return _mcp_text("Parameter 'expression' is required.", true);
	}

	String self_path = p_args.get("self", p_args.get("node", ""));
	String err;
	Node *base_node = _resolve_node(self_path, err);
	if (!base_node) {
		return _mcp_text(err, true);
	}

	Ref<Expression> expr;
	expr.instantiate();
	Error parse_err = expr->parse(expr_str);
	if (parse_err != OK) {
		return _mcp_text("GDScript Expression parse error: " + expr->get_error_text(), true);
	}

	Variant ret = expr->execute(Array(), base_node, true, false);
	if (expr->has_execute_failed()) {
		return _mcp_text("GDScript Expression execution error: " + expr->get_error_text(), true);
	}

	Dictionary out;
	out["ok"] = true;
	out["self"] = String(base_node->get_path());
	out["result"] = _variant_to_json(ret);
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_run_gd(const Dictionary &p_args) {
#ifndef MODULE_GDSCRIPT_ENABLED
	return _mcp_text("GDScript module is not enabled in this build.", true);
#else
	String code = p_args.get("code", p_args.get("source", ""));
	if (code.strip_edges().is_empty()) {
		return _mcp_text("Parameter 'code' cannot be empty.", true);
	}

	String src;
	if (code.contains("func run(") || code.contains("extends ")) {
		if (!code.contains("extends ")) {
			src = "extends RefCounted\n\n" + code;
		} else {
			src = code;
		}
	} else {
		Vector<String> lines = code.split("\n");
		String body;
		for (int i = 0; i < lines.size(); i++) {
			body += "\t" + lines[i] + "\n";
		}
		src = "extends RefCounted\n\nfunc run(ctx):\n" + body;
	}

	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code(src);
	Error err = script->reload();
	if (err != OK) {
		return _mcp_text(vformat("GDScript compilation failed (error %d).\nSource:\n%s", (int)err, src), true);
	}

	Object *obj = ClassDB::instantiate("RefCounted");
	Ref<RefCounted> ref_holder(Object::cast_to<RefCounted>(obj));
	obj->set_script(script);

	Dictionary ctx;
	if (SceneTree::get_singleton()) {
		ctx["tree"] = SceneTree::get_singleton();
		Window *root_win = SceneTree::get_singleton()->get_root();
		ctx["root"] = root_win;
		ctx["current_scene"] = SceneTree::get_singleton()->get_current_scene();
	}
	ctx["args"] = p_args.get("ctx", Dictionary());

	Callable::CallError call_err;
	Variant ctx_var = ctx;
	const Variant *arg_ptrs[1] = { &ctx_var };
	Variant ret = obj->callp(StringName("run"), arg_ptrs, 1, call_err);
	if (call_err.error == Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS) {
		ret = obj->callp(StringName("run"), nullptr, 0, call_err);
	}
	if (call_err.error != Callable::CallError::CALL_OK) {
		return _mcp_text(vformat("GDScript run(ctx) execution failed (CallError %d).", (int)call_err.error), true);
	}

	Dictionary out;
	out["ok"] = true;
	out["result"] = _variant_to_json(ret);
	return _mcp_json(out);
#endif
}

Dictionary AIDebugServer::_tool_eval_cs(int p_client_idx, const Variant &p_rpc_id, const Dictionary &p_args, bool &r_async) {
	r_async = false;
	if (p_rpc_id.get_type() == Variant::NIL) {
		return _mcp_text("eval_cs requires a JSON-RPC request id.", true);
	}
	Variant value = JSON::parse_string(_invoke_managed("eval_cs_start", p_args));
	if (value.get_type() != Variant::DICTIONARY) {
		return _mcp_text("Invalid managed evaluation response.", true);
	}
	Dictionary started = value;
	if (!bool(started.get("ok", false))) {
		return _mcp_json(started, true);
	}
	PendingEvaluation *task = memnew(PendingEvaluation);
	if (p_client_idx >= 0) {
		task->peer = clients[p_client_idx].peer;
	} else {
		task->debugger_request_id = current_debugger_request_id;
	}
	task->managed_job_id = started.get("job_id", 0);
	task->rpc_id = p_rpc_id;
	task->args = p_args;
	task->start_usec = OS::get_singleton()->get_ticks_usec();
	task->timeout_msec = CLAMP((int64_t)p_args.get("timeout_ms", 10000), (int64_t)1, (int64_t)300000);
	evaluations.push_back(task);
	r_async = true;
	return Dictionary();
}

Dictionary AIDebugServer::_tool_patch_cs(const Dictionary &p_args) {
	Dictionary call_args = p_args.duplicate();
#ifdef MODULE_MONO_ENABLED
	bool jit_no_inline = GDMono::get_singleton() && GDMono::get_singleton()->ai_debug_is_jit_no_inline();
	call_args["jit_no_inline"] = jit_no_inline;
#else
	call_args["jit_no_inline"] = false;
#endif

	String raw = _invoke_managed("patch_cs", call_args);
	Variant parsed = JSON::parse_string(raw);
	if (parsed.get_type() == Variant::DICTIONARY) {
		Dictionary d = parsed;
		bool ok = d.get("ok", false);
		if (ok) {
			modified_state = true;
		}
		return _mcp_json(d, !ok);
	}
	return _mcp_text(raw, true);
}

Dictionary AIDebugServer::_tool_unpatch(const Dictionary &p_args) {
	String raw = _invoke_managed("unpatch", p_args);
	Variant parsed = JSON::parse_string(raw);
	if (parsed.get_type() == Variant::DICTIONARY) {
		Dictionary d = parsed;
		bool ok = d.get("ok", false);
		return _mcp_json(d, !ok);
	}
	return _mcp_text(raw, true);
}

Dictionary AIDebugServer::_tool_list_patches(const Dictionary &p_args) {
	String raw = _invoke_managed("list_patches", p_args);
	Variant parsed = JSON::parse_string(raw);
	if (parsed.get_type() == Variant::DICTIONARY) {
		Dictionary d = parsed;
		bool ok = d.get("ok", false);
		return _mcp_json(d, !ok);
	}
	return _mcp_text(raw, true);
}

Dictionary AIDebugServer::_tool_pause(const Dictionary &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return _mcp_text("SceneTree is not available.", true);
	}

	bool want_pause = p_args.has("paused") ? bool(p_args["paused"]) : !tree->is_paused();
	tree->set_pause(want_pause);
	step_frames_remaining = 0;
	modified_state = true;

	Dictionary out;
	out["ok"] = true;
	out["paused"] = tree->is_paused();
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_step(const Dictionary &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return _mcp_text("SceneTree is not available.", true);
	}

	int frames = p_args.get("frames", p_args.get("count", 1));
	if (frames < 1) {
		frames = 1;
	}

	tree->set_pause(false);
	step_frames_remaining = frames;
	step_restore_pause = true;
	modified_state = true;

	Dictionary out;
	out["ok"] = true;
	out["stepping_frames"] = frames;
	out["will_pause_after"] = true;
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_time_scale(const Dictionary &p_args) {
	if (p_args.has("scale") || p_args.has("value")) {
		double scale = p_args.has("scale") ? (double)p_args["scale"] : (double)p_args["value"];
		if (scale < 0.0) {
			return _mcp_text("time_scale must be >= 0.", true);
		}
		Engine::get_singleton()->set_time_scale(scale);
		modified_state = true;
	}

	Dictionary out;
	out["ok"] = true;
	out["time_scale"] = Engine::get_singleton()->get_time_scale();
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_screenshot(const Dictionary &p_args) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return _mcp_text("SceneTree root window is not available.", true);
	}

	// Ensure latest frame is drawn before capturing viewport texture
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->draw(false, 0.0);
	}

	Ref<ViewportTexture> tex = tree->get_root()->get_texture();
	if (tex.is_null()) {
		return _mcp_text("Root viewport texture is null.", true);
	}
	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		return _mcp_text("Failed to capture image from root viewport (headless mode or empty image).", true);
	}

	int max_w = p_args.get("max_width", 0);
	if (max_w > 0 && img->get_width() > max_w) {
		int new_h = MAX(1, (int)((double)img->get_height() * (double)max_w / (double)img->get_width()));
		img->resize(max_w, new_h, Image::INTERPOLATE_BILINEAR);
	}

	String save_path = p_args.get("path", "");
	bool return_base64 = p_args.get("base64", save_path.is_empty());

	String resolved_path;
	if (!save_path.is_empty()) {
		if (save_path.begins_with("user://") || save_path.begins_with("res://")) {
			resolved_path = ProjectSettings::get_singleton()->globalize_path(save_path);
		} else {
			resolved_path = save_path;
		}
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid()) {
			da->make_dir_recursive(resolved_path.get_base_dir());
		}
		Error save_err = img->save_png(resolved_path);
		if (save_err != OK) {
			return _mcp_text(vformat("Failed to save PNG screenshot to '%s' (Error %d).", resolved_path, (int)save_err), true);
		}
	}

	Vector<uint8_t> png_buf = img->save_png_to_buffer();
	String b64 = return_base64 ? CryptoCore::b64_encode_str(png_buf.ptr(), png_buf.size()) : "";

	Dictionary meta;
	meta["ok"] = true;
	meta["width"] = img->get_width();
	meta["height"] = img->get_height();
	meta["size_bytes"] = png_buf.size();
	if (!resolved_path.is_empty()) {
		meta["path"] = resolved_path.simplify_path();
	}
	if (return_base64 && p_args.get("inline_base64_in_json", false)) {
		meta["base64"] = b64;
	}

	Array content;
	if (return_base64) {
		Dictionary img_item;
		img_item["type"] = "image";
		img_item["data"] = b64;
		img_item["mimeType"] = "image/png";
		content.push_back(img_item);
	}
	Dictionary text_item;
	text_item["type"] = "text";
	text_item["text"] = JSON::stringify(meta, "  ");
	content.push_back(text_item);

	Dictionary res;
	res["content"] = content;
	res["isError"] = false;
	return res;
}

Dictionary AIDebugServer::_tool_logs(const Dictionary &p_args) {
	int limit = p_args.get("limit", p_args.get("count", 100));
	String level = p_args.get("level", "");
	bool clear_after = p_args.get("clear", false);

	Array entries = logger ? logger->get_recent(limit, level) : Array();
	if (clear_after && logger) {
		logger->clear();
	}

	Dictionary out;
	out["ok"] = true;
	out["count"] = entries.size();
	out["logs"] = entries;
	return _mcp_json(out);
}

Dictionary AIDebugServer::_tool_dump(const Dictionary &p_args) {
	String mode = p_args.get("mode", p_args.get("type", "mini"));
	String custom_path = p_args.get("path", "");
	Dictionary res = AIDebugDump::write_minidump(mode, custom_path);
	bool ok = res.get("ok", false);
	return _mcp_json(res, !ok);
}

Dictionary AIDebugServer::_tool_reset(const Dictionary &p_args) {
	revert_all("manual_reset");
	last_reset_result["time_scale"] = Engine::get_singleton()->get_time_scale();
	last_reset_result["paused"] = SceneTree::get_singleton() ? SceneTree::get_singleton()->is_paused() : false;
	return _mcp_json(last_reset_result, !bool(last_reset_result.get("ok", false)));
}

// ============================================================================
// Native hooking tools
// ============================================================================

Dictionary AIDebugServer::_tool_hook_native(const Dictionary &p_args) {
	Dictionary res = AIDebugNative::hook_native(p_args);
	bool ok = res.get("ok", false);
	if (ok) {
		modified_state = true;
	}
	return _mcp_json(res, !ok);
}

Dictionary AIDebugServer::_tool_unhook_native(const Dictionary &p_args) {
	String id = p_args.get("id", p_args.get("symbol", "all"));
	Dictionary res = AIDebugNative::unhook_native(id);
	bool ok = res.get("ok", false);
	return _mcp_json(res, !ok);
}

Dictionary AIDebugServer::_tool_list_native_hooks(const Dictionary &p_args) {
	return _mcp_json(AIDebugNative::list_native_hooks());
}

Array AIDebugServer::_get_tool_definitions() {
	Variant v = JSON::parse_string(String::utf8(AI_DEBUG_TOOLS_JSON));
	if (v.get_type() == Variant::ARRAY) {
		return v;
	}
	return Array();
}
