CC       := gcc
OPENSSL  ?= openssl
SERVER_CONF ?= config/server.conf
CLIENT_CONF ?= config/client.conf

CFLAGS   := -std=gnu11 -g -Wall -Wextra -O2 -Iinclude -pthread -DUSE_GUI=1 -DUSE_TLS=1
LDFLAGS  := -pthread -lm
LIBS_SSL := -lssl -lcrypto
LIBS_GL  := -lGL -lGLU -lglut

COMMON_SRC := $(wildcard src/common/*.c)
SERVER_SRC := $(wildcard src/server/*.c)
CLIENT_SRC := $(wildcard src/client/*.c)

COMMON_OBJ := $(COMMON_SRC:.c=.o)
SERVER_OBJ := $(SERVER_SRC:.c=.o)
CLIENT_OBJ := $(CLIENT_SRC:.c=.o)

all: server client

server: $(SERVER_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS_GL) $(LIBS_SSL)

client: $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS_GL) $(LIBS_SSL)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) server client

certs:
	mkdir -p config/clients
	$(OPENSSL) req -x509 -newkey rsa:2048 -nodes -days 3650 \
		-keyout config/ca.key -out config/ca.crt -subj "/CN=Project-3-CA" \
		-addext "basicConstraints=critical,CA:TRUE" \
		-addext "keyUsage=critical,keyCertSign,cRLSign"
	$(OPENSSL) req -newkey rsa:2048 -nodes \
		-keyout config/server.key -out config/server.csr -subj "/CN=update-server"
	$(OPENSSL) x509 -req -in config/server.csr -CA config/ca.crt -CAkey config/ca.key \
		-CAcreateserial -out config/server.crt -days 365 -sha256 \
		-extfile config/server_cert.ext
	for i in $$(seq 1 8); do \
		id=$$(printf "client_%03d" $$i); \
		$(OPENSSL) req -newkey rsa:2048 -nodes \
			-keyout config/clients/$$id.key \
			-out config/clients/$$id.csr -subj "/CN=$$id"; \
		$(OPENSSL) x509 -req -in config/clients/$$id.csr \
			-CA config/ca.crt -CAkey config/ca.key -CAcreateserial \
			-out config/clients/$$id.crt -days 365 -sha256 \
			-extfile config/client_cert.ext; \
	done

run_server: server
	./server $(SERVER_CONF)

run_client: client
	@trap '' INT; \
	(trap - INT; ./client $(CLIENT_CONF)); rc=$$?; \
	trap - INT; \
	if [ $$rc -eq 0 ]; then \
		exit 0; \
	elif [ $$rc -eq 130 ]; then \
		printf "\033[31mclient: stopped or paused by user\033[0m\n\n" >&2; \
		exit 0; \
	else \
		exit 0; \
	fi

.PHONY: all clean certs run_server run_client
