#ifndef RA_HTTP_H
#define RA_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct HTTP_Response {
	int status; // HTTP status code
	char* body; // null terminated
	size_t body_length;
} HTTP_Response;

// blocking; returns 1 when a response was received (check status), 0 on connection/TLS failure
int HTTP_request(const char* url, const char* post_data, const char* content_type, const char* user_agent, const char* ca_path, HTTP_Response* response);
void HTTP_freeResponse(HTTP_Response* response);

#endif
