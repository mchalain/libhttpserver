/*****************************************************************************
 * httpserver.c: Simple HTTP server
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#else
# define strcasestr strstr
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef WIN32
# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <netdb.h>

#else

# include <winsock2.h>
# include <ws2tcpip.h>

# define SHUT_RDWR SD_BOTH
# pragma comment (lib, "Ws2_32.lib")

#ifdef __cplusplus
extern "C" {
#endif
   void WSAAPI freeaddrinfo( struct addrinfo* );

   int WSAAPI getaddrinfo( const char*, const char*, const struct addrinfo*,
                 struct addrinfo** );

   int WSAAPI getnameinfo( const struct sockaddr*, socklen_t, char*, DWORD,
                char*, DWORD, int );
#ifdef __cplusplus
}
#endif

#endif

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

#include "valloc.h"
#include "vthread.h"
#include "dbentry.h"
#include "httpserver.h"

#define CHUNKSIZE 64

typedef struct buffer_s
{
	char *data;
	char *offset;
	int size;
	int length;
	int maxchunks;
} buffer_t;

struct http_connector_list_s
{
	char *vhost;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
};
typedef struct http_connector_list_s http_connector_list_t;

typedef struct http_server_mod_s http_server_mod_t;
struct http_server_mod_s
{
	void *arg;
	http_getctx_t func;
	http_freectx_t freectx;
	http_server_mod_t *next;
};

typedef struct http_client_modctx_s http_client_modctx_t;
struct http_client_modctx_s
{
	void *ctx;
	http_freectx_t freectx;
	http_client_modctx_t *next;
};

#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
#define CLIENT_NONBLOCK 0x1000
#define CLIENT_ERROR 0x2000
#define CLIENT_RESPONSEREADY 0x4000
#define CLIENT_KEEPALIVE 0x8000
#define CLIENT_MACHINEMASK 0x00FF
#define CLIENT_NEW 0x0000
#define CLIENT_REQUEST 0x0001
#define CLIENT_PUSHREQUEST 0x0002
#define CLIENT_PARSER1 0x0003
#define CLIENT_PARSER2 0x0004
#define CLIENT_RESPONSEHEADER 0x0005
#define CLIENT_RESPONSECONTENT 0x0006
#define CLIENT_PARSERERROR 0x0007
#define CLIENT_COMPLETE 0x0008
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server; /* the server which create the client */
	vthread_t thread; /* The thread of socket management during the live of the connection */

	http_recv_t recvreq; /* callback to receive data on the socket */
	http_send_t sendresp; /* callback to send data on the socket */
	void *ctx; /* ctx of recvreq and sendresp functions */

	http_connector_list_t *callbacks;
	http_connector_list_t *callback;
	http_message_t *request;
	http_message_t *request_queue;

	http_client_modctx_t *modctx; /* list of pointers returned by getctx of each mod */

	dbentry_t *session;
	buffer_t *session_storage;
#ifndef WIN32
	struct sockaddr_un addr;
#elif defined IPV6
	struct sockaddr_in6 addr;
#else
	struct sockaddr_in addr;
#endif
	unsigned int addr_size;
	struct http_client_s *next;
};
typedef struct http_client_s http_client_t;

struct http_message_s
{
	http_message_result_e result;
	int keepalive;
	http_client_t *client;
	http_message_t *response;
	http_connector_list_t *connector;
	enum
	{
		MESSAGE_TYPE_GET,
		MESSAGE_TYPE_POST,
		MESSAGE_TYPE_HEAD,
		MESSAGE_TYPE_PUT,
		MESSAGE_TYPE_DELETE,
	} type;
	enum
	{
		PARSE_INIT,
		PARSE_URI,
		PARSE_VERSION,
		PARSE_STATUS,
		PARSE_HEADER,
		PARSE_HEADERNEXT,
		PARSE_CONTENT,
		PARSE_END,
		PARSE_MASK = 0x00FF,
		PARSE_CONTINUE = 0x0100,
	} state;
	buffer_t *content;
	int content_length;
	buffer_t *uri;
	char *query;
	http_message_version_e version;
	buffer_t *headers_storage;
	dbentry_t *headers;
	void *private;
	http_message_t *next;
};

struct http_server_s
{
	int sock;
	int run;
	vthread_t thread;
	http_client_t *clients;
	http_connector_list_t *callbacks;
	http_server_config_t *config;
	http_server_mod_t *mod;
};

static int _httpserver_start(http_server_t *server);
static void _httpmessage_fillheaderdb(http_message_t *message);
static void _httpmessage_addheader(http_message_t *message, char *key, char *value);
static int _httpclient_run(http_client_t *client);

/********************************************************************/
static http_server_config_t defaultconfig = {
	.addr = NULL,
	.port = 80,
	.maxclients = 10,
	.chunksize = 64,
	.keepalive = 1,
	.version = HTTP10,
};

static const char *_http_message_result[] =
{
	" 200 OK",
	" 400 Bad Request",
	" 404 File Not Found",
	" 405 Method Not Allowed",
#ifndef HTTP_STATUS_PARTIAL
	" 301 Moved Permanently",
	" 302 Found",
	" 304 Not Modified",
	" 401 Unauthorized",
	" 414 Request URI too long",
	" 505 HTTP Version Not Supported",
	" 511 Network Authentication Required",
#endif
};

static char *_http_message_version[] =
{
	"HTTP/0.9",
	"HTTP/1.0",
	"HTTP/1.1",
	"HTTP/2",
};
static char _httpserver_software[] = "libhttpserver";
char *httpserver_software = _httpserver_software;
static const char str_connection[] = "Connection";
static const char str_contenttype[] = "Content-Type";
static const char str_contentlength[] = "Content-Length";
/********************************************************************/
#define BUFFERMAX 2048
static int ChunkSize = 0;
static buffer_t * _buffer_create(int nbchunks, int chunksize)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	/**
	 * nbchunks is unused here, because is it possible to realloc.
	 * Embeded version may use the nbchunk with special vcalloc.
	 * The idea is to create a pool of chunks into the stack.
	 */
	buffer->data = vcalloc(1, chunksize);
	/**
	 * the chunksize has to be constant during the life of the application.
	 * Two ways are available:
	 *  - to store the chunksize into each buffer (takes a lot of place).
	 *  - to store into a global variable (looks bad).
	 */
	if (ChunkSize == 0)
		ChunkSize = chunksize;
	buffer->maxchunks = nbchunks;
	buffer->size = chunksize;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, char *data, int length)
{
	if (buffer->data + buffer->size <= buffer->offset + length + 1)
	{
		char *data = buffer->data;
		int nbchunks = length / ChunkSize + 1;
		if (buffer->maxchunks - nbchunks < 0)
			nbchunks = buffer->maxchunks;
		int chunksize = ChunkSize * nbchunks;
		if (nbchunks > 0)
		{
			buffer->maxchunks -= nbchunks;
			data = vrealloc(buffer->data, buffer->size + chunksize);
			if ((data == NULL && errno == ENOMEM) || (buffer->size + chunksize) > BUFFERMAX)
			{
				warn("buffer max: %d / %d", buffer->size + chunksize, BUFFERMAX);
				return NULL;
			}
			buffer->size += chunksize;
			if (data != buffer->data)
			{
				char *offset = buffer->offset;
				buffer->offset = data + (offset - buffer->data);
				buffer->data = data;
			}
		}
		length = (length > chunksize)? chunksize: length;
	}
	char *offset = buffer->offset;
	memcpy(buffer->offset, data, length);
	buffer->length += length;
	buffer->offset += length;
	*buffer->offset = '\0';
	return offset;
}

static void _buffer_reset(buffer_t *buffer)
{
	buffer->offset = buffer->data;
	buffer->length = 0;
}

static void _buffer_destroy(buffer_t *buffer)
{
	vfree(buffer->data);
	vfree(buffer);
}

static http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	if (message)
	{
		message->client = client;
		if (parent)
		{
			parent->response = message;

			message->type = parent->type;
			message->client = parent->client;
			message->version = parent->version;
			message->result = parent->result;
		}
	}
	return message;
}

http_message_t * httpmessage_create()
{
	return _httpmessage_create(NULL, NULL);
}

static void _httpmessage_reset(http_message_t *message)
{
	if (message->uri)
		_buffer_reset(message->uri);
	if (message->content)
		_buffer_reset(message->content);
	if (message->headers_storage)
		_buffer_reset(message->headers_storage);
}

void httpmessage_destroy(http_message_t *message)
{
	if (message->response)
		httpmessage_destroy(message->response);
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content)
		_buffer_destroy(message->content);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	dbentry_t *header = message->headers;
	while (header)
	{
		dbentry_t *next = header->next;
		free(header);
		header = next;
	}
	vfree(message);
}

static int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;

	do
	{
		int next = message->state  & PARSE_MASK;
		switch (next)
		{
			case PARSE_INIT:
			{
				if (!strncasecmp(data->offset,"GET ",4))
				{
					message->type = MESSAGE_TYPE_GET;
					data->offset += 4;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"POST ",5))
				{
					message->type = MESSAGE_TYPE_POST;
					data->offset += 5;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"HEAD ",5))
				{
					message->type = MESSAGE_TYPE_HEAD;
					data->offset += 5;
					next = PARSE_URI;
				}
#ifndef HTTP_METHOD_PARTIAL
				else if (!strncasecmp(data->offset,"PUT ",4))
				{
					message->type = MESSAGE_TYPE_PUT;
					data->offset += 4;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"DELETE ",7))
				{
					message->type = MESSAGE_TYPE_DELETE;
					data->offset += 7;
					next = PARSE_URI;
				}
#endif
				else
				{
					data->offset++;
					message->version = message->client->server->config->version;
					message->result = RESULT_405;
					ret = EREJECT;
				}
			}
			break;
			case PARSE_URI:
			{
				char *uri = data->offset;
				int length = 0;
				if (message->uri == NULL)
				{
					/**
					 * to use parse_cgi from a module, the functions
					 * has to run on message without client attached.
					 */
					int chunksize = CHUNKSIZE;
					if (message->client)
						chunksize = message->client->server->config->chunksize;
					message->uri = _buffer_create(2, chunksize);
				}
				while (data->offset < (data->data + data->size) && next == PARSE_URI)
				{
					switch (*data->offset)
					{
						case ' ':
						{
							*data->offset = '\0';
							uri = _buffer_append(message->uri, uri, length + 1);
							next = PARSE_VERSION;
						}
						break;
						case '\r':
						{
							next = PARSE_HEADER;
							*data->offset = '\0';
							if (*(data->offset + 1) == '\n')
								data->offset++;
							uri = _buffer_append(message->uri, uri, length + 1);
						}
						break;
						case '\n':
						{
							next = PARSE_HEADER;
							*data->offset = '\0';
						}
						break;
						default:
						{
							length++;
						}
					}
					data->offset++;
				}
				
				if (next == PARSE_URI)
				{
					uri = _buffer_append(message->uri, uri, length);
				}
				if (uri == NULL)
				{
					_buffer_destroy(message->uri);
					int chunksize = CHUNKSIZE;
					if (message->client)
						chunksize = message->client->server->config->chunksize;
					message->uri = _buffer_create(1, chunksize);
					message->version = message->client->server->config->version;
					message->result = RESULT_414;
					ret = EREJECT;
				}
				else
					message->query = uri;
				if (message->uri->maxchunks == 0)
				{
					message->version = message->client->server->config->version;
					message->result = RESULT_414;
					ret = EREJECT;
				}
				if (next != PARSE_URI)
				{
					if (message->uri->length > 1)
					{
						int i;
						for (i = 0; i < message->uri->length; i++)
						{
							if (message->uri->data[i] == '?')
							{
								message->query = message->uri->data + i + 1;
								break;
							}
						}
					}
					else
					{
						message->version = message->client->server->config->version;
						message->result = RESULT_400;
						ret = EREJECT;
					}
					dbg("new request for %s", message->uri->data);
				}
			}
			break;
			case PARSE_STATUS:
			{
				int i;
				for (i = HTTP09; i < HTTPVERSIONS; i++)
				{
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(data->offset, _http_message_version[i], length))
					{
						message->version = i;
						data->offset += length;
						break;
					}
				}
				if (i < HTTPVERSIONS)
				{
					/** pass the next space character */
					data->offset++;
					char status[4] = {data->offset[0], data->offset[1], data->offset[2], 0};
					httpmessage_addheader(message, "Status", status);
					data->offset = strchr(data->offset, '\n') + 1;
				}
				next = PARSE_HEADER;
			}
			break;
			case PARSE_VERSION:
			{
				int i;
				for (i = HTTP09; i < HTTPVERSIONS; i++)
				{
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(data->offset, _http_message_version[i], length))
					{
						message->version = i;
						data->offset += length;
						break;
					}
				}
				if (i == HTTPVERSIONS)
					ret = EREJECT;
				while (data->offset < (data->data + data->size) && next == PARSE_VERSION)
				{
					switch (*data->offset)
					{
						case '\r':
						{
							next = PARSE_HEADER;
							if (data->offset[1] == '\n')
							{
								data->offset++;
							}
						}
						break;
						case '\n':
						{
							next = PARSE_HEADER;
							if (data->offset[1] == '\r')
							{
								data->offset++;
							}
						}
						break;
						default:
						{
						}
					}
					data->offset++;
				}
			}
			break;
			case PARSE_HEADER:
			{
				char *header = data->offset;
				int length = 0;
				if (message->headers_storage == NULL)
				{
					int chunksize = CHUNKSIZE;
					if (message->client)
						chunksize = message->client->server->config->chunksize;
					message->headers_storage = _buffer_create(MAXCHUNKS_HEADER, chunksize);
				}
				/* store header line as "<key>:<value>\0" */
				while (data->offset < (data->data + data->size) && next == PARSE_HEADER)
				{
					if (*data->offset == '\n')
					{
						*data->offset = '\0';
						if (length == 0 && !(message->state & PARSE_CONTINUE))
						{
							next = PARSE_HEADERNEXT;
						}
						else
						{
							header[length] = 0;
							_buffer_append(message->headers_storage, header, length + 1);
							header = data->offset + 1;
							length = 0;
							message->state &= ~PARSE_CONTINUE;
						}
					}
					else if (*data->offset != '\r')
						length++;
					data->offset++;
				}
				/* not enougth data to complete the line */
				if (next == PARSE_HEADER && length > 0)
				{
					_buffer_append(message->headers_storage, header, length);
					message->state |= PARSE_CONTINUE;
				}
			}
			break;
			case PARSE_HEADERNEXT:
			{
				/* create key/value entry for each "<key>:<value>\0" string */
				_httpmessage_fillheaderdb(message);
				/* reset the buffer to begin the content at the begining of the buffer */
				data->length -= (data->offset - data->data);
				while (data->length > 0 && (*(data->offset) == 0))
				{
					data->offset++;
					data->length--;
				}
				if (data->length > 0)
					memcpy(data->data, data->offset, data->length);
				else
					*data->data = '\0';
				data->offset = data->data;
				next = PARSE_CONTENT;
			}
			break;
			case PARSE_CONTENT:
			{
				if (message->content_length == 0)
				{
					next = PARSE_END;
				}
				else
				{
					/**
					 * The content of the request is the buffer past to the socket.
					 * Then it is always a partial content. And the content has
					 * to be treat part by part.
					 * Is possible to use the buffer with the function
					 *  httpmessage_content
					 */
					int length = data->length -(data->offset - data->data);
					message->content = data;

					/**
					 * At the end of the parsing the content_length of request 
					 * is zero. But it is false, the true value is
					 * Sum(content->length);
					 */
					if (message->content_length > 0)
					{
						message->content_length -= length;
						if (message->content_length <= 0)
							next = PARSE_END;
					}
				}
			}
			break;
			case PARSE_END:
			{
				ret = ESUCCESS;
			}
			break;
		}
		if (next == (message->state & PARSE_MASK) && (ret == ECONTINUE))
		{
			if (next < PARSE_HEADERNEXT)
				ret = EINCOMPLETE;
			break;
		}
		message->state = (message->state & ~PARSE_MASK) | next;
	} while (ret == ECONTINUE);
	return ret;
}

void httpclient_addconnector(http_client_t *client, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}

	callback->func = func;
	callback->arg = funcarg;
	callback->next = client->callbacks;
	client->callbacks = callback;
}

void *httpclient_context(http_client_t *client)
{
	return client->ctx;
}

http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg)
{
	http_recv_t previous = client->recvreq;
	client->recvreq = func;
	client->ctx = arg;
	return previous;
}

http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg)
{
	http_send_t previous = client->sendresp;
	client->sendresp = func;
	client->ctx = arg;
	return previous;
}

int httpclient_recv(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = recv(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

int httpclient_send(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = send(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

static int _httpmessage_buildheader(http_client_t *client, http_message_t *response, buffer_t *header)
{
	if (response->headers == NULL)
		_httpmessage_fillheaderdb(response);
	dbentry_t *headers = response->headers;
	http_message_version_e version = response->version;
	if (response->version > (client->server->config->version & HTTPVERSION_MASK))
		version = (client->server->config->version & HTTPVERSION_MASK);
	_buffer_append(header, _http_message_version[version], strlen(_http_message_version[version]));
	_buffer_append(header, (char *)_http_message_result[response->result], strlen(_http_message_result[response->result]));
	_buffer_append(header, "\r\n", 2);
	while (headers != NULL)
	{
		_buffer_append(header, headers->key, strlen(headers->key));
		_buffer_append(header, ": ", 2);
		_buffer_append(header, headers->value, strlen(headers->value));
		_buffer_append(header, "\r\n", 2);
		headers = headers->next;
	}
	if (response->content_length > 0)
	{
		if (response->keepalive > 0)
		{
			char keepalive[32];
			snprintf(keepalive, 31, "%s: %s\r\n", str_connection, "Keep-Alive");
			_buffer_append(header, keepalive, strlen(keepalive));
		}
		char content_length[32];
		snprintf(content_length, 31, "%s: %d\r\n", str_contentlength, response->content_length);
		_buffer_append(header, content_length, strlen(content_length));
	}
	header->offset = header->data;
	return ESUCCESS;
}

static http_client_t *_httpclient_create(http_server_t *server)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	client->server = server;

	client->recvreq = httpclient_recv;
	client->sendresp = httpclient_send;
	client->ctx = client;

	http_connector_list_t *callback = server->callbacks;
	while (callback != NULL)
	{
		httpclient_addconnector(client, callback->vhost, callback->func, callback->arg);
		callback = callback->next;
	}
	client->callback = client->callbacks;

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	http_connector_list_t *callback = client->callbacks;
	while (callback != NULL)
	{
		http_connector_list_t *next = callback->next;
		if (callback->vhost)
			free(callback->vhost);
		free(callback);
		callback = next;
	}
	if (client->request)
		httpmessage_destroy(client->request);
	if (client->session_storage)
		vfree(client->session_storage);
	vfree(client);
}

static int _httpclient_connect(http_client_t *client)
{

	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
	do
	{
		_httpclient_run(client);
	} while(!(client->state & CLIENT_STOPPED));
	dbg("client %p close", client);
#ifdef DEBUG
	fflush(stderr);
#endif
	return 0;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	char *vhost = NULL;
	http_connector_list_t *iterator;

	iterator = client->callbacks;
	while (iterator != NULL)
	{
		vhost = iterator->vhost;
		if (vhost != NULL)
		{
			char *host = httpmessage_REQUEST(request, "host");
			if (!strcasecmp(vhost, host))
				vhost = NULL;
		}

		if (vhost == NULL && iterator->func)
		{
			ret = iterator->func(iterator->arg, request, response);
			if (ret != EREJECT)
			{
				if (ret == ESUCCESS)
				{
					client->state |= CLIENT_RESPONSEREADY;
					if (response->result != RESULT_200)
					{
						ret = EREJECT;
					}
				}
				client->callback = iterator;
				break;
			}
		}
		iterator = iterator->next;
	}
	return ret;
}

static int _httpclient_request(http_client_t *client)
{
	/**
	 * By default the function has to way more data on the socket
	 * before to be call again.
	 *  ECONTINUE means treatment (here is more data) is needed.
	 *  EINCOMPLETE means the function needs to be call again ASAP.
	 * This is the same meaning with the connectors.
	 */
	int ret = ECONTINUE;
	int size = 0;
	buffer_t *tempo = _buffer_create(1, client->server->config->chunksize);
	if (client->request == NULL)
		client->request = _httpmessage_create(client, NULL);

	/**
	 * here, it is the call to the recvreq callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	size = client->recvreq(client->ctx, tempo->data, tempo->size);
	if (size < 0)
	{
		if (errno != EAGAIN)
		{
			_buffer_destroy(tempo);
			return EREJECT;
		}
	}
	else if (size > 0)
	{
		tempo->length = size;
		ret = _httpmessage_parserequest(client->request, tempo);

		if (ret == EREJECT)
		{
			if (client->request->response == NULL)
			{
				/** parsing error before response creation */
				client->request->response = _httpmessage_create(client, client->request);
			}
		}
		else
		{
			/**
			 * The request is partially read.
			 * The connector can start to read the request when the header is ready.
			 * A problem is the size of the header. It is impossible to start
			 * the treatment before the end of the header, and it needs to
			 * store the header informations. It takes some place in  memory,
			 * depending of the server. It may be dangerous, a hacker can send
			 * a request with a very big header.
			 */
			if ((client->request->state & PARSE_MASK) >= PARSE_CONTENT)
			{
				if (client->request->response == NULL)
					client->request->response = _httpmessage_create(client, client->request);
				ret = _httpclient_checkconnector(client, client->request, client->request->response);
				if (ret == EREJECT)
					client->request->response->result = RESULT_404;
			}
		}
		client->request->content = NULL;
	}
	else /* socket shutdown */
	{
		_buffer_destroy(tempo);
		return EREJECT;
	}
	_buffer_destroy(tempo);
	switch (ret)
	{
		case ESUCCESS:
		{
			if (!(client->state & CLIENT_RESPONSEREADY) &&
				(client->request->type == MESSAGE_TYPE_PUT ||
				client->request->type == MESSAGE_TYPE_DELETE))
			{
				client->request->result = RESULT_405;
			}
		}
		break;
		case EREJECT:
		{
			ret = ESUCCESS;
		}
		break;
		case ECONTINUE:
		case EINCOMPLETE:
		{
			if ((client->request->state & PARSE_MASK) == PARSE_END)
				ret = ESUCCESS;
		}
		break;
	}
	return ret;
}

static int _httpclient_pushrequest(http_client_t *client, http_message_t *request)
{
	request->connector = client->callback;
	http_message_t *iterator = client->request_queue;
	if (iterator == NULL)
	{
		client->request_queue = request;
	}
	else
	{
		while (iterator->next != NULL) iterator = iterator->next;
		iterator->next = request;
	}
	return (request->result != RESULT_200)? EREJECT:ESUCCESS;
}

static int _httpclient_run(http_client_t *client)
{
	http_message_t *request = NULL;
	if (client->request_queue)
		request = client->request_queue;

	int request_ret = ECONTINUE;
	if ((client->server->config->version >= (HTTP11 | HTTP_PIPELINE)) || 
		((client->state & CLIENT_MACHINEMASK) < CLIENT_PUSHREQUEST))
	{
		request_ret = _httpclient_request(client);
		if (request_ret == ESUCCESS)
		{
			_httpclient_pushrequest(client, client->request);
		}
	}

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
		{
			client->state &= ~CLIENT_RESPONSEREADY;
			if (request_ret == ESUCCESS)
			{
				client->state = CLIENT_PUSHREQUEST | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
				client->state = CLIENT_REQUEST | (client->state & ~CLIENT_MACHINEMASK);
#ifdef VTHREAD
			if (request_ret == ECONTINUE)
			{
				int sret;
				struct timeval *ptimeout = NULL;
				struct timeval timeout;
				fd_set rfds;
				if (client->server->config->keepalive)
				{
					timeout.tv_sec = client->server->config->keepalive;
					timeout.tv_usec = 0;
					ptimeout = &timeout;
				}
				FD_ZERO(&rfds);
				FD_SET(client->sock, &rfds);
				sret = select(client->sock + 1, &rfds, NULL, NULL, ptimeout);
				if (sret < 1)
				{
					/* timeout */
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
				}
			}
#endif
		}
		break;
		case CLIENT_REQUEST:
		{
			if (request_ret == ESUCCESS)
			{
				client->state = CLIENT_PUSHREQUEST | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (request_ret == EREJECT)
			{
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				client->state &= ~CLIENT_KEEPALIVE;
			}
#ifdef VTHREAD
			else if (request_ret == ECONTINUE)
			{
				int sret;
				struct timeval *ptimeout = NULL;
				struct timeval timeout;
				fd_set rfds;
				if (client->server->config->keepalive)
				{
					timeout.tv_sec = client->server->config->keepalive;
					timeout.tv_usec = 0;
					ptimeout = &timeout;
				}
				FD_ZERO(&rfds);
				FD_SET(client->sock, &rfds);
				sret = select(client->sock + 1, &rfds, NULL, NULL, ptimeout);
				if (sret < 1)
				{
					/* timeout */
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
				}
			}
#endif
		}
		break;
		case CLIENT_PUSHREQUEST:
		{
			if (client->request->response->result != RESULT_200)
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->response->content == NULL)
				client->state = CLIENT_PARSER1 | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			if (client->request->keepalive)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			if (client->request->response->keepalive)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			client->request = NULL;
		}
		break;
		case CLIENT_PARSER1:
		{
			int ret = EREJECT;
			if (request->connector)
				ret = request->connector->func(request->connector->arg, request, request->response);
			if (ret == EREJECT)
			{
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
				/** delete func to stop request after the error response **/
				request->connector = NULL;
			}
			else if (ret != EINCOMPLETE)
			{
				if (ret == ESUCCESS)
					client->state |= CLIENT_RESPONSEREADY;
				if (request->response->version == HTTP09)
					client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
				else
					client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			}
		}
		break;
		case CLIENT_PARSER2:
		{
			int ret = EREJECT;
			if (request->connector && request->connector->func)
				ret = request->connector->func(request->connector->arg, request, request->response);
			if (ret == EREJECT)
			{
				/**
				 * the connector rejects now the request, then an error occured
				 * and the connector is not able to continue
				 */
				/** delete func to stop request after the error response **/
				request->connector = NULL;
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (ret != EINCOMPLETE &&
					request->response->content &&
					request->response->content->length > 0)
			{
				if (ret == ESUCCESS)
					client->state |= CLIENT_RESPONSEREADY;
				/**
				 * the connector return ESUCCESS or ECONTINUE, then it
				 * allows the connection to change state
				 * on EINCOMPLETE the state has to stay in this value
				 */
				/**
				 * on ESUCCESS or ECONTINUE if some value is ready
				 * it is required to send them
				 */
				/**
				 * an empty file may have not content
				 */
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (ret == ESUCCESS)
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_RESPONSEHEADER:
		{
			int size = 0;
			buffer_t *header = _buffer_create(MAXCHUNKS_HEADER, client->server->config->chunksize);
			_httpmessage_buildheader(client, request->response, header);
			while (header->length > 0)
			{
				/**
				 * here, it is the call to the sendresp callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				size = client->sendresp(client->ctx, header->offset, header->length);
				if (size < 0)
					break;
				header->offset += size;
				header->length -= size;
			}
			client->sendresp(client->ctx, "\r\n", 2);
			if (size < 0)
			{
				client->state &= ~CLIENT_KEEPALIVE;
				client->state |= CLIENT_ERROR;
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (client->state & CLIENT_RESPONSEREADY)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
			_buffer_destroy(header);
		}
		break;
		case CLIENT_RESPONSECONTENT:
		{
			if (request->type != MESSAGE_TYPE_HEAD &&
				request->response->content &&
				request->response->content->length > 0)
			{
				int size = 0;
				size = client->sendresp(client->ctx, request->response->content->data, request->response->content->length);
				if (size < 0)
				{
					client->state &= ~CLIENT_KEEPALIVE;
					client->state |= CLIENT_ERROR;
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (size == 0 && (request->response->content->length > 0))
				{
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (size == request->response->content->length)
				{
					_buffer_reset(request->response->content);
					if (client->state & CLIENT_RESPONSEREADY)
						client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
					else
						client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
				}
				else
				{
					request->response->content->length -= size;
					request->response->content->offset += size;
				}
			}
			else
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_PARSERERROR:
		{
			if (request->response->result == RESULT_200)
				request->response->result = RESULT_400;
			httpmessage_addheader(request->response, "Allow", "GET, POST, HEAD");
			const char *value = _http_message_result[request->response->result];
			httpmessage_addcontent(request->response, "text/plain", (char *)value, strlen(value));
			if (request->response->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			client->state |= CLIENT_RESPONSEREADY;
		}
		break;
		case CLIENT_COMPLETE:
		{
			setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
			/**
			 * to stay in keep alive the rules are:
			 *  - the server has to be configurated;
			 *  - the request uses the protocol HTTP11 and over
			 *  - the client is not in error
			 *  - the request asks to stay in keep alive mode
			 *  - the response is understandable by the client
			 *     (the webbrowser nees to know when the response is complete,
			 *      then the response needs a content length).
			 */
			if (client->server->config->keepalive &&
				(client->state & CLIENT_KEEPALIVE) &&
				request && request->response->version > HTTP10 &&
				((client->state & ~CLIENT_ERROR) == client->state) &&
				request->response->content_length > 0)
			{
				client->state = CLIENT_NEW | (client->state & ~CLIENT_MACHINEMASK);
				dbg("keepalive %p", client);
			}
			else
			{
				client->state |= CLIENT_STOPPED;
				http_client_modctx_t *modctx = client->modctx;
				while (modctx)
				{
					http_client_modctx_t *next = modctx->next;
					if (modctx->freectx)
					{
						modctx->freectx(modctx->ctx);
					}
					free(modctx);
					modctx = next;
				}
				client->modctx = NULL;
				shutdown(client->sock, SHUT_RDWR);
		#ifndef WIN32
				close(client->sock);
		#else
				closesocket(client->sock);
		#endif
				client->sock = -1;
			}
			if (client->request_queue)
			{
				http_message_t *next = client->request_queue->next;
				httpmessage_destroy(client->request_queue);
				client->request_queue = next;
			}
		}
		break;
	}
	return 0;
}

static int _httpserver_connect(http_server_t *server)
{
	int ret = 0;
	int maxfd = 0;
	fd_set rfds, wfds;

	server->run = 1;
	while(server->run)
	{
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(server->sock, &rfds);
		maxfd = server->sock;
		http_client_t *client = server->clients;

		while (client != NULL)
		{
			if (client->state & CLIENT_STOPPED)
			{
#ifdef VTHREAD
				vthread_join(client->thread, NULL);
#endif

				http_client_t *client2 = server->clients;
				if (client == server->clients)
				{
					server->clients = client->next;
					client2 = server->clients;
				}
				else
				{
					while (client2->next != client) client2 = client2->next;
					client2->next = client->next;
					client2 = client2->next;
				}
				_httpclient_destroy(client);
				client = client2;
			}
			else if (client->sock > 0)
			{
#ifndef VTHREAD
				if ((client->state & CLIENT_MACHINEMASK) != CLIENT_REQUEST)
				{
					FD_SET(client->sock, &wfds);
				}
				else
					FD_SET(client->sock, &rfds);
#else
				FD_SET(client->sock, &rfds);
#endif
				maxfd = (maxfd > client->sock)? maxfd:client->sock;
				client = client->next;
			}
		}
		struct timeval *ptimeout = NULL;
		struct timeval timeout;
		if (server->config->keepalive)
		{
			timeout.tv_sec = server->config->keepalive;
			timeout.tv_usec = 0;
			ptimeout = &timeout;
		}
		ret = select(maxfd +1, &rfds, &wfds, NULL, ptimeout);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = _httpclient_create(server);
				// Client connection request recieved
				// Create new client socket to communicate
				client->addr_size = sizeof(client->addr);
				client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &client->addr_size);
				dbg("new connection %p", client);
				http_server_mod_t *mod = server->mod;
				http_client_modctx_t *currentctx = NULL;
				while (mod)
				{
					http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));

					if (mod->func)
					{
						modctx->ctx = mod->func(mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
					}
					modctx->freectx = mod->freectx;
					mod = mod->next;
					if (client->modctx == NULL)
						client->modctx = modctx;
					else
					{
						currentctx->next = modctx;
					}
					currentctx = modctx;
				}
				int flags;
				flags = fcntl(client->sock,F_GETFL, 0);
				fcntl(client->sock,F_SETFL, flags | O_NONBLOCK);

				client->next = server->clients;
				server->clients = client;

				ret = 1;
			}
			else
			{
				client = server->clients;
				while (client != NULL)
				{
					http_client_t *next = client->next;
					if (FD_ISSET(client->sock, &rfds) || FD_ISSET(client->sock, &wfds))
					{
#ifndef VTHREAD
						client->state |= CLIENT_RUNNING;
						_httpclient_run(client);
#else
						if (!(client->state & (CLIENT_STARTED | CLIENT_RUNNING)))
						{
							vthread_attr_t attr;
							client->state &= ~CLIENT_STOPPED;
							client->state |= CLIENT_STARTED;
							vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_connect, (void *)client, sizeof(*client));
						}
						else
						{
							if (!vthread_exist(client->thread))
								client->state |= CLIENT_STOPPED;
							vthread_yield(client->thread);
						}
#endif
					}
					client = next;
				}
			}
		}
#ifndef VTHREAD
		/**
		 * TODO: this code has to be checked and explained
		 */
		else if (ret == 0)
		{
			client = server->clients;
			while (client != NULL)
			{
				http_client_t *next = client->next;
				client->state &= ~CLIENT_KEEPALIVE;
				_httpclient_run(client);
				client = next;
			}
		}
#endif
	}
	return ret;
}

http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;

	server = vcalloc(1, sizeof(*server));
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
#ifdef WIN32
	WSADATA wsaData = {0};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
	if (_httpserver_start(server))
	{
		free(server);
		return NULL;
	}
	return server;
}

static int _httpserver_start(http_server_t *server)
{
	int status;

	if (server->config->addr == NULL)
	{
#ifdef IPV6
		server->sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
#else
		server->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
#endif
		if ( server->sock < 0 )
		{
			warn("Error creating socket");
			return -1;
		}

		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEPORT) failed");
#endif

		int socklen = sizeof(struct sockaddr_in);
#ifdef IPV6
		struct sockaddr_in6saddr;
		saddr.sin6_family = AF_INET6;
		saddr.sin6_port = htons(server->config->port);
		saddr.sin6_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
#else
		struct sockaddr_in saddr;

		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(server->config->port);
		saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
#endif
		status = bind(server->sock, (struct sockaddr *)&saddr, socklen);
	}
	else
	{
		struct addrinfo hints;
		struct addrinfo *result, *rp;

		memset(&hints, 0, sizeof(struct addrinfo));
#ifdef IPV6
		hints.ai_family = AF_INET6;    /* Allow IPv4 or IPv6 */
#else
		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
#endif
		hints.ai_socktype = SOCK_STREAM; /* Stream socket */
		hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
		hints.ai_protocol = 0;          /* Any protocol */
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		status = getaddrinfo(server->config->addr, NULL, &hints, &result);
		if (status != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
			return -1;
		}

		for (rp = result; rp != NULL; rp = rp->ai_next)
		{
			server->sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (server->sock == -1)
				continue;

			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEPORT) failed");
#endif

			((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(server->config->port);
			if (bind(server->sock, rp->ai_addr, rp->ai_addrlen) == 0)
				break;                  /* Success */

#ifndef WIN32
			close(server->sock);
#else
			closesocket(server->sock);
#endif
		}
		freeaddrinfo(result); 
	}

	if (!status)
	{
		status = listen(server->sock, server->config->maxclients);
	}
	if (status)
	{
		warn("Error bind/listen port %d", server->config->port);
		return -1;
	}
	return 0;
}

void httpserver_addmod(http_server_t *server, http_getctx_t modf, http_freectx_t unmodf, void *arg)
{
	http_server_mod_t *mod = vcalloc(1, sizeof(*mod));
	mod->func = modf;
	mod->freectx = unmodf;
	mod->arg = arg;
	mod->next = server->mod;
	server->mod = mod;
}

void httpserver_addconnector(http_server_t *server, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;
	
	callback = vcalloc(1, sizeof(*callback));
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = server->callbacks;
	server->callbacks = callback;
}

void httpserver_connect(http_server_t *server)
{
	vthread_attr_t attr;

#ifndef VTHREAD
	_httpserver_connect(server);
#else
	vthread_create(&server->thread, &attr, (vthread_routine)_httpserver_connect, (void *)server, sizeof(*server));
#endif
}

void httpserver_disconnect(http_server_t *server)
{
	if (server->thread)
	{
		server->run = 0;
#ifdef VTHREAD
		vthread_join(server->thread, NULL);
		server->thread = 0;
#endif
	}
}

void httpserver_destroy(http_server_t *server)
{
	http_connector_list_t *callback = server->callbacks;
	while (callback)
	{
		http_connector_list_t  *next = callback->next;
		if (callback->vhost)
			vfree(callback->vhost);
		vfree(callback);
		callback = next;
	}
	http_server_mod_t *mod = server->mod;
	while (mod)
	{
		http_server_mod_t  *next = mod->next;
		vfree(mod);
		mod = next;
	}
	vfree(server);
#ifdef WIN32
	WSACleanup();
#endif
}

/*************************************************************
 * httpmessage public functions
 */
void *httpmessage_private(http_message_t *message, void *data)
{
	if (data != NULL)
	{
		message->private = data;
	}
	return message->private;
}

int httpmessage_content(http_message_t *message, char **data, int *size)
{
	*size = 0;
	if (message->content)
	{
		*data = message->content->data;
		*size = message->content->length;
	}
	return message->content_length + *size;
}

int httpmessage_parsecgi(http_message_t *message, char *data, int *size)
{
	static buffer_t tempo;
	tempo.data = data;
	tempo.offset = data;
	tempo.length = *size;
	tempo.size = *size;
	if (message->state == PARSE_INIT)
		message->state = PARSE_STATUS;
	int ret = _httpmessage_parserequest(message, &tempo);
	*size = tempo.length;
	if (message->state == PARSE_END)
		message->content = NULL;
	return ret;
}

http_message_result_e httpmessage_result(http_message_t *message, http_message_result_e result)
{
	if (result > 0)
		message->result = result;
	return message->result;
}

static void _httpmessage_fillheaderdb(http_message_t *message)
{
	int i;
	buffer_t *storage = message->headers_storage;
	if (storage == NULL)
		return;
	char *key = storage->data;
	char *value = NULL;
	for (i = 0; i < storage->length; i++)
	{
		if (storage->data[i] == ':' && value == NULL)
		{
			storage->data[i] = '\0';
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if (storage->data[i] == '\0')
		{
			_httpmessage_addheader(message, key, value);
			key = storage->data + i + 1;
			value = NULL;
		}
	}
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	if (message->headers_storage == NULL)
	{
		int chunksize = CHUNKSIZE;
		if (message->client != NULL)
			chunksize = message->client->server->config->chunksize;
		message->headers_storage = _buffer_create(MAXCHUNKS_HEADER, chunksize);
	}
	key = _buffer_append(message->headers_storage, key, strlen(key));
	_buffer_append(message->headers_storage, ":", 1);
	value = _buffer_append(message->headers_storage, value, strlen(value) + 1);
}

static void _httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	dbentry_t *headerinfo;
	headerinfo = vcalloc(1, sizeof(dbentry_t));
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
	dbg("header %s => %s", key, value);
	if (!strncasecmp(key, str_connection, 10) && strcasestr(value, "Keep-Alive") )
	{
		message->keepalive = 1;
	}
	if (!strncasecmp(key, str_contentlength, 14))
	{
		message->content_length = atoi(value);
	}
	if (!strncasecmp(key, "Status", 6))
	{
		int result;
		sscanf(value,"%d",&result);
		switch (result)
		{
			case 200:
				result = RESULT_200;
			break;
			case 400:
				result = RESULT_400;
			break;
			case 404:
				result = RESULT_404;
			break;
			case 405:
				result = RESULT_405;
			break;
#ifndef HTTP_STATUS_PARTIAL
			case 301:
				result = RESULT_301;
			break;
			case 302:
				result = RESULT_302;
			break;
			case 304:
				result = RESULT_304;
			break;
			case 401:
				result = RESULT_401;
			break;
			case 414:
				result = RESULT_414;
			break;
			case 505:
				result = RESULT_505;
			break;
			case 511:
				result = RESULT_511;
			break;
#endif
			default:
				result = RESULT_400;
		}
		httpmessage_result(message, result);
	}
}

char *httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
	if (message->content == NULL && content != NULL)
		message->content = _buffer_create(MAXCHUNKS_CONTENT, message->client->server->config->chunksize);

	if (message->state < PARSE_CONTENT)
	{
		if (type == NULL)
		{
			httpmessage_addheader(message, (char *)str_contenttype, "text/plain");
		}
		else
		{
			httpmessage_addheader(message, (char *)str_contenttype, type);
		}
		message->state = PARSE_CONTENT;
	}
	if (content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		_buffer_append(message->content, content, length);
	}
	if (message->content_length == 0)
		message->content_length = length;
	if (message->content != NULL && message->content->data != NULL )
		return message->content->data;
	return NULL;
}

int httpmessage_keepalive(http_message_t *message)
{
	message->keepalive = 1;
	return message->client->sock;
}

static char default_value[8] = {0};
static char host[NI_MAXHOST], service[NI_MAXSERV];
char *httpserver_INFO(http_server_t *server, char *key)
{
	char *value = default_value;
	memset(default_value, 0, sizeof(default_value));

	if (!strcasecmp(key, "name"))
	{
		value = server->config->hostname;
	}
	else if (!strcasecmp(key, "software"))
	{
		value = httpserver_software;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		value = _http_message_version[(server->config->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "port"))
	{
		//snprintf(value, 5, "%d", message->client->server->config->port);
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				0, 0,
				service, NI_MAXSERV, NI_NUMERICSERV);
			value = service;
		}
	}
	else if (!strcasecmp(key, "addr"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				host, NI_MAXHOST,
				0, 0, NI_NUMERICHOST);
			value = host;
		}
	}
	return value;
}

char *httpmessage_SERVER(http_message_t *message, char *key)
{
	if (message->client == NULL)
		return NULL;
	return httpserver_INFO(message->client->server, key);
}

char *httpmessage_REQUEST(http_message_t *message, char *key)
{
	char *value = default_value;
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
	}
	else if (!strcasecmp(key, "query"))
	{
		if (message->query != NULL)
			value = message->query;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		strcpy(value, "http");
	}
	else if (!strcasecmp(key, "method"))
	{
		switch (message->type)
		{
			case MESSAGE_TYPE_GET:
				value = "GET";
			break;
			case MESSAGE_TYPE_POST:
				value = "POST";
			break;
			case MESSAGE_TYPE_HEAD:
				value = "HEAD";
			break;
#ifndef HTTP_METHOD_PARTIAL
			case MESSAGE_TYPE_PUT:
				value = "PUT";
			break;
			case MESSAGE_TYPE_DELETE:
				value = "DELETE";
			break;
#endif
			default:
			break;
		}
	}
	else if (!strcasecmp(key, "content"))
	{
		if (message->content != NULL)
		{
			value = message->content->data;
		}
	}
	else if (!strncasecmp(key, "remote_addr", 11))
	{
		if (message->client == NULL)
			return NULL;

		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST, 0, 0, NI_NUMERICHOST);
		value = host;
	}
	else if (!strncasecmp(key, "remote_", 7))
	{
		if (message->client == NULL)
			return NULL;
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST,
			service, NI_MAXSERV, NI_NUMERICSERV);

		if (!strcasecmp(key + 7, "host"))
			value = host;
		if (!strcasecmp(key + 7, "port"))
			value = service;
	}
	else
	{
		dbentry_t *header = message->headers;
		while (header != NULL)
		{
			if (!strcasecmp(header->key, key))
			{
				value = header->value;
				break;
			}
			header = header->next;
		}
	}
	return value;
}

char *httpmessage_SESSION(http_message_t *message, char *key, char *value)
{
	dbentry_t *sessioninfo;
	if (message->client == NULL)
		return NULL;

	sessioninfo = message->client->session;
	
	while (sessioninfo && strcmp(sessioninfo->key, key))
	{
		sessioninfo = sessioninfo->next;
	}
	if (value != NULL)
	{
		if (!sessioninfo)
		{
			sessioninfo = vcalloc(1, sizeof(*sessioninfo));
			if (!message->client->session_storage)
			{
				int chunksize = CHUNKSIZE;
				if (message->client)
					chunksize = message->client->server->config->chunksize;
				message->client->session_storage = _buffer_create(2, chunksize);
			}
			sessioninfo->key = 
				_buffer_append(message->client->session_storage, key, strlen(key) + 1);
			sessioninfo->next = message->client->session;
			message->client->session = sessioninfo;
		}
		if (sessioninfo->value)
		{
			dbentry_t *next = sessioninfo->next;
			if (strlen(value) <= strlen(sessioninfo->value))
			{
				strcpy(sessioninfo->value, value);
			}
			else if (next != NULL)
			{
				char *data = message->client->session_storage->data;
				int length = message->client->session_storage->length;
				length -= next->key - data;
				memmove(sessioninfo->key, next->key, length);
				length = next->key - sessioninfo->key;
				while (next != NULL)
				{
					next->key -= length;
					next->value -= length;
					next = next->next;
				}
				message->client->session_storage->length -= length;
			}
		}
		else
			sessioninfo->value = 
				_buffer_append(message->client->session_storage, value, strlen(value) + 1);
	}
	else if (sessioninfo == NULL)
		return default_value;
	return sessioninfo->value;
}
