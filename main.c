#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <error.h>
#include <execinfo.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <poll.h>
#include <arpa/inet.h>

#define SOCK_TYPE AF_INET
#define LISTEN_ADDRESS "199.180.133.213"
#define LISTEN_PORT 80
// #define SOCK_TYPE AF_UNIX

#if !defined(likely)
#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__) || (defined(__IBMC__) || defined(__IBMCPP__))
#define likely(x)       __builtin_expect(!!(x),1)
#define unlikely(x)     __builtin_expect(!!(x),0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif
#endif

#define macrobacktrace() { \
	void *array[20]; \
	int traces=backtrace(array,sizeof(array)/sizeof(array[0])); \
	if(traces<=0) { \
		fprintf(stderr,"failed to get a backtrace!"); \
	} else { \
		backtrace_symbols_fd(array,traces,STDERR_FILENO); \
	} \
	fflush(stderr); \
}
#define myerror(status,errnum,...){macrobacktrace();error_at_line(status,errnum,__FILE__,__LINE__,__VA_ARGS__);}
#define nf(in,str){ \
	if(unlikely(!(in))) { \
		myerror(1,errno,"error with %s()",str); \
		exit(errno);    \
	} \
}
/*
 * this function is stolen from android's bionic, sorry guys.
 * This uses the "Not So Naive" algorithm, a very simple but
 * usually effective algorithm, see:
 * http://www-igm.univ-mlv.fr/~lecroq/string/
 */
void *memmem(const void *haystack, size_t n, const void *needle, size_t m)
{
	if (m > n || !m || !n)
		return NULL;
	if (likely(m > 1))
	{
		const unsigned char* y = (const unsigned char*) haystack;
		const unsigned char* x = (const unsigned char*) needle;
		size_t j = 0;
		size_t k = 1, l = 2;
		if (x[0] == x[1])
		{
			k = 2;
			l = 1;
		}
		while (j <= n - m)
		{
			if (x[1] != y[j + 1])
			{
				j += k;
			}
			else
			{
				if (!memcmp(x + 2, y + j + 2, m - 2) && x[0] == y[j])
					return (void*) &y[j];
				j += l;
			}
		}
	}
	else
	{
		/* degenerate case */
		return memchr(haystack, ((unsigned char*) needle)[0], n);
	}
	return NULL;
}

static bool writeAll(const int cid, const void *buf, const size_t len)
{
	size_t written = 0;
	while (written < len)
	{
		ssize_t written_last = write(cid, (void*) ((intptr_t) buf + written),
				len - written);
		if (written_last == -1)
		{
			myerror(0, errno,
					"could only write %lu/%lu bytes of response for client (will proceed to disconnect client)",
					written, len);
			return false;
		}
		written += written_last;
	}
	return true;
}
static void* connectionHandler(void *cid_in)
{
	//printf(">%i<", __LINE__);
	const int cid = *(int*) cid_in;
	free(cid_in);
	char buf[500]; ///
	struct linger l =
	{ .l_onoff = 1, .l_linger = 100 };
	nf(setsockopt(cid, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) != -1,
			"setsockopt SO_LINGER");
	// FIXME: if the end of headers is at sizeof(buf), so half of \r\n\r\n is at first recv()
	// and the other half is at the 2nd recv(), this code wont work.
	bool sent100Continue = false;
	bool recievedExpect100 = false;
	bool allHeadersRecieved = false;
	bool sentHeaders = false;
//	if (writeAll(cid, response, sizeof(response) - 1))
	{
		struct pollfd fds =
		{ .fd = cid, .events = POLLIN };
		for (;;)
		{
			int pres = poll(&fds, 1, 500);
			if (pres == 0)
			{
				break; // timeout
			}
			if (pres == -1)
			{
				if (errno == EINTR || errno == EAGAIN)
				{
					continue;
				}
				else
				{
					// a bunch of errors can cause this, we don't really care which specific error.
					break;
				}
			}
			if ((fds.revents & POLLERR) || (fds.revents & POLLHUP))
			{
				// some error
				break;
			}
			ssize_t read_last;
			do
			{
				read_last = recv(cid, buf, sizeof(buf), MSG_DONTWAIT);
				if (read_last != -1)
				{
					break;
				}
			} while (errno == EAGAIN /* ? || errno == EWOULDBLOCK */);
			if (read_last == -1)
			{
				// some error reading, don't care which.
				break;
			}
			if (!sentHeaders && !allHeadersRecieved)
			{
				const char x100Continue[] = "Expect: 100-continue\r\n";
				if (!recievedExpect100
						&& memmem(buf, read_last, x100Continue,
								sizeof(x100Continue) - 1))
				{
					recievedExpect100 = true;
				}
				if (memmem(buf, read_last, "\r\n\r\n", 4))
				{
					allHeadersRecieved = true;
				}
				// fixme: if not all headers are received, we should
				// read more data before sending response headers
				// (because we don't know if a Expect: 100  will come or not)
				if (recievedExpect100)
				{
					const char x100Continue[] = "HTTP/1.0 100 Continue\r\n";
					if (!writeAll(cid, x100Continue, sizeof(x100Continue) - 1))
					{
						break;
					}
					sent100Continue = true;
				}
				const char response[] = "HTTP/1.0 200 OK\r\n"
						"Content-Type: text/plain\r\n"
						"Connection: close\r\n"
						"Cache-Control: no-cache, no-store, must-revalidate\r\n"
						"\r\n";
				if (!writeAll(cid, response, sizeof(response) - 1))
				{
					break;
				}
				sentHeaders = true;
			}
			if (!writeAll(cid, buf, read_last))
			{
				// also some error, note that writeAll is synchronous and won't quit until
				// everything is written, or an error, so this is an error, don't care which.
				break;
			}
		}
	}
	nf(close(cid) != -1, "close");
	return NULL;
}

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "usage: %s socket_path\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	const char *master_socket = argv[1];
	if (strlen(master_socket) >= sizeof((struct sockaddr_un
			)
			{ 0 } .sun_path))
	{
		fprintf(stderr,
				"error: socket name is too long, cannot be longer than %li\n",
				sizeof((struct sockaddr_un
						)
						{ 0 } .sun_path));
		exit(EXIT_FAILURE);
	}
#if SOCK_TYPE == AF_UNIX
	const int master = socket(AF_UNIX, SOCK_STREAM, 0);
	// printf("master: %i\n", master);
	nf(master != -1, "socket");
	{
		struct sockaddr_un master_addr =
		{	.sun_family = AF_UNIX};
		strncpy(master_addr.sun_path, master_socket,
				sizeof(master_addr.sun_path) - 1);
		nf(
				bind(master, (struct sockaddr* ) &master_addr,
						sizeof(master_addr)) != -1, "bind");
		nf(listen(master, 100) != -1, "listen");
		printf("listening on: %s\n", master_addr.sun_path);
	}
#elif SOCK_TYPE == AF_INET
	const int master = socket(AF_INET, SOCK_STREAM, SOL_TCP);
	nf(master != -1, "socket");
	{
		int reuse = 1;
		if (setsockopt(master, SOL_SOCKET, SO_REUSEADDR, (const char*) &reuse,
				sizeof(reuse)) < 0)
			perror("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
		if (setsockopt(master, SOL_SOCKET, SO_REUSEPORT, (const char*) &reuse,
				sizeof(reuse)) < 0)
			perror("setsockopt(SO_REUSEPORT) failed");
#endif

		uint32_t addri;
		// TODO: error detection
		inet_pton(AF_INET,LISTEN_ADDRESS,&addri);
		struct sockaddr_in master_addr =
		{ .sin_family = AF_INET, .sin_port = htons(LISTEN_PORT), .sin_addr.s_addr =
				addri /*htonl(INADDR_ANY)*/ };
		nf(
				bind(master, (struct sockaddr* )&master_addr,
						sizeof(master_addr)) != -1, "bind");
		nf(listen(master, 100) != -1, "listen");
		printf("listening on %s:%i\n",LISTEN_ADDRESS,LISTEN_PORT);
	}
#else
#error "SOCK_TYPE must be defined as either AF_UNIX for unix sockets, or AF_INET for ipv4 connections"
#endif
	while (1)
	{
		int *newc_arg = malloc(sizeof(int));
		nf(newc_arg, "malloc");
		int newc = accept(master, NULL, NULL);
		nf(newc != -1, "accept");
		*newc_arg = newc;
		pthread_t thread_id;
		nf(pthread_create(&thread_id, NULL, &connectionHandler, newc_arg)==0,
				"pthread_create");
		nf(pthread_detach(thread_id) == 0, "pthread_detach");
		printf("new connection! %i\n", newc);
	}
	return 0;
}
