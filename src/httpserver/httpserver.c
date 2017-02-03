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
# include <err.h>

# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <netdb.h>

#else
# define warn(...) fprintf(stderr, __VA_ARGS__)

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

#ifdef DEBUG
# define dbg(...)	fprintf(stderr, __VA_ARGS__)
#else
# define dbg(...)
#endif

#include "valloc.h"
#include "vthread.h"
#include "dbentry.h"
#include "httpserver.h"

typedef struct buffer_s
{
	char *data;
	char *offset;
	int size;
	int length;
} buffer_t;

struct http_connector_list_s
{
	char *vhost;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
};
typedef struct http_connector_list_s http_connector_list_t;

typedef struct http_server_mod_s
{
	void *arg;
	http_getctx_t func;
	http_freectx_t freectx;
} http_server_mod_t;

#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
#define CLIENT_RESPONSEREADY 0x4000
#define CLIENT_KEEPALIVE 0x8000
#define CLIENT_MACHINEMASK 0x00FF
#define CLIENT_REQUEST 0x0000
#define CLIENT_PARSER1 0x0001
#define CLIENT_PARSER2 0x0002
#define CLIENT_RESPONSEHEADER 0x0003
#define CLIENT_RESPONSECONTENT 0x0004
#define CLIENT_PARSERERROR 0x0005
#define CLIENT_COMPLETE 0x0006
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server;
	vthread_t thread;
	http_freectx_t freectx;
	http_recv_t recvreq;
	http_send_t sendresp;
	http_connector_list_t *callbacks;
	http_connector_list_t *callback;
	http_message_t *request;
	http_message_t *response;
	void *ctx;
	dbentry_t *session;
	buffer_t *session_storage;
#ifndef WIN32
	struct sockaddr_un addr;
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
		PARSE_HEADER,
		PARSE_HEADERNEXT,
		PARSE_CONTENT,
		PARSE_END,
	} state;
	buffer_t *content;
	int content_length;
	buffer_t *uri;
	char *query;
	http_message_version_e version;
	buffer_t *headers_storage;
	dbentry_t *headers;
	void *private;
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
static void _httpmessage_addheader(http_message_t *message, char *key, char *value);
static int _httpclient_run(http_client_t *client);

/********************************************************************/
static http_server_config_t defaultconfig = {
	.addr = NULL,
	.port = 80,
	.maxclient = 10,
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
static const char str_connection[] = "Connection";
static const char str_contenttype[] = "Content-Type";
static const char str_contentlength[] = "Content-Length";
/********************************************************************/
#define CHUNKSIZE 64
#define BUFFERMAX 2048
static buffer_t * _buffer_create()
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	buffer->data = vcalloc(1, CHUNKSIZE);
	buffer->size = CHUNKSIZE;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, char *data, int length)
{
	if (buffer->data + buffer->size <= buffer->offset + length + 1)
	{
		char *data = buffer->data;
		int chunksize = CHUNKSIZE * (length/CHUNKSIZE +1);

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

static http_message_t * _httpmessage_create(http_server_t *server, http_message_t *parent)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	message->content = _buffer_create();
	if (parent)
	{
		message->type = parent->type;
		message->client = parent->client;
		message->version = parent->version;
		if (server->config->keepalive)
			message->keepalive = parent->keepalive;
	}
	return message;
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

static void _httpmessage_destroy(http_message_t *message)
{
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content)
		_buffer_destroy(message->content);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	vfree(message);
}

static int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;

	do
	{
		int next = message->state;
		switch (message->state)
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
					message->uri = _buffer_create();
				while (data->offset < (data->data + data->size) && next == PARSE_URI)
				{
					switch (*data->offset)
					{
						case ' ':
						{
							uri = _buffer_append(message->uri, uri, length + 1);
							message->query = uri;
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
							message->query = uri;
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
					message->uri = _buffer_create();
					message->result = RESULT_414;
					ret = EREJECT;
				}
				if (next != PARSE_URI)
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
			}
			break;
			case PARSE_VERSION:
			{
				int i;
				for (i = HTTP09; i < HTTPVERSIONS; i++)
				{
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(data->offset,_http_message_version[i],length))
					{
						message->version = i;
						data->offset += length;
						break;
					}
				}
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
					message->headers_storage = _buffer_create();
				while (data->offset < (data->data + data->size) && next == PARSE_HEADER)
				{
					if (*data->offset == '\r')
					{
						*data->offset = '\0';
						if (length == 0)
							next = PARSE_HEADERNEXT;
						else
						{
							_buffer_append(message->headers_storage, header, length + 1);
							header = data->offset + 1;
							length = 0;
						}
					}
					else  if (*data->offset == '\n')
					{
						header = data->offset + 1;
					}
					else
						length++;
					data->offset++;
				}

				/* not enougth data to complete the line */
				if (next == PARSE_HEADER)
				{
					_buffer_append(message->headers_storage, header, length);
				}
			}
			break;
			case PARSE_HEADERNEXT:
			{
				int i;
				buffer_t *storage = message->headers_storage;
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
				data->length -= (data->offset - data->data);
				memcpy(data->data, data->offset + 1, data->length);
				data->offset = data->data;
				next = PARSE_CONTENT;
			}
			break;
			case PARSE_CONTENT:
			{
				if (message->content_length > 0)
				{
					while (data->offset < (data->data + data->length))
					{
						data->offset++;
						message->content_length--;
						if (message->content_length <= 0)
						{
							next = PARSE_END;
							break;
						}
					}
				}
				else
				{
					next = PARSE_END;
				}
			}
			break;
			case PARSE_END:
			{
				ret = ESUCCESS;
			}
			break;
		}
		if (next == message->state && ret == ECONTINUE)
			ret = EINCOMPLETE;
		message->state = next;
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
	dbentry_t *headers = response->headers;
	http_message_version_e version = response->version;
	if (response->version > client->server->config->version)
		version = client->server->config->version;
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
	if (response->content != NULL && response->content_length > 0)
	{
		char content_length[32];
		snprintf(content_length, 31, "%s: %d\r\n", str_contentlength, response->content_length);
		_buffer_append(header, content_length, strlen(content_length));
	}
	return ESUCCESS;
}

static http_client_t *_httpclient_create(http_server_t *server)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	client->server = server;

	client->recvreq = httpclient_recv;
	client->sendresp = httpclient_send;
	client->ctx = client;
	client->freectx = NULL;

	http_connector_list_t *callback = server->callbacks;
	while (callback != NULL)
	{
		httpclient_addconnector(client, callback->vhost, callback->func, callback->arg);
		callback = callback->next;
	}
	client->callback = client->callbacks;

	client->request = _httpmessage_create(client->server, NULL);
	client->request->client = client;

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	if (client->response)
		_httpmessage_destroy(client->response);
	if (client->request)
		_httpmessage_destroy(client->request);
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
	return 0;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	char *vhost = NULL;
	http_connector_list_t *previous;
	http_connector_list_t *iterator;

	iterator = client->callbacks;
	previous = iterator;
	while (iterator != NULL)
	{
		vhost = iterator->vhost;
		if (vhost != NULL)
		{
			char *host = httpmessage_REQUEST(request, "host");
			if (!strcasecmp(vhost, host))
				vhost = NULL;
		}

		if (vhost == NULL)
		{
			ret = iterator->func(iterator->arg, client->request, client->response);
			if (ret == EREJECT)
			{
				if (iterator == client->callbacks)
				{
					client->callbacks = iterator->next;
					previous = iterator;
				}
				else
					previous->next = iterator->next;
			}
			else 
			{
				if (ret == ESUCCESS)
				{
					client->state |= CLIENT_RESPONSEREADY;
					if (client->response->result != RESULT_200)
					{
						ret = EREJECT;
					}
				}
				else 
					previous = iterator;
				client->callback = iterator;
				break;
			}
		}
		iterator = iterator->next;
	}
	return ret;
}

static int _httpclient_run(http_client_t *client)
{
	http_message_t *request = client->request;
	http_message_t *response = client->response;

		switch (client->state & CLIENT_MACHINEMASK)
		{
			case CLIENT_REQUEST:
			{
				int ret = EINCOMPLETE;
				int size = CHUNKSIZE - 1;
				buffer_t *tempo = _buffer_create();
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
						client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
						client->state &= ~CLIENT_KEEPALIVE;
						break;
					}
				}
				else if (size > 0)
				{
					tempo->length = size;
					ret = _httpmessage_parserequest(request, tempo);
					if (client->response == NULL)
					{
						client->response = _httpmessage_create(client->server, client->request);
					}
					client->response->client = client;
					if (ret == ESUCCESS && client->request->state >= PARSE_CONTENT)
					{
						ret = _httpclient_checkconnector(client, client->request, client->response);
					}
					client->callback = client->callbacks;
				}
				else /* socket shutdown */
				{
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
					_buffer_destroy(tempo);
					break;
				}
				_buffer_destroy(tempo);
				switch (ret)
				{
					case ESUCCESS:
					{
						if (!(client->state & CLIENT_RESPONSEREADY) &&
							(request->type == MESSAGE_TYPE_PUT ||
							request->type == MESSAGE_TYPE_DELETE))
						{
							request->result = RESULT_405;
							client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
						}
						else
							client->state = CLIENT_PARSER1 | (client->state & ~CLIENT_MACHINEMASK);
					}
					break;
					case EREJECT:
					{
						if (request->result == RESULT_200)
							request->result = RESULT_400;
						client->response->result = request->result;
						client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
					}
					break;
					case ECONTINUE:
					case EINCOMPLETE:
					{
						if (client->request->state == PARSE_END)
							client->state = CLIENT_PARSER1 | (client->state & ~CLIENT_MACHINEMASK);
					}
					break;
				}
			}
			break;
			case CLIENT_PARSER1:
			{
				int ret;
				ret = _httpclient_checkconnector(client, client->request, client->response);
				if (ret == EREJECT)
				{
					response->result = RESULT_404;
					client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (ret != EINCOMPLETE)
				{
					if (response->version == HTTP09)
						client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
					else
						client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
				}
				if (response->keepalive)
				{
					client->state |= CLIENT_KEEPALIVE;
				}
			}
			break;
			case CLIENT_PARSER2:
			{
				int ret = EINCOMPLETE;
				if (client->callback && client->callback->func)
					ret = client->callback->func(client->callback->arg, request, response);
				if (ret != ESUCCESS)
					client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
				else
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_RESPONSEHEADER:
			{
				int size = 0;
				buffer_t *header = _buffer_create();
				_httpmessage_buildheader(client, response, header);

				/**
				 * here, it is the call to the sendresp callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				size = client->sendresp(client->ctx, header->data, header->length);
				if (size < 0)
				{
					client->state &= ~CLIENT_KEEPALIVE;
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				}
				else
					client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
				_buffer_destroy(header);
			}
			break;
			case CLIENT_RESPONSECONTENT:
			{
				if (response->result == RESULT_200 &&
					request->type != MESSAGE_TYPE_HEAD)
				{
					int size = CHUNKSIZE - 1;
					size = client->sendresp(client->ctx, response->content->data, response->content->length);
					if (size < 0)
					{
						client->state &= ~CLIENT_KEEPALIVE;
						client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					}
					else
					{
						_buffer_reset(response->content);
						client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
					}
				}
				else
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_PARSERERROR:
			{
				httpmessage_addheader(client->response, "Allow", "GET, POST, HEAD");
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_COMPLETE:
			{
				setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
				if (!(client->state & CLIENT_KEEPALIVE))
				{
					client->state |= CLIENT_STOPPED;
					if (client->freectx)
						client->freectx(client->ctx);
					shutdown(client->sock, SHUT_RDWR);
			#ifndef WIN32
					close(client->sock);
			#else
					closesocket(client->sock);
			#endif
					client->sock = -1;
				}
				else
				{
					client->state = CLIENT_REQUEST | (client->state & ~CLIENT_MACHINEMASK);
					_httpmessage_reset(client->request);
					_httpmessage_destroy(client->response);
					client->response = NULL;
					dbg("keepalive\n");
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
		ret = select(maxfd +1, &rfds, &wfds, NULL, NULL);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = _httpclient_create(server);
				// Client connection request recieved
				// Create new client socket to communicate
				client->addr_size = sizeof(client->addr);
				client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &client->addr_size);
				if (server->mod && server->mod->func)
				{
					client->ctx = server->mod->func(server->mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
					client->freectx = server->mod->freectx;
				}
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
							vthread_yield(client->thread);
						}
#endif
					}
					client = next;
				}
			}
		}
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
		server->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
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
		struct sockaddr_in saddr;

		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(server->config->port);
		saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
		status = bind(server->sock, (struct sockaddr *)&saddr, socklen);
	}
	else
	{
		struct addrinfo hints;
		struct addrinfo *result, *rp;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
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
		status = listen(server->sock, server->config->maxclient);
	}
	if (status)
	{
		warn("Error bind/listen port %d", server->config->port);
		return -1;
	}
	return 0;
}

void httpserver_addmod(http_server_t *server, http_getctx_t mod, http_freectx_t unmod, void *arg)
{
	if (!server->mod)
		server->mod = vcalloc(1, sizeof(*server->mod));
	server->mod->func = mod;
	server->mod->freectx = unmod;
	server->mod->arg = arg;
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
	if (server->mod)
		vfree(server->mod);
	vfree(server);
#ifdef WIN32
	WSACleanup();
#endif
}

void *httpmessage_private(http_message_t *message, void *data)
{
	if (data != NULL)
	{
		message->private = data;
	}
	return message->private;
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	if (message->headers_storage == NULL)
		message->headers_storage = _buffer_create();
	key = _buffer_append(message->headers_storage, key, strlen(key) + 1);
	value = _buffer_append(message->headers_storage, value, strlen(value) + 1);
	_httpmessage_addheader(message, key, value);
}

static void _httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	dbentry_t *headerinfo;
	headerinfo = vcalloc(1, sizeof(dbentry_t));
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
	dbg("header %s => %s\n", key, value);
	if (!strncasecmp(key, str_connection, 10) && strcasestr(value, "Keep-Alive") )
	{
		message->keepalive = 1;
	}
	if (!strncasecmp(key, str_contentlength, 14))
	{
		message->content_length = atoi(value);
	}
}

char *httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
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
		/* end the header part */
		if (message->version > HTTP09)
			_buffer_append(message->content, "\r\n", 2);
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
	return message->content->data;
}

int httpmessage_keepalive(http_message_t *message)
{
	message->keepalive = 1;
	return message->client->sock;
}

static char default_value[8] = {0};
char *httpmessage_SERVER(http_message_t *message, char *key)
{
	char *value = default_value;
	char host[NI_MAXHOST], service[NI_MAXSERV];

	if (!strcasecmp(key, "name"))
	{
		strcpy(value, "libhttpserver");
	}
	else if (!strcasecmp(key, "protocol"))
	{
		value = _http_message_version[message->client->server->config->version];
	}
	else if (!strcasecmp(key, "port"))
	{
		snprintf(value, 5, "%d", message->client->server->config->port);
	}
	else if (!strncasecmp(key, "remote_", 7))
	{
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST,
			service, NI_MAXSERV, NI_NUMERICSERV);

		if (!strcasecmp(key + 7, "host"))
			value = host;
		if (!strcasecmp(key + 7, "service"))
			value = host;
	}
	return value;
}

char *httpmessage_REQUEST(http_message_t *message, char *key)
{
	char *value = "";
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
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
	else if (!strcasecmp(key, str_contentlength))
	{
		if (message->content != NULL)
		{
			value = (char *)message->content_length;
		}
	}
	else if (!strcasecmp(key, "content"))
	{
		if (message->content != NULL)
		{
			value = message->content->data;
		}
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
	dbentry_t *sessioninfo = message->client->session;
	
	while (sessioninfo && strcmp(sessioninfo->key, key))
		sessioninfo = sessioninfo->next;
	if (!sessioninfo)
	{
		sessioninfo = vcalloc(1, sizeof(*sessioninfo));
		if (!message->client->session_storage)
			message->client->session_storage = _buffer_create();
		sessioninfo->key = 
			_buffer_append(message->client->session_storage, key, strlen(key) + 1);
		sessioninfo->next = message->client->session;
		message->client->session = sessioninfo;
	}
	if (value != NULL)
	{
		if (strlen(value) <= strlen(sessioninfo->value))
			strcpy(sessioninfo->value, value);
		else
			sessioninfo->value = 
				_buffer_append(message->client->session_storage, value, strlen(value) + 1);
	}
	return sessioninfo->value;
}
