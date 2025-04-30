#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <x86intrin.h>
#include "librpal/rpal.h"

#define SOCKET_PATH "/tmp/rpal_socket"
#define BUFFER_SIZE 1025
#define MSG_NUM 1000000
#define MSG_LEN 32

char hello[BUFFER_SIZE];
char buffer[BUFFER_SIZE] = { 0 };

int remote_id;
uint64_t remote_sidfd;

#define INIT_MSG "INIT"
#define SUCC_MSG "SUCC"
#define FAIL_MSG "FAIL"

#define handle_error(s)                                                        \
	do {                                                                   \
		perror(s);                                                     \
		exit(EXIT_FAILURE);                                            \
	} while (0)

int rpal_epoll_add(int epfd, int fd)
{
	struct epoll_event ev;

	ev.events = EPOLLRPALIN | EPOLLIN | EPOLLRDHUP | EPOLLET;
	ev.data.fd = fd;

	return rpal_epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void rpal_client_init(int fd)
{
	struct epoll_event ev;
	char buffer[BUFFER_SIZE];
	rpal_error_code_t err;
	uint64_t remote_key, service_key;
	int epoll_fd;
	int proc_fd;
	int ret;

	proc_fd = rpal_init(1, 0, &err);
	if (proc_fd < 0)
		handle_error("rpal init fail");
	rpal_get_service_key(&service_key);

	strcpy(buffer, INIT_MSG);
	*(uint64_t *)(buffer + strlen(INIT_MSG)) = service_key;
	ret = write(fd, buffer, strlen(INIT_MSG) + sizeof(uint64_t));
	if (ret < 0)
		handle_error("write key");

	ret = read(fd, buffer, BUFFER_SIZE);
	if (ret < 0)
		handle_error("read key");

	memcpy(&remote_key, buffer, sizeof(remote_key));
	if (remote_key == 0)
		handle_error("remote down");

	ret = rpal_request_service(remote_key);
	if (ret) {
		write(fd, FAIL_MSG, strlen(FAIL_MSG));
		handle_error("request");
	}

	ret = write(fd, SUCC_MSG, strlen(SUCC_MSG));
	if (ret < 0)
		handle_error("handshake");

	remote_id = rpal_get_request_service_id(remote_key);
	rpal_sender_init(&err);

	epoll_fd = epoll_create(1024);
	if (epoll_fd == -1) {
		perror("epoll_create");
		exit(EXIT_FAILURE);
	}
	rpal_epoll_add(epoll_fd, fd);

	sleep(3); //wait for epoll wait
	ret = rpal_uds_fdmap(((unsigned long)remote_id << 32) | fd,
			     &remote_sidfd);
	if (ret < 0)
		handle_error("uds fdmap fail");
}

int run_rpal_client(int msg_len)
{
	ssize_t valread;
	int sock = 0;
	struct sockaddr_un serv_addr;
	int count = MSG_NUM;
	int ret;

	// rpal_init();
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("socket creation error");
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sun_family = AF_UNIX;
	strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(SOCKET_PATH));

	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) <
	    0) {
		perror("Connection Failed");
		return -1;
	}
	rpal_client_init(sock);

	while (count) {
		for (int i = 18; i < msg_len; i++)
			hello[i] = 'a' + i % 26;
		sprintf(hello, "0x%016lx", __rdtsc());
		ret = rpal_write_ptrs(remote_id, remote_sidfd, (int64_t *)hello,
				      msg_len / sizeof(int64_t *));
		valread = read(sock, buffer, BUFFER_SIZE);
		if (memcmp(hello, buffer, msg_len) != 0)
			perror("data error");
		count--;
	}

	close(sock);
	printf("rpal_client finish\n");
}

int run_client(int msg_len)
{
	ssize_t valread;
	int sock = 0;
	struct sockaddr_un serv_addr;
	int count = MSG_NUM;

	// rpal_init();
	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("socket creation error");
		return -1;
	}

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sun_family = AF_UNIX;
	strncpy(serv_addr.sun_path, SOCKET_PATH, sizeof(SOCKET_PATH));

	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) <
	    0) {
		perror("Connection Failed");
		return -1;
	}

	while (count) {
		for (int i = 18; i < msg_len; i++)
			hello[i] = 'a' + i % 26;
		sprintf(hello, "0x%016lx", __rdtsc());
		send(sock, hello, msg_len, 0);
		valread = read(sock, buffer, BUFFER_SIZE);
		if (memcmp(hello, buffer, msg_len) != 0)
			perror("data error");
		count--;
	}

	close(sock);
	printf("client finish\n");
}

int main()
{
	run_client(MSG_LEN);
	run_rpal_client(MSG_LEN);

	return 0;
}
