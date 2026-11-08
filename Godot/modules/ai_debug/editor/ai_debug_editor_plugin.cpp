/**************************************************************************/
/*  ai_debug_editor_plugin.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "ai_debug_editor_plugin.h"

#include "../ai_debug_http.h"

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"

AIDebugEditorDebuggerPlugin *AIDebugEditorDebuggerPlugin::singleton = nullptr;

static String _gen_hex_token(int p_bytes) {
	Vector<uint8_t> buf;
	buf.resize(p_bytes);
	if (CryptoCore::generate_random(buf.ptrw(), p_bytes) != OK) {
		uint64_t t = OS::get_singleton()->get_ticks_usec();
		for (int i = 0; i < p_bytes; i++) {
			buf.write[i] = (uint8_t)((t >> ((i % 8) * 8)) ^ (i * 41));
		}
	}
	return String::hex_encode_buffer(buf.ptr(), p_bytes);
}

AIDebugEditorDebuggerPlugin::AIDebugEditorDebuggerPlugin() {
	singleton = this;
	token = OS::get_singleton()->get_environment("GODOT_AI_DEBUG_TOKEN").strip_edges();
	if (token.is_empty()) {
		token = _gen_hex_token(8);
	}
	session_id = _gen_hex_token(8);
}

AIDebugEditorDebuggerPlugin::~AIDebugEditorDebuggerPlugin() {
	_stop_listening();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void AIDebugEditorDebuggerPlugin::setup_session(int p_session_id) {
	Ref<EditorDebuggerSession> sess = get_session(p_session_id);
	if (sess.is_null()) {
		return;
	}
	sess->connect("started", callable_mp(this, &AIDebugEditorDebuggerPlugin::_on_session_started).bind(p_session_id));
	sess->connect("stopped", callable_mp(this, &AIDebugEditorDebuggerPlugin::_on_session_stopped).bind(p_session_id));
}

void AIDebugEditorDebuggerPlugin::_on_session_started(int p_session_id) {
	active_session_id = p_session_id;
	_ensure_listening();
}

void AIDebugEditorDebuggerPlugin::_on_session_stopped(int p_session_id) {
	if (active_session_id == p_session_id) {
		active_session_id = -1;
	}
	// Check if any other session is still active
	Array all = get_sessions();
	for (int i = 0; i < all.size(); i++) {
		Ref<EditorDebuggerSession> s = all[i];
		if (s.is_valid() && s->is_active()) {
			active_session_id = i;
			return;
		}
	}
	_stop_listening();
}

Ref<EditorDebuggerSession> AIDebugEditorDebuggerPlugin::_get_active_session() {
	if (active_session_id >= 0) {
		Ref<EditorDebuggerSession> s = get_session(active_session_id);
		if (s.is_valid() && s->is_active()) {
			return s;
		}
	}
	Array all = get_sessions();
	for (int i = 0; i < all.size(); i++) {
		Ref<EditorDebuggerSession> s = all[i];
		if (s.is_valid() && s->is_active()) {
			active_session_id = i;
			return s;
		}
	}
	return Ref<EditorDebuggerSession>();
}

void AIDebugEditorDebuggerPlugin::_ensure_listening() {
	if (tcp_server.is_valid() && tcp_server->is_listening()) {
		return;
	}

	tcp_server.instantiate();
	Error err = tcp_server->listen((uint16_t)listen_port, IPAddress("127.0.0.1"));
	if (err != OK) {
		// Port 7651 may already be bound directly by the game if launched with --ai-debug; that's fine.
		tcp_server.unref();
		return;
	}

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
		sess["bind"] = "127.0.0.1";
		sess["token"] = token;
		sess["mode"] = "editor_bridge";
		sess["url"] = vformat("http://127.0.0.1:%d/mcp", listen_port);
		sess["activated_at"] = Time::get_singleton()->get_datetime_string_from_system(false, true);
		fa->store_line(JSON::stringify(sess));
	}

	print_line(vformat("[AI Debug] Editor debugger bridge listening on http://127.0.0.1:%d/mcp | Token: %s", listen_port, token));
}

void AIDebugEditorDebuggerPlugin::_stop_listening() {
	if (tcp_server.is_valid() && tcp_server->is_listening()) {
		AIDebugHTTP::clear_session_file(token);
	}
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
}

bool AIDebugEditorDebuggerPlugin::has_capture(const String &p_capture) const {
	return p_capture == "ai_debug";
}

bool AIDebugEditorDebuggerPlugin::capture(const String &p_message, const Array &p_data, int p_session_id) {
	if (p_message == "ai_debug:hello") {
		active_session_id = p_session_id;
		_ensure_listening();
		return true;
	}

	if (p_message == "ai_debug:rpc_response" && p_data.size() >= 2) {
		int64_t req_id = p_data[0];
		String json_str = p_data[1];
		for (int i = 0; i < clients.size(); i++) {
			if (clients[i].pending_req_id == req_id) {
				clients.write[i].pending_req_id = 0;
				if (json_str.is_empty()) {
					_respond_raw(clients.write[i], 202, "");
				} else {
					_respond_json_str(clients.write[i], json_str);
				}
				break;
			}
		}
		return true;
	}

	return false;
}

void AIDebugEditorDebuggerPlugin::poll() {
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
			BridgeClient c;
			c.peer = peer;
			c.born_msec = now;
			clients.push_back(c);
		}
	}

	for (int i = clients.size() - 1; i >= 0; i--) {
		BridgeClient &c = clients.write[i];
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
			if (now - c.busy_since_msec > 310000) {
				Dictionary err;
				err["jsonrpc"] = "2.0";
				err["id"] = Variant();
				Dictionary eobj;
				eobj["code"] = -32603;
				eobj["message"] = "Editor debugger bridge timed out waiting for game response.";
				err["error"] = eobj;
				_respond_json_str(c, JSON::stringify(err));
			}
			continue;
		}

		int avail = peer->get_available_bytes();
		if (avail > AIDebugHTTP::MAX_REQUEST_BYTES - c.buf.size()) {
			_respond_raw(c, 413, "Payload Too Large");
			continue;
		}
		if (avail > 0) {
			int old_sz = c.buf.size();
			c.buf.resize(old_sz + avail);
			int got = 0;
			if (peer->get_partial_data(c.buf.ptrw() + old_sz, avail, got) == OK && got > 0) {
				c.buf.resize(old_sz + got);
			} else {
				c.buf.resize(old_sz);
			}
		}

		AIDebugHTTP::Request request;
		if (!AIDebugHTTP::try_parse(c.buf, c.head_end, request)) {
			if (now - c.born_msec > AIDebugHTTP::REQUEST_TIMEOUT_MSEC) {
				peer->disconnect_from_host();
				clients.remove_at(i);
			}
			continue;
		}
		c.busy = true;
		c.busy_since_msec = now;
		int access_error = AIDebugHTTP::check_access(request, token);
		if (access_error != 0) {
			_respond_raw(c, access_error, "Request rejected");
			continue;
		}
		String method = request.method;
		String body = request.body;
		if (method == "OPTIONS") {
			Dictionary headers;
			headers["Allow"] = "POST, GET, DELETE, OPTIONS";
			_respond_raw(c, 204, "", headers);
			continue;
		}

		Ref<EditorDebuggerSession> sess = _get_active_session();
		if (sess.is_null()) {
			_respond_raw(c, 500, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32002,\"message\":\"No active debuggable game session in editor.\"}}");
			continue;
		}

		if (method == "DELETE") {
			sess->send_message("ai_debug:disconnect", Array());
			_respond_raw(c, 200, "");
			continue;
		}

		if (method != "POST") {
			Dictionary h;
			h["Allow"] = "POST, DELETE, OPTIONS";
			_respond_raw(c, 405, "", h);
			continue;
		}

		int64_t req_id = next_req_id++;
		c.pending_req_id = req_id;
		Array args;
		args.push_back(req_id);
		args.push_back(body);
		sess->send_message("ai_debug:rpc", args);
	}
}

void AIDebugEditorDebuggerPlugin::_respond_json_str(BridgeClient &c, const String &p_json_str) {
	Dictionary h;
	h["Content-Type"] = "application/json; charset=utf-8";
	h["Mcp-Session-Id"] = session_id;
	_respond_raw(c, 200, p_json_str, h);
}

void AIDebugEditorDebuggerPlugin::_respond_raw(BridgeClient &c, int p_code, const String &p_body, const Dictionary &p_headers) {
	if (c.peer.is_null()) {
		return;
	}
	const char *status_text = (p_code == 200) ? "OK" : (p_code == 202) ? "Accepted"
			: (p_code == 204)										   ? "No Content"
			: (p_code == 403)										   ? "Forbidden"
			: (p_code == 404)										   ? "Not Found"
																	   : "Error";
	CharString body_utf8 = p_body.utf8();
	int body_len = body_utf8.length();

	String head = vformat("HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n", p_code, status_text, body_len);
	Array keys = p_headers.keys();
	for (int i = 0; i < keys.size(); i++) {
		String k = keys[i];
		head += k + ": " + String(p_headers[k]) + "\r\n";
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

#endif // TOOLS_ENABLED
