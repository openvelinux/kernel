#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <x86intrin.h>
#include "librpal/rpal.h"

#define SOCKET_PATH "/tmp/rpal_socket"
#define MAX_EVENTS 10
#define BUFFER_SIZE 1025
#define MSG_LEN 32

#define INIT_MSG "INIT"
#define SUCC_MSG "SUCC"
#define FAIL_MSG "FAIL"

#define handle_error(s)                                                        \
	do {                                                                   \
		perror(s);                                                     \
		exit(EXIT_FAILURE);                                            \
	} while (0)

uint64_t service_key;
int server_fd;
int epoll_fd;

int rpal_epoll_add(int epfd, int fd)
{
	struct epoll_event ev;

	ev.events = EPOLLRPALIN | EPOLLIN | EPOLLRDHUP | EPOLLET;
	ev.data.fd = fd;

	return rpal_epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void rpal_server_init(int fd, int epoll_fd)
{
	char buffer[BUFFER_SIZE];
	rpal_error_code_t err;
	uint64_t remote_key, service_key;
	int remote_id;
	int proc_fd;
	int ret;

	proc_fd = rpal_init(1, 0, &err);
	if (proc_fd < 0)
		handle_error("rpal init fail");
	rpal_get_service_key(&service_key);

	rpal_epoll_add(epoll_fd, fd);

	ret = read(fd, buffer, BUFFER_SIZE);
	if (ret < 0)
		handle_error("rpal init: read");

	if (strncmp(buffer, INIT_MSG, strlen(INIT_MSG)) != 0) {
		buffer[BUFFER_SIZE - 1] = 0;
		handle_error("Invalid msg\n");
		return;
	}

	remote_key = *(uint64_t *)(buffer + strlen(INIT_MSG));
	ret = rpal_request_service(remote_key);
	if (ret) {
		uint64_t service_key = 0;
		ret = write(fd, (char *)&service_key, sizeof(uint64_t));
		handle_error("request service fail");
		return;
	}
	ret = write(fd, (char *)&service_key, sizeof(uint64_t));
	if (ret < 0)
		handle_error("write error");

	ret = read(fd, buffer, BUFFER_SIZE);
	if (ret < 0)
		handle_error("handshake read");

	if (strncmp(SUCC_MSG, buffer, strlen(SUCC_MSG)) != 0)
		handle_error("handshake");

	remote_id = rpal_get_request_service_id(remote_key);
	if (remote_id < 0)
		handle_error("remote id get fail");
	rpal_thread_init();
}

void run_rpal_server(int msg_len)
{
	struct epoll_event ev, events[MAX_EVENTS];
	int new_socket;
	int nfds;
	uint64_t tsc, total_tsc = 0;
	int count = 0;

	while (1) {
		nfds = rpal_epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		for (int n = 0; n < nfds; ++n) {
			if (events[n].data.fd == server_fd) {
				new_socket = accept(server_fd, NULL, NULL);
				if (new_socket == -1) {
					perror("accept");
					continue;
				}

				rpal_server_init(new_socket, epoll_fd);
			} else if (events[n].events & EPOLLRDHUP) {
				close(events[n].data.fd);
				goto finish;
			} else if (events[n].events & EPOLLRPALIN) {
				char buffer[BUFFER_SIZE] = { 0 };

				ssize_t valread = rpal_read_ptrs(
					events[n].data.fd, (int64_t *)buffer,
					MSG_LEN / sizeof(int64_t *));
				if (valread <= 0) {
					close(events[n].data.fd);
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
						  events[n].data.fd, NULL);
					goto finish;
				} else {
					count++;
					sscanf(buffer, "0x%016lx", &tsc);
					total_tsc += __rdtsc() - tsc;
					send(events[n].data.fd, buffer, msg_len,
					     0);
				}
			} else {
				perror("bad request\n");
			}
		}
	}
finish:
	printf("RPAL: Message length: %d bytes, Total TSC cycles: %lu,"
	       "Message count: %d, Average latency: %lu cycles\n",
	       MSG_LEN, total_tsc, count, total_tsc / count);
}

void run_server(int msg_len)
{
	struct epoll_event ev, events[MAX_EVENTS];
	int new_socket;
	int nfds;
	uint64_t tsc, total_tsc = 0;
	int count = 0;

	while (1) {
		nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		for (int n = 0; n < nfds; ++n) {
			if (events[n].data.fd == server_fd) {
				new_socket = accept(server_fd, NULL, NULL);
				if (new_socket == -1) {
					perror("accept");
					continue;
				}

				ev.events = EPOLLIN;
				ev.data.fd = new_socket;
				if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
					      new_socket, &ev) == -1) {
					close(new_socket);
					perror("epoll_ctl: add new socket");
				}
			} else if (events[n].events & EPOLLRDHUP) {
				close(events[n].data.fd);
				goto finish;
			} else {
				char buffer[BUFFER_SIZE] = { 0 };

				ssize_t valread = read(events[n].data.fd,
						       buffer, BUFFER_SIZE);
				if (valread <= 0) {
					close(events[n].data.fd);
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
						  events[n].data.fd, NULL);
					goto finish;
				} else {
					count++;
					sscanf(buffer, "0x%016lx", &tsc);
					total_tsc += __rdtsc() - tsc;
					send(events[n].data.fd, buffer, msg_len,
					     0);
				}
			}
		}
	}
finish:
	printf("EPOLL: Message length: %d bytes, Total TSC cycles: %lu,"
	       "Message count: %d, Average latency: %lu cycles\n",
	       MSG_LEN, total_tsc, count, total_tsc / count);
}

int main()
{
	struct sockaddr_un address;
	struct epoll_event ev;

	if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == 0) {
		perror("socket failed");
		exit(EXIT_FAILURE);
	}

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, SOCKET_PATH, sizeof(SOCKET_PATH));

	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind failed");
		exit(EXIT_FAILURE);
	}

	if (listen(server_fd, 3) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	epoll_fd = epoll_create(1024);
	if (epoll_fd == -1) {
		perror("epoll_create");
		exit(EXIT_FAILURE);
	}

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
		perror("epoll_ctl: listen_sock");
		exit(EXIT_FAILURE);
	}

	run_server(MSG_LEN);
	run_rpal_server(MSG_LEN);

	close(server_fd);
	unlink(SOCKET_PATH);
	return 0;
}
