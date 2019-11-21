bin-$(HTTPCLIENT_FEATURES)=client_websocket

client_websocket_SOURCES+=client_websocket.c
client_websocket_CFLAGS+=-I../include
client_websocket_LDFLAGS+=-L$(obj)httpserver/
client_websocket_LIBRARY+=pthread
client_websocket_LIBS+=$(LIBHTTPSERVER_NAME)
client_websocket_LIBS+=websocket
client_websocket_LIBS+=ouihash
client_websocket_LIBS-$(MBEDTLS)+=mbedtls mbedx509 mbedcrypto mod_mbedtls
client_websocket_LIBS-$(OPENSSL)+=ssl crypto mod_openssl

client_websocket_CFLAGS-$(DEBUG)+=-DDEBUG -g
