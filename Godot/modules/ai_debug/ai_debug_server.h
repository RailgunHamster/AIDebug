/**************************************************************************/
/*  ai_debug_server.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "ai_debug_http.h"

#include "core/io/logger.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/list.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class CanvasLayer;
class Label;
class Node;
class PanelContainer;

class AIDebugLogger : public Logger {
public:
	struct Entry {
		uint64_t timestamp_msec = 0;
		String time_str;
		String level;
		String message;
	};

private:
	Mutex mutex;
	Vector<Entry> ring;
	int head = 0;
	int count = 0;
	int capacity = 2000;

	void _push_entry(const String &p_level, const String &p_msg);

public:
	explicit AIDebugLogger(int p_capacity = 2000);
	virtual void logv(const char *p_format, va_list p_list, bool p_err) override _PRINTF_FORMAT_ATTRIBUTE_2_0;
	virtual void log_error(const char *p_function, const char *p_file, int p_line, const char *p_code, const char *p_rationale, bool p_editor_notify = false, ErrorType p_type = ERR_ERROR, const Vector<Ref<ScriptBacktrace>> &p_script_backtraces = {}) override;

	Array get_recent(int p_limit, const String &p_level_filter = "");
	void clear();
};

class AIDebugServer : public Object {
	GDCLASS(AIDebugServer, Object);

private:
	static AIDebugServer *singleton;

	struct ClientConn {
		Ref<StreamPeerTCP> peer;
		Vector<uint8_t> buf;
		int head_end = -1;
		uint64_t born_msec = 0;
		uint64_t busy_since_msec = 0;
		uint64_t close_at_msec = 0;
		bool busy = false;
		Variant rpc_id;
		String rpc_tool;
	};

	using HttpRequest = AIDebugHTTP::Request;

	struct PendingEvaluation {
		Ref<StreamPeerTCP> peer; // Stable identity, independent of clients vector compaction.
		int64_t debugger_request_id = 0;
		int64_t managed_job_id = 0;
		Variant rpc_id;
		Dictionary args;
		uint64_t start_usec = 0;
		uint64_t timeout_msec = 10000;
	};

	bool active = false;
	bool activated_via_hotkey = false;
	bool activated_via_debugger = false;
	bool debugger_capture_registered = false;
	bool hotkey_pressed_prev = false;
	bool managed_loaded = false;

	int listen_port = 7651;
	String bind_ip = "127.0.0.1";
	String token;
	String session_id;
	String activated_time_iso;
	uint64_t activated_ticks_msec = 0;
	uint64_t last_request_ticks_msec = 0;
	uint64_t auto_revert_timeout_msec = 30ULL * 60ULL * 1000ULL; // 30 minutes default
	int max_code_bytes = 256 * 1024; // 256 KB default

	bool has_ever_connected = false;
	bool modified_state = false;
	double initial_time_scale = 1.0;
	bool initial_paused = false;

	// Step frame state
	int step_frames_remaining = 0;
	bool step_restore_pause = false;

	// Overlay state
	bool show_overlay = true;
	ObjectID overlay_layer_id;
	ObjectID overlay_label_id;

	Ref<TCPServer> tcp_server;
	Vector<ClientConn> clients;
	AIDebugLogger *logger = nullptr;

	List<PendingEvaluation *> evaluations;
	int64_t current_debugger_request_id = 0;
	Dictionary last_reset_result;
	void _respond_evaluation(PendingEvaluation *p_task, const Dictionary &p_result);
	static Error _debugger_capture_static(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured);
	Error _on_debugger_message(const String &p_msg, const Array &p_args, bool &r_captured);

	void _check_startup_activation();
	void _check_hotkey();
	void _ensure_debugger_capture();
	void _activate_for_debugger();
	void _poll_network();
	void _poll_workers();
	void _update_stepping();
	void _update_overlay();
	void _remove_overlay();

	bool _try_parse_http(ClientConn &c, HttpRequest &r_req);
	void _serve_http(int p_client_idx, const HttpRequest &p_req);
	void _serve_post(int p_client_idx, const String &p_body);
	Variant _dispatch_rpc(int p_client_idx, const Variant &p_msg, bool &r_async);

	void _respond_json(ClientConn &c, const Variant &p_data);
	void _respond_raw(ClientConn &c, int p_code, const String &p_body, const Dictionary &p_headers = Dictionary());

	Dictionary _execute_tool(int p_client_idx, const Variant &p_rpc_id, const String &p_name, const Dictionary &p_args, bool &r_async);
	void _write_audit_log(const String &p_tool, const Dictionary &p_args, bool p_is_error, uint64_t p_elapsed_usec, const String &p_note = "");

	// Tool implementations (Phase 1)
	Dictionary _tool_info(const Dictionary &p_args);
	Dictionary _tool_tree(const Dictionary &p_args);
	Dictionary _tool_get(const Dictionary &p_args);
	Dictionary _tool_set(const Dictionary &p_args);
	Dictionary _tool_call(const Dictionary &p_args);
	Dictionary _tool_eval_gd(const Dictionary &p_args);
	Dictionary _tool_run_gd(const Dictionary &p_args);
	Dictionary _tool_eval_cs(int p_client_idx, const Variant &p_rpc_id, const Dictionary &p_args, bool &r_async);
	Dictionary _tool_patch_cs(const Dictionary &p_args);
	Dictionary _tool_unpatch(const Dictionary &p_args);
	Dictionary _tool_list_patches(const Dictionary &p_args);
	Dictionary _tool_pause(const Dictionary &p_args);
	Dictionary _tool_step(const Dictionary &p_args);
	Dictionary _tool_time_scale(const Dictionary &p_args);
	Dictionary _tool_screenshot(const Dictionary &p_args);
	Dictionary _tool_logs(const Dictionary &p_args);
	Dictionary _tool_dump(const Dictionary &p_args);
	Dictionary _tool_reset(const Dictionary &p_args);

	// Tool implementations (Phase 2)
	Dictionary _tool_hook_native(const Dictionary &p_args);
	Dictionary _tool_unhook_native(const Dictionary &p_args);
	Dictionary _tool_list_native_hooks(const Dictionary &p_args);

	// Helpers
	Node *_resolve_node(const String &p_path, String &r_error) const;
	void _walk_tree(Node *p_root, Node *p_node, int p_depth, int p_max_depth, bool p_include_props, const Vector<String> &p_prop_filter, Vector<String> &r_lines, Array &r_nodes) const;
	static Variant _json_to_variant(const Variant &p_val, Variant::Type p_expected_type = Variant::NIL);
	static Variant _variant_to_json(const Variant &p_val, int p_depth = 0);
	static Dictionary _mcp_text(const String &p_text, bool p_is_error = false);
	static Dictionary _mcp_json(const Variant &p_data, bool p_is_error = false);
	static Array _get_tool_definitions();
	String _invoke_managed(const String &p_cmd, const Dictionary &p_args);

public:
	static AIDebugServer *get_singleton() { return singleton; }
	static void on_idle_frame();

	bool activate(int p_port = 7651, const String &p_bind_ip = "127.0.0.1", const String &p_token = "", bool p_from_hotkey = false);
	void deactivate();
	void revert_all(const String &p_reason);

	bool is_active() const { return active; }
	int get_port() const { return listen_port; }
	String get_token() const { return token; }

	void poll();

	AIDebugServer();
	~AIDebugServer();
};
