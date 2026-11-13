// Minimal HTTPS client for RetroAchievements requests.
// OpenSSL is loaded at runtime from the firmware (libssl.so.1.1) so builds don't need its headers
// and devices without it simply report the request as failed.

#define _GNU_SOURCE // strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "ra_http.h"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

static struct {
	int loaded;
	int (*init_ssl)(uint64_t opts, const void* settings);
	const SSL_METHOD* (*client_method)(void);
	SSL_CTX* (*ctx_new)(const SSL_METHOD* method);
	void (*ctx_free)(SSL_CTX* ctx);
	int (*ctx_load_verify_locations)(SSL_CTX* ctx, const char* file, const char* path);
	void (*ctx_set_verify)(SSL_CTX* ctx, int mode, void* callback);
	SSL* (*ssl_new)(SSL_CTX* ctx);
	void (*ssl_free)(SSL* ssl);
	int (*set_fd)(SSL* ssl, int fd);
	long (*ctrl)(SSL* ssl, int cmd, long larg, void* parg);
	int (*set1_host)(SSL* ssl, const char* hostname);
	int (*connect)(SSL* ssl);
	int (*read)(SSL* ssl, void* buf, int num);
	int (*write)(SSL* ssl, const void* buf, int num);
	int (*shutdown)(SSL* ssl);
	long (*get_verify_result)(const SSL* ssl);
} ssl;

#define SSL_VERIFY_PEER 1
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0
#define X509_V_OK 0

// once for every request (they run on threads of their own, several at a game's start), under this lock: a second
// request mustn't find it half loaded and give up
static pthread_mutex_t ssl_mx = PTHREAD_MUTEX_INITIALIZER;
static int loadSSL(void) {
	if (ssl.loaded) return ssl.loaded==1;
	ssl.loaded = -1;
	// RA_LIBCRYPTO/RA_LIBSSL let off-device test builds point at a different OpenSSL (the calls used exist in 1.1 and 3.x)
	char* crypto_name = getenv("RA_LIBCRYPTO");
	char* ssl_name = getenv("RA_LIBSSL");
	void* crypto = dlopen(crypto_name ? crypto_name : "libcrypto.so.1.1", RTLD_NOW|RTLD_GLOBAL);
	void* lib = dlopen(ssl_name ? ssl_name : "libssl.so.1.1", RTLD_NOW);
	if (!crypto || !lib) {
		fprintf(stderr, "RetroAchievements: no TLS library: %s\n", dlerror());
		return 0;
	}

	#define LOAD(field, name) if (!(*(void**)&ssl.field = dlsym(lib, name))) return 0
	LOAD(init_ssl, "OPENSSL_init_ssl");
	LOAD(client_method, "TLS_client_method");
	LOAD(ctx_new, "SSL_CTX_new");
	LOAD(ctx_free, "SSL_CTX_free");
	LOAD(ctx_load_verify_locations, "SSL_CTX_load_verify_locations");
	LOAD(ctx_set_verify, "SSL_CTX_set_verify");
	LOAD(ssl_new, "SSL_new");
	LOAD(ssl_free, "SSL_free");
	LOAD(set_fd, "SSL_set_fd");
	LOAD(ctrl, "SSL_ctrl");
	LOAD(set1_host, "SSL_set1_host");
	LOAD(connect, "SSL_connect");
	LOAD(read, "SSL_read");
	LOAD(write, "SSL_write");
	LOAD(shutdown, "SSL_shutdown");
	LOAD(get_verify_result, "SSL_get_verify_result");
	#undef LOAD

	ssl.init_ssl(0, NULL);
	ssl.loaded = 1;
	return 1;
}
// the certificates, read once: a few hundred of them parsed for every request is slow on the A30. An SSL_CTX is safe
// to share between threads once it's set up
static SSL_CTX* shared_ctx = NULL;
static SSL_CTX* getContext(const char* ca_path) {
	pthread_mutex_lock(&ssl_mx);
	if (!shared_ctx && loadSSL()) {
		SSL_CTX* ctx = ssl.ctx_new(ssl.client_method());
		if (ctx && ssl.ctx_load_verify_locations(ctx, ca_path, NULL)) { // no certificates means no trust
			ssl.ctx_set_verify(ctx, SSL_VERIFY_PEER, NULL);
			shared_ctx = ctx;
		}
		else if (ctx) ssl.ctx_free(ctx);
	}
	SSL_CTX* ctx = shared_ctx;
	pthread_mutex_unlock(&ssl_mx);
	return ctx;
}

static int openSocket(const char* host, int port) {
	char port_str[8];
	sprintf(port_str, "%i", port);
	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* result;
	if (getaddrinfo(host, port_str, &hints, &result)!=0) return -1;

	int fd = -1;
	for (struct addrinfo* info=result; info; info=info->ai_next) {
		fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
		if (fd<0) continue;
		struct timeval timeout = { 15, 0 }; // never hang a game on a dead connection
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		if (connect(fd, info->ai_addr, info->ai_addrlen)==0) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(result);
	return fd;
}

// decodes a chunked body in place, returns the new length
// complete is set only when the closing zero-size chunk arrived, so a dropped connection isn't mistaken for the whole reply
static size_t dechunk(char* body, size_t len, int* complete) {
	char* in = body;
	char* out = body;
	char* end = body + len;
	*complete = 0;
	while (in<end) {
		size_t size = strtoul(in, NULL, 16);
		char* data = strstr(in, "\r\n");
		if (!data) break;
		if (size==0) {
			*complete = 1;
			break;
		}
		data += 2;
		if (data+size>end) {
			size = end - data;
			memmove(out, data, size);
			out += size;
			break; // cut off mid-chunk
		}
		memmove(out, data, size);
		out += size;
		in = data + size + 2;
	}
	*out = '\0';
	return out - body;
}

int HTTP_request(const char* url, const char* post_data, const char* content_type, const char* user_agent, const char* ca_path, HTTP_Response* response) {
	memset(response, 0, sizeof(HTTP_Response));
	if (strncmp(url, "https://", 8)!=0) return 0;
	SSL_CTX* ctx = getContext(ca_path);
	if (!ctx) return 0;

	char host[256];
	const char* host_start = url + 8;
	const char* slash = strchr(host_start, '/');
	const char* path = slash ? slash : "/";
	size_t host_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
	if (host_len>=sizeof(host)) return 0;
	memcpy(host, host_start, host_len);
	host[host_len] = '\0';

	int fd = openSocket(host, 443);
	if (fd<0) {
		fprintf(stderr, "RetroAchievements: can't reach %s (no network?)\n", host);
		return 0;
	}

	int ok = 0;
	SSL* conn = NULL;
	char* raw = NULL;
	conn = ssl.ssl_new(ctx);
	if (!conn) goto done;
	ssl.set_fd(conn, fd);
	ssl.ctrl(conn, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, host); // SNI
	ssl.set1_host(conn, host); // check the certificate is for this host
	if (ssl.connect(conn)!=1 || ssl.get_verify_result(conn)!=X509_V_OK) {
		fprintf(stderr, "RetroAchievements: secure connection to %s failed (is the clock right?)\n", host);
		goto done;
	}

	size_t post_len = post_data ? strlen(post_data) : 0;
	char header[1024];
	int header_len;
	if (post_data) header_len = snprintf(header, sizeof(header),
		"POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		path, host, user_agent, content_type ? content_type : "application/x-www-form-urlencoded", post_len);
	else header_len = snprintf(header, sizeof(header),
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\n\r\n", path, host, user_agent);
	if (ssl.write(conn, header, header_len)!=header_len) goto done;
	if (post_len && ssl.write(conn, post_data, post_len)!=(int)post_len) goto done;

	size_t capacity = 16384, len = 0;
	raw = malloc(capacity);
	if (!raw) goto done;
	while (1) {
		if (len+4096+1>capacity) {
			capacity *= 2;
			char* bigger = realloc(raw, capacity);
			if (!bigger) goto done;
			raw = bigger;
		}
		int n = ssl.read(conn, raw+len, 4096);
		if (n<=0) break;
		len += n;
	}
	raw[len] = '\0';

	char* body = strstr(raw, "\r\n\r\n");
	if (!body || sscanf(raw, "HTTP/%*s %d", &response->status)!=1) goto done;
	*body = '\0';
	int chunked = strcasestr(raw, "transfer-encoding: chunked")!=NULL;
	char* length_header = strcasestr(raw, "\r\ncontent-length:");
	body += 4;
	size_t body_len = len - (body - raw);
	int complete = 1;
	if (chunked) body_len = dechunk(body, body_len, &complete);
	else if (length_header) complete = body_len >= strtoul(length_header+17, NULL, 10);
	if (!complete) {
		// the connection dropped partway (a Wi-Fi blip): half a reply would fail to parse and, cached, break offline play
		fprintf(stderr, "RetroAchievements: the reply from %s was cut off\n", host);
		goto done;
	}

	response->body = malloc(body_len+1);
	if (!response->body) goto done;
	memcpy(response->body, body, body_len);
	response->body[body_len] = '\0';
	response->body_length = body_len;
	ok = 1;

done:
	if (raw) free(raw);
	if (conn) {
		ssl.shutdown(conn);
		ssl.ssl_free(conn);
	}
	close(fd);
	return ok;
}

void HTTP_freeResponse(HTTP_Response* response) {
	if (response->body) free(response->body);
	response->body = NULL;
}
