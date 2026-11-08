#include "ai_debug_http.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"

bool AIDebugHTTP::try_parse(const Vector<uint8_t> &p_buffer, int &r_header_end, Request &r_request) {
	if (p_buffer.size() > MAX_REQUEST_BYTES) {
		r_request.too_big = true;
		return true;
	}
	if (r_header_end < 0) {
		for (int i = 0; i + 3 < p_buffer.size(); i++) {
			if (p_buffer[i] == '\r' && p_buffer[i + 1] == '\n' && p_buffer[i + 2] == '\r' && p_buffer[i + 3] == '\n') {
				r_header_end = i;
				break;
			}
		}
		if (r_header_end < 0) {
			r_request.too_big = p_buffer.size() > MAX_HEADER_BYTES;
			return r_request.too_big;
		}
	}
	if (r_header_end > MAX_HEADER_BYTES) {
		r_request.too_big = true;
		return true;
	}
	String header = String::utf8((const char *)p_buffer.ptr(), r_header_end);
	Vector<String> lines = header.split("\r\n");
	Vector<String> first = lines[0].split(" ");
	if (first.size() != 3 || first[2] != "HTTP/1.1") {
		r_request.bad = true;
		return true;
	}
	r_request.method = first[0].to_upper();
	r_request.path = first[1].get_slicec('?', 0);
	for (int i = 1; i < lines.size(); i++) {
		int colon = lines[i].find_char(':');
		if (colon <= 0) {
			r_request.bad = true;
			return true;
		}
		String key = lines[i].substr(0, colon).strip_edges().to_lower();
		if (r_request.headers.has(key)) {
			r_request.bad = true;
			return true;
		}
		r_request.headers[key] = lines[i].substr(colon + 1).strip_edges();
	}
	if (r_request.headers.has("transfer-encoding")) {
		r_request.bad = true; // Chunked request bodies are not supported.
		return true;
	}
	String length = r_request.headers.get("content-length", "0");
	if (length.is_empty()) {
		r_request.bad = true;
		return true;
	}
	for (int i = 0; i < length.length(); i++) {
		if (length[i] < '0' || length[i] > '9') {
			r_request.bad = true;
			return true;
		}
	}
	if (length.length() > 9 || length.to_int() > MAX_BODY_BYTES) {
		r_request.too_big = true;
		return true;
	}
	int body_size = (int)length.to_int();
	int body_start = r_header_end + 4;
	if (p_buffer.size() < body_start + body_size) {
		return false;
	}
	if (body_size > 0) {
		r_request.body = String::utf8((const char *)p_buffer.ptr() + body_start, body_size);
	}
	return true;
}

int AIDebugHTTP::check_access(const Request &p_request, const String &p_token) {
	if (p_request.bad) {
		return 400;
	}
	if (p_request.too_big) {
		return 413;
	}
	String origin = p_request.headers.get("origin", "");
	if (!origin.is_empty()) {
		bool local = false;
		const char *prefixes[] = { "http://127.0.0.1", "http://localhost", "http://[::1]", "https://127.0.0.1", "https://localhost", "https://[::1]" };
		for (const char *prefix : prefixes) {
			String base = prefix;
			if (origin == base || origin.begins_with(base + ":") || origin.begins_with(base + "/")) {
				local = true;
				break;
			}
		}
		if (!local) {
			return 403;
		}
	}
	if (p_request.path != "/mcp" && p_request.path != "/mcp/" && p_request.path != "/") {
		return 404;
	}
	if (p_request.method == "OPTIONS") {
		return 0;
	}
	String provided = p_request.headers.get("x-aidebug-token", "");
	String auth = p_request.headers.get("authorization", "");
	if (provided.is_empty() && auth.begins_with("Bearer ")) {
		provided = auth.substr(7).strip_edges();
	}
	return provided == p_token ? 0 : 403;
}

void AIDebugHTTP::clear_session_file(const String &p_token) {
	const String path = "user://ai_debug/session.json";
	if (p_token.is_empty() || !ProjectSettings::get_singleton() || !FileAccess::exists(path)) {
		return;
	}
	Variant value = JSON::parse_string(FileAccess::get_file_as_string(path));
	if (value.get_type() == Variant::DICTIONARY) {
		Dictionary session = value;
		if (String(session.get("token", "")) == p_token && (int64_t)session.get("pid", 0) == OS::get_singleton()->get_process_id()) {
			DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		}
	}
}
