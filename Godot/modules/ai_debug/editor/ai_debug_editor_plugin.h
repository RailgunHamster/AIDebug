/**************************************************************************/
/*  ai_debug_editor_plugin.h                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "editor/debugger/editor_debugger_plugin.h"

class AIDebugEditorDebuggerPlugin : public EditorDebuggerPlugin {
	GDCLASS(AIDebugEditorDebuggerPlugin, EditorDebuggerPlugin);

private:
	static AIDebugEditorDebuggerPlugin *singleton;

	struct BridgeClient {
		Ref<StreamPeerTCP> peer;
		Vector<uint8_t> buf;
		int head_end = -1;
		uint64_t born_msec = 0;
		uint64_t busy_since_msec = 0;
		uint64_t close_at_msec = 0;
		bool busy = false;
		int64_t pending_req_id = 0;
	};

	Ref<TCPServer> tcp_server;
	Vector<BridgeClient> clients;
	int listen_port = 7651;
	String token;
	String session_id;
	int64_t next_req_id = 1;
	int active_session_id = -1;

	void _on_session_started(int p_session_id);
	void _on_session_stopped(int p_session_id);
	void _ensure_listening();
	void _stop_listening();
	Ref<EditorDebuggerSession> _get_active_session();

	void _respond_raw(BridgeClient &c, int p_code, const String &p_body, const Dictionary &p_headers = Dictionary());
	void _respond_json_str(BridgeClient &c, const String &p_json_str);

public:
	static AIDebugEditorDebuggerPlugin *get_singleton() { return singleton; }

	virtual void setup_session(int p_session_id) override;
	virtual bool has_capture(const String &p_capture) const override;
	virtual bool capture(const String &p_message, const Array &p_data, int p_session_id) override;

	void poll();

	AIDebugEditorDebuggerPlugin();
	~AIDebugEditorDebuggerPlugin();
};

#endif // TOOLS_ENABLED
