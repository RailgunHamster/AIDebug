#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Shared by the direct runtime endpoint and the editor debugger bridge.
class AIDebugHTTP {
public:
	static constexpr int MAX_HEADER_BYTES = 16 * 1024;
	static constexpr int MAX_BODY_BYTES = 16 * 1024 * 1024;
	static constexpr int MAX_REQUEST_BYTES = MAX_HEADER_BYTES + MAX_BODY_BYTES;
	static constexpr int MAX_CLIENTS = 32;
	static constexpr uint64_t REQUEST_TIMEOUT_MSEC = 30000;

	struct Request {
		bool bad = false;
		bool too_big = false;
		String method;
		String path;
		Dictionary headers;
		String body;
	};

	static bool try_parse(const Vector<uint8_t> &p_buffer, int &r_header_end, Request &r_request);
	static int check_access(const Request &p_request, const String &p_token);
	static void clear_session_file(const String &p_token);
};
