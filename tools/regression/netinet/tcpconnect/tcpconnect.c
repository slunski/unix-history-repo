/*-
 * Copyright (c) 2004 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(volid)
{

	fprintf(stderr, "tcpconnect server port\n");
	fprintf(stderr, "tcpconnect client ip port count\n");
	exit(-1);
}

static void
tcpconnect_server(int argc, char *argv[])
{
	int listen_sock, accept_sock;
	struct sockaddr_in sin;
	char *dummy;
	long port;

	if (argc != 1)
		usage();

	bzero(&sin, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	port = strtoul(argv[0], &dummy, 10);
	if (port < 1 || port > 65535 || *dummy != '\0')
		usage();
	sin.sin_port = htons(port);

	listen_sock = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_sock == -1) {
		perror("socket");
		exit(-1);
	}

	if (bind(listen_sock, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
		perror("bind");
		exit(-1);
	}

	if (listen(listen_sock, -1) == -1) {
		perror("listen");
		exit(1);
	}

	while (1) {
		accept_sock = accept(listen_sock, NULL, NULL);
		close(accept_sock);
	}
}

static void
tcpconnect_client(int argc, char *argv[])
{
	struct sockaddr_in sin;
	long count, i, port;
	char *dummy;
	int sock;

	if (argc != 3)
		usage();

	bzero(&sin, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	if (inet_aton(argv[0], &sin.sin_addr) == 0) {
		perror(argv[0]);
		exit(-1);
	}

	port = strtoul(argv[1], &dummy, 10);
	if (port < 1 || port > 65535 || *dummy != '\0')
		usage();
	sin.sin_port = htons(port);

	count = strtoul(argv[2], &dummy, 10);
	if (count < 1 || count > 100000 || *dummy != '\0')
		usage();

	for (i = 0; i < count; i++) {
		sock = socket(PF_INET, SOCK_STREAM, 0);
		if (sock == -1) {
			perror("socket");
			exit(-1);
		}

		if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
			perror("connect");
			exit(-1);
		}

		close(sock);
	}
}

int
main(int argc, char *argv[])
{

	if (argc < 2)
		usage();

	if (strcmp(argv[1], "server") == 0)
		tcpconnect_server(argc - 2, argv + 2);
	else if (strcmp(argv[1], "client") == 0)
		tcpconnect_client(argc - 2, argv + 2);
	else
		usage();

	exit(0);
}
