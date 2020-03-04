/*
 * Task management server.
 *
 * By Alex Belits <abelits@marvell.com>
 *
 * This is an implementation of a server using AF_UNIX socket.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include "isol-server.h"

/* Maximum number of clients */
#define NCLIENTS		50

/* Buffer size */
#define INIT_BUF_SIZE		4096

/* Index of the socket fd */
#define SOCKFD_INDEX		0

/* Total number of fixed fds */
#define FIXED_FD_INDEXES	1

#define CLIENT_FLAG_INVALID	1
#define CLIENT_FLAG_CLOSE	2

static struct pollfd fds[NCLIENTS + FIXED_FD_INDEXES];
static int nfds = 0, pending_data_flag = 0;

static int (*client_line_handler)(int client_index, const char *line) = NULL;
static int (*client_connect_handler)(int client_index) = NULL;
static int (*client_disconnect_handler)(int client_index) = NULL;

struct client_desc {
	char *input_buffer;
	size_t input_buffer_len;
	size_t input_buffer_alloc;
	char *output_buffer;
	off_t output_buffer_pos_wr;
	off_t output_buffer_pos_rd;
	size_t output_buffer_alloc;
	int flags;
	void *task;
};

static struct client_desc *isol_server_clients[NCLIENTS];

#ifndef HAVE_RENAMEAT2
static inline int renameat2(int olddirfd, const char *oldpath,
			    int newdirfd, const char *newpath,
			    unsigned int flags)
{
	return syscall(SYS_renameat2, olddirfd, oldpath, newdirfd,
		       newpath, flags);
}
#endif

/*
  Set client line handler.
*/
void set_client_line_handler(int (*handler)(int client_index,
					    const char *line))
{
	client_line_handler = handler;
}

/*
  Set client connect handler.
*/
void set_client_connect_handler(int (*handler)(int client_index))
{
	client_connect_handler = handler;
}

/*
  Set client disconnect handler.
*/
void set_client_disconnect_handler(int (*handler)(int client_index))
{
	client_disconnect_handler = handler;
}

/*
  Return nonzero if there is data to be sent to the clients.
*/
int is_pending_data_present(void)
{
	return pending_data_flag;
}

static int get_client_flags(int client_index)
{
	if (isol_server_clients[client_index] == NULL)
		return CLIENT_FLAG_INVALID;
	else
		return isol_server_clients[client_index]->flags;
}

static void set_client_flags(int client_index, int value)
{
	if (isol_server_clients[client_index] != NULL)
		isol_server_clients[client_index]->flags |= value;
}

#if 0
static void clr_client_flags(int client_index, int value)
{
	if (isol_server_clients[client_index] != NULL)
		isol_server_clients[client_index]->flags &= ~value;
}
#endif
/*
  Close client connection after all data is sent.
*/
void close_client_connection(int client_index)
{
	set_client_flags(client_index, CLIENT_FLAG_CLOSE);
}

static int create_client_desc(int client_index)
{
	isol_server_clients[client_index] =
		(struct client_desc*) malloc(sizeof(struct client_desc));
	if (isol_server_clients[client_index] == NULL)
		return -1;
	memset(isol_server_clients[client_index], 0,
	       sizeof(struct client_desc));

	isol_server_clients[client_index]->input_buffer
		= (char *)malloc(INIT_BUF_SIZE);
	if (isol_server_clients[client_index]->input_buffer == NULL) {
		free(isol_server_clients[client_index]);
		isol_server_clients[client_index] = NULL;
		return -1;
	}

	isol_server_clients[client_index]->output_buffer
		= (char *)malloc(INIT_BUF_SIZE);
	if (isol_server_clients[client_index]->output_buffer == NULL) {
		free(isol_server_clients[client_index]->input_buffer);
		free(isol_server_clients[client_index]);
		isol_server_clients[client_index] = NULL;
		return -1;
	}

	isol_server_clients[client_index]->input_buffer_alloc = INIT_BUF_SIZE;
	isol_server_clients[client_index]->output_buffer_alloc = INIT_BUF_SIZE;
	return 0;
}

static void delete_client_desc(int client_index)
{
	int i;

	if (isol_server_clients[client_index] == NULL)
		return;
	if (isol_server_clients[client_index]->output_buffer != NULL)
		free(isol_server_clients[client_index]->output_buffer);
	if (isol_server_clients[client_index]->input_buffer != NULL)
		free(isol_server_clients[client_index]->input_buffer);
	free(isol_server_clients[client_index]);

	for (i = client_index; i < (nfds - FIXED_FD_INDEXES - 1); i++)
		isol_server_clients[i] = isol_server_clients[i + 1];
}


void *get_client_task(int client_index)
{
	return isol_server_clients[client_index]->task;
}

void set_client_task(int client_index, void *task)
{
	isol_server_clients[client_index]->task = task;
}

int get_client_index(void *task)
{
	int i;
	if (task == NULL)
		return -1;

	for (i = 0; i < (nfds - FIXED_FD_INDEXES - 1); i++) {
		if (isol_server_clients[i]->task == task)
			return i;
	}
	return -1;
}

void clear_kv_rx(struct kv_rx *kvpairs)
{
	if (kvpairs->set) {
		if (kvpairs->val_type == KV_TYPE_STRING) {
			free(kvpairs->val.val_ptr);
			kvpairs->val.val_ptr = NULL;
		}
		kvpairs->set = 0;
	}
}


static void process_kvpair_line(struct kv_rx *kvpairs, char *line)
{
	int i, j;
	char *eq, *valstr;

	if ((line == NULL) || (kvpairs == NULL))
		return;

	eq = strchr(line, '=');
	if (eq == NULL)
		return;

	for (i = 0; kvpairs[i].key != NULL; i++) {
		if ((strlen(kvpairs[i].key) == (size_t)(eq - line))
		    && !memcmp(kvpairs[i].key, line, (size_t)(eq - line))) {
			/* Key matches */
			valstr = eq + 1;
			switch (kvpairs[i].val_type) {
			case KV_TYPE_INT:
				kvpairs[i].val.val_int =
					strtol(valstr, NULL, 0);
				kvpairs[i].set = 1;
				break;
			case KV_TYPE_ENUM:
				for (j = 0;
				     (j >= 0)
					&& (kvpairs[i].enum_strings[j]
					    != NULL);
				     j++) {
					if (!strcmp(valstr,
						kvpairs[i].enum_strings[j])) {
						kvpairs[i].val.val_int = j;
						j = -2;
						kvpairs[i].set = 1;
					}
				}
				break;
			case KV_TYPE_STRING:
				if (kvpairs[i].set)
					free(kvpairs[i].val.val_ptr);
				kvpairs[i].val.val_ptr = strdup(valstr);
				if (kvpairs[i].val.val_ptr != NULL)
					kvpairs[i].set = 1;
				break;
			}
		}
	}
}

int init_rx_buffer(struct rx_buffer *rx)
{
	rx->input_buffer = (char *)malloc(INIT_BUF_SIZE);
	if (rx->input_buffer == NULL)
		return -1;
	rx->input_buffer_alloc = INIT_BUF_SIZE;
	rx->input_buffer_len = 0;
	return 0;
}

void free_rx_buffer(struct rx_buffer *rx)
{
	free(rx->input_buffer);
}

int read_rx_data(struct rx_buffer *rx, int fd, struct kv_rx *kvpairs)
{
	ssize_t avail_size, received;
	unsigned input_buffer_pos, i;
	int rcode_value = -1;
	char *line, *endline;
	int cont;

	do {
		avail_size = rx->input_buffer_alloc
			- rx->input_buffer_len;

		if (avail_size <= 0)
			return -1;

	    do {
		    received = read(fd,
				    &(rx->input_buffer[rx->input_buffer_len]),
				    avail_size);
	    }
	    while ((received < 0) && ((errno == EINTR) || (errno == EAGAIN)));

	    if (received < 0)
		    received = 0;

	    if (received == 0)
		    return -1;

	    input_buffer_pos = 0;
	    i = rx->input_buffer_len;
	    rx->input_buffer_len += received;

	    while (i < rx->input_buffer_len) {
		    while ((rx->input_buffer[i] != '\n')
			   && (i < rx->input_buffer_len))
			    i++;
		    if (i < rx->input_buffer_len) {
			    rx->input_buffer[i] = '\0';
			    line = &rx-> input_buffer[input_buffer_pos];
			    endline = &rx->input_buffer[i];
			    if (((endline - line) >= 4)
				&& (line[0] >= '0') && (line[0] <='9')
				&& (line[1] >= '0') && (line[1] <= '9')
				&& (line[2] >= '0') && (line[2] <= '9')
				&& ((line[3] == ' ') || (line[3] == '-'))) {
				    /* Formatted line with response code. */
				    *endline = '\0';
				    rcode_value =
					    ((unsigned int)(line[0] - '0'))
					    * 100
					    + ((unsigned int)(line[1] - '0'))
					    * 10
					    + ((unsigned int)(line[2] - '0'));
				    cont = (line[3] == '-');
				    line += 4;
				    process_kvpair_line(kvpairs, line);
			    }
			    i++;
			    input_buffer_pos = i;
		    }
	    }
	    if (input_buffer_pos != 0) {
		    rx->input_buffer_len -= input_buffer_pos;
		    if (rx->input_buffer_len) {
			    memmove(&(rx->input_buffer[0]),
				    &(rx->input_buffer[input_buffer_pos]),
				    rx->input_buffer_len);
		    }
	    }
	}
	while (cont);
	return rcode_value;
}

static ssize_t read_client_data(int client_index, int fd)
{
	struct client_desc *isol_server_client;
	ssize_t avail_size, received;
	unsigned input_buffer_pos, i;

	isol_server_client = isol_server_clients[client_index];
	if (isol_server_client == NULL) {
		fprintf(stderr, "Client descriptor is not allocated "
			"while receiving data\n");
		return 0;
	}
	avail_size = isol_server_client->input_buffer_alloc
		- isol_server_client->input_buffer_len;

	if (avail_size <= 0)
		return 0;

	do {
		received = read(fd, &(isol_server_client->
				      input_buffer[isol_server_client->
					       input_buffer_len]),
				avail_size);
	}
	while ((received < 0) && ((errno == EINTR) || (errno == EAGAIN)));

	if (received < 0)
		received = 0;

	input_buffer_pos = 0;
	i = isol_server_client->input_buffer_len;
	isol_server_client->input_buffer_len += received;

	while (i < isol_server_client->input_buffer_len) {
		while ((isol_server_client->input_buffer[i] != '\n')
		       && (i < isol_server_client->input_buffer_len))
			i++;
		if (i < isol_server_client->input_buffer_len) {
			isol_server_client->input_buffer[i] = '\0';
			if ((client_line_handler != NULL)
			    && ((get_client_flags(client_index)
				 & CLIENT_FLAG_CLOSE) == 0))
				client_line_handler(client_index,
						    &(isol_server_client->
					input_buffer[input_buffer_pos]));
			i++;
			input_buffer_pos = i;
		}
	}
	if (input_buffer_pos != 0) {
		isol_server_client->input_buffer_len -= input_buffer_pos;
		if (isol_server_client->input_buffer_len) {
			memmove(&(isol_server_client->
				  input_buffer[0]),
				&(isol_server_client->
				  input_buffer[input_buffer_pos]),
				isol_server_client->input_buffer_len);
		}
	}
	return received;
}

/*
  Return the size of data buffered for sending.
*/
static ssize_t size_client_pending_data(int client_index)
{
	struct client_desc *isol_server_client;

	isol_server_client = isol_server_clients[client_index];
	if (isol_server_client == NULL) {
		fprintf(stderr, "Client descriptor is not allocated "
			"while sending data\n");
		return 0;
	}
	if (isol_server_client->output_buffer_pos_wr
	    >= isol_server_client->output_buffer_pos_rd)
		return isol_server_client->output_buffer_pos_wr
			- isol_server_client->output_buffer_pos_rd;
	else
		return isol_server_client->output_buffer_alloc
			- isol_server_client->output_buffer_pos_rd
			+ isol_server_client->output_buffer_pos_wr;
}

/*
  Clear data buffered for sending.
*/
static void clear_client_pending_data(int client_index)
{
	struct client_desc *isol_server_client;

	isol_server_client = isol_server_clients[client_index];
	if (isol_server_client == NULL) {
		fprintf(stderr, "Client descriptor is not allocated "
			"while clearing data\n");
	} else {
		isol_server_client->output_buffer_pos_wr = 0;
		isol_server_client->output_buffer_pos_rd = 0;
	}
}

/*
  Send pending data to the client.
*/
static ssize_t send_client_pending_data(int client_index, int fd)
{
	struct client_desc *isol_server_client;
	struct iovec iov[2];
	ssize_t sent;

	/* Get the client descriptor. */
	isol_server_client = isol_server_clients[client_index];
	if (isol_server_client == NULL) {
		fprintf(stderr, "Client descriptor is not allocated "
			"while sending data\n");
		return 0;
	}

	if (isol_server_client->output_buffer_pos_wr
	    >= isol_server_client->output_buffer_pos_rd) {
		/* Single chunk of data in the buffer, send it. */
		sent = write(fd, &(isol_server_client->
				   output_buffer[isol_server_client->
						 output_buffer_pos_rd]),
			     isol_server_client->output_buffer_pos_wr
			     - isol_server_client->output_buffer_pos_rd);
		/* On success update the read offset. */
		if (sent > 0)
			isol_server_client->output_buffer_pos_rd += sent;
	} else {
		/* Two chunks of data in the buffer, send both of them. */
		iov[0].iov_base = &(isol_server_client->
				    output_buffer[isol_server_client->
						  output_buffer_pos_rd]);
		iov[0].iov_len = isol_server_client->output_buffer_alloc
			- isol_server_client->output_buffer_pos_rd;
		iov[1].iov_base = &(isol_server_client->
				    output_buffer[0]);
		iov[1].iov_len = isol_server_client->output_buffer_pos_wr;
		sent = writev(fd, iov, 2);
		if (sent > 0) {
			/* Update the read offset, handle the wraparound. */
			if ((size_t)sent < iov[0].iov_len)
				isol_server_client->output_buffer_pos_rd +=
					sent;
			else
				isol_server_client->output_buffer_pos_rd =
					sent - iov[0].iov_len;
		}
	}
	return sent;
}

/*
  Send data to the client. As much as possible will be sent
  immediately, the rest is buffered. This function may produce short
  write if there is no sufficient space in the buffer.
*/
ssize_t send_data_nonblock(int client_index, const char *data, size_t size)
{
	struct client_desc *isol_server_client;
	ssize_t sent, rv, avail_1, avail_2;
	int fd;

	/* Get the client descriptor. */
	isol_server_client = isol_server_clients[client_index];
	if (isol_server_client == NULL) {
		fprintf(stderr, "Client descriptor is not allocated "
			"while sending data\n");
		return 0;
	}

	fd = fds[client_index + FIXED_FD_INDEXES].fd;

	/* Try to send pending data. */
	if (size_client_pending_data(client_index) > 0) {
		if ((rv = send_client_pending_data(client_index, fd)) < 0) {
			if ((errno != EINTR)
			    && (errno != EAGAIN)
			    && (errno != EWOULDBLOCK))
				return rv;
		}
	}

	/* If there is no pending data left, try to send everything directly. */
	if (size_client_pending_data(client_index) == 0) {
		/* Nothing is pending, clear POLLOUT event. */
		fds[client_index + FIXED_FD_INDEXES].events &= ~POLLOUT;

		sent = write(fd, data, size);

		/*
		  Return on permanent errors.
		  Transient errors at this point do not affect the
		  outcome, so just report that nothing was sent and
		  continue.
		*/
		if (sent < 0) {
			if ((errno != EINTR)
			    && (errno != EAGAIN)
			    && (errno != EWOULDBLOCK))
				return sent;
			else
				sent = 0;
		}
		/* Update the source pointer and size to reflect sent data. */
		size -= sent;
		data += sent;
	} else
		sent = 0;

	/* If something remained to be sent, place it into the buffer. */
	if (size > 0) {
		if (isol_server_client->output_buffer_pos_rd
		    > isol_server_client->output_buffer_pos_wr) {
			/*
			  One contiguous free area in the ring buffer,
			  mark it as available except for one reserved
			  byte before the read offset.
			*/
			avail_1 = isol_server_client->output_buffer_pos_rd
				- isol_server_client->output_buffer_pos_wr - 1;
			avail_2 = 0;
		} else {
			/* Two free areas in the ring buffer. */
			avail_1 = isol_server_client->output_buffer_alloc
				- isol_server_client->output_buffer_pos_wr;
			avail_2 = isol_server_client->output_buffer_pos_rd;
			/*
			  If read offset is at the start of the buffer,
			  reserve the last byte in the first free area,
			  otherwise reserve it before the read offset.
			*/
			if (avail_2 > 0)
				avail_2--;
			else
				avail_1--;
		}
		if (size < (size_t)avail_1) {
			/*
			  Data does not completely fill the first
			  available area.
			*/
			memcpy(&(isol_server_client->
				 output_buffer[isol_server_client->
					       output_buffer_pos_wr]),
			       data, size);
			/* Update write offset, record data as "sent". */
			isol_server_client->
				output_buffer_pos_wr += size;
			sent += size;
			/*
			  No need to update source pointer and size, this
			  is the last write/buffering operation.
			*/
		} else {
			/*
			  Data completely fills the first available area,
			  and may spill into the second one.
			*/
			memcpy(&(isol_server_client->
				 output_buffer[isol_server_client->
					       output_buffer_pos_wr]),
			       data, avail_1);
			/*
			  Update the source pointer and size to reflect
			  buffered data.
			*/
			data += avail_1;
			size -= avail_1;
			/* Update write offset, record data as "sent". */
			isol_server_client->
				output_buffer_pos_wr += avail_1;
			sent += avail_1;

			/*
			  Wrap around write offset if it reached the end
			  of the buffer.
			*/
			if (isol_server_client->output_buffer_pos_wr ==
			    (ssize_t)isol_server_client->output_buffer_alloc)
				isol_server_client->output_buffer_pos_wr = 0;

			if ((size > 0) && (avail_2 > 0)) {
				/*
				  Some data is left, and there is a second
				  available area.
				*/
				if (size < (size_t)avail_2) {
					/* Copy remaining data. */
					memcpy(&(isol_server_client->
						 output_buffer[0]), data, size);
					/*
					  Update write offset, record
					  data as "sent".
					*/
					isol_server_client->
						output_buffer_pos_wr = size;
					sent += size;
				} else {
					/*
					  Copy whatever fits into the
					  remaining available area.
					*/
					memcpy(&(isol_server_client->
						 output_buffer[0]), data,
					       avail_2);
					/*
					  Update write offset, record
					  data as "sent".
					*/
					isol_server_client->
						output_buffer_pos_wr = avail_2;
					sent += avail_2;
				}
				/*
				  No need to update source pointer and size,
				  this is the last write/buffering operation.
				*/
			}
		}
		/* Data buffered, set POLLOUT event to be processed. */
		fds[client_index + FIXED_FD_INDEXES].events |= POLLOUT;
	}

	return sent;
}

void tx_init(struct tx_text *tx)
{
	tx->first = NULL;
	tx->last = NULL;
}

int tx_add_text(struct tx_text *tx, char *text)
{
	size_t l;
	struct tx_text_chunk *chunk;

	l = strlen(text);
	chunk = (struct tx_text_chunk *)malloc(sizeof(struct tx_text_chunk)
					       + l + 1);
	if (chunk == NULL)
		return -1;

	chunk->buffer = ((unsigned char*)chunk) + sizeof(struct tx_text_chunk);
	memcpy(chunk->buffer, text, l);
	chunk->buffer[l] = '\0';
	chunk->size = l;
	chunk->next = NULL;
	if (tx->last == NULL) {
		tx->first = chunk;
		tx->last = chunk;
	} else {
		tx->last->next = chunk;
		tx->last = chunk;
	}
	return 0;
}

int tx_add_text_num(struct tx_text *tx, long v)
{
	char s[22];
	sprintf(s, "%ld", v);
	return tx_add_text(tx, s);
}

int send_tx_persist(int client_index, struct tx_text *tx)
{
	struct tx_text_chunk *chunk, *last_chunk;
	unsigned char *data;
	ssize_t sent;
	int rv;

	rv = 0;
	for (chunk = tx->first; chunk != NULL;
	     chunk = chunk ? chunk->next : NULL) {
		data = chunk->buffer;
		while ((chunk != NULL)
		       && (data < (chunk->buffer + chunk->size))) {
			sent = send_data_nonblock(client_index, (char *)data,
						  (chunk->buffer + chunk->size)
						  - data);
			/*
			  Return on permanent errors.
			  Transient errors at this point do not affect the
			  outcome, so just report that nothing was sent and
			  continue.
			*/
			if (sent < 0) {
				if ((errno != EINTR)
				    && (errno != EAGAIN)
				    && (errno != EWOULDBLOCK)) {
					chunk = NULL;
					rv = 1;
				}
				sent = 0;
			}
			data += sent;
		}
	}

	for (last_chunk = NULL, chunk = tx->first;
	     chunk != NULL; chunk = chunk->next) {
		if (last_chunk != NULL)
			free(last_chunk);
		last_chunk = chunk;
	}
	if (last_chunk != NULL)
		free(last_chunk);

	tx->first = NULL;
	tx->last = NULL;
	return rv;
}

int send_tx_fd_persist(int fd, struct tx_text *tx)
{
	struct tx_text_chunk *chunk, *last_chunk;
	unsigned char *data;
	ssize_t sent;
	int rv;

	rv = 0;
	for (chunk = tx->first; chunk != NULL;
	     chunk = chunk ? chunk->next : NULL) {
		data = chunk->buffer;
		while ((chunk != NULL)
		       && (data < (chunk->buffer + chunk->size))) {
			sent = write(fd, (char *)data,
				     (chunk->buffer + chunk->size) - data);
			/*
			  Return on permanent errors.
			  Transient errors at this point do not affect the
			  outcome, so just report that nothing was sent and
			  continue.
			*/
			if (sent < 0) {
				if ((errno != EINTR)
				    && (errno != EAGAIN)
				    && (errno != EWOULDBLOCK)) {
					chunk = NULL;
					rv = 1;
				}
				sent = 0;
			}
			data += sent;
		}
	}

	for (last_chunk = NULL, chunk = tx->first;
	     chunk != NULL; chunk = chunk->next) {
		if (last_chunk != NULL)
			free(last_chunk);
		last_chunk = chunk;
	}
	if (last_chunk != NULL)
		free(last_chunk);

	tx->first = NULL;
	tx->last = NULL;
	return rv;
}

/*
  Send data to the client, persist on transient errors until either
  all data is sent, or data can not be sent because of an error.  This
  function returns either size on success or negative number on
  failure. On success, the data might be buffered and not yet sent, on
  failure some data may be still sent before the error was
  encountered.
*/
ssize_t send_data_persist(int client_index, const char *data, size_t size)
{
	ssize_t sent, sent_total;

	sent_total = 0;
	while (size > 0) {
		sent = send_data_nonblock(client_index, data, size);
		/*
		  Return on permanent errors.
		  Transient errors at this point do not affect the
		  outcome, so just report that nothing was sent and
		  continue.
		*/
		if (sent < 0) {
			if ((errno != EINTR)
			    && (errno != EAGAIN)
			    && (errno != EWOULDBLOCK))
				return sent;
			else
				sent = 0;
		}
		sent_total += sent;
		/* Update the source pointer and size to reflect sent data. */
		size -= sent;
		data += sent;
	}
	return sent_total;
}

/*
  Create AF_UNIX socket.
*/
int isol_server_socket_create(const char *name)
{
	char *tmpname;
	struct sockaddr_un server_socket_addr;
	int sockfd, flags, l;

	if (nfds != 0) {
		fprintf(stderr, "File descriptors already initialized\n");
		return 0;
	}
	l = strlen(name);
	tmpname = (char*)malloc(l + 22);
	if (tmpname == NULL) {
		fprintf(stderr, "Insufficient memory\n");
		return -1;
	}
	strcpy(tmpname, name);
	snprintf(tmpname + l, 22, ".%llu", (unsigned long long)getpid());

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		free(tmpname);
		return -1;
	}
	memset(&server_socket_addr, 0, sizeof(struct sockaddr_un));
	server_socket_addr.sun_family = AF_UNIX;
	strncpy(server_socket_addr.sun_path, tmpname,
		sizeof(server_socket_addr.sun_path) - 1);
	if (bind(sockfd, (const struct sockaddr *) &server_socket_addr,
		 sizeof(struct sockaddr_un)) != 0) {
		close(sockfd);
		free(tmpname);
		return -1;
	}
	if (listen(sockfd, 12) != 0) {
		close(sockfd);
		unlink(tmpname);
		free(tmpname);
		return -1;
	}
	if (renameat2(-1, tmpname, -1, name, RENAME_NOREPLACE)) {
		close(sockfd);
		unlink(tmpname);
		free(tmpname);
		return -1;
	}
	free(tmpname);

	flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	memset(&fds, 0, sizeof(fds));
	/*
	  This should initialize all fixed file descriptors, in this case
	  we have only one.
	*/
	fds[SOCKFD_INDEX].fd = sockfd;
	fds[SOCKFD_INDEX].events = POLLIN;
	nfds = FIXED_FD_INDEXES;

	return 0;
}

/*
  Connect to server in a blocking mode.
*/
int isol_client_connect_to_server(const char *name)
{
	struct sockaddr_un server_socket_addr;
	int sockfd;
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("Can't create socket");
		return -1;
	}
	memset(&server_socket_addr, 0, sizeof(struct sockaddr_un));
	server_socket_addr.sun_family = AF_UNIX;
	strncpy(server_socket_addr.sun_path, name,
		sizeof(server_socket_addr.sun_path) - 1);
	if (connect(sockfd, (const struct sockaddr *) &server_socket_addr,
		    sizeof(struct sockaddr_un)) != 0) {
		close(sockfd);
		return -1;
	}
	return sockfd;
}


/*
  One pass of the application loop.
*/
int isol_server_poll_pass(int timeout)
{
	int rv, i, fd_ind, flags, newsock, fd_closing;
	static struct sockaddr_un client_socket_addr;
	socklen_t addrlen;
	ssize_t l;

	if (nfds <= 0) {
		fprintf(stderr, "Server is running but server socket "
			"does not exist yet\n");
		errno = EINVAL;
		return -1;
	}
	if ((rv = poll(fds, nfds, timeout)) <= 0)
		return rv;

	pending_data_flag = 0;

	/* Fixed fd positions. */
	if (fds[SOCKFD_INDEX].revents & POLLIN) {
		/* New connection. */
		newsock = accept(fds[SOCKFD_INDEX].fd,
				 (struct sockaddr *) &client_socket_addr,
				 &addrlen);
		if (newsock >=0) {
			flags = fcntl(newsock, F_GETFL, 0);
			fcntl(newsock, F_SETFL, flags | O_NONBLOCK);
			
			if ((nfds - FIXED_FD_INDEXES) < NCLIENTS) {
				/* Add new client. */
				fds[nfds].fd = newsock;
				fds[nfds].events = POLLIN;
				fds[nfds].revents = 0;
				nfds++;
				if (create_client_desc(nfds - 1
						       - FIXED_FD_INDEXES)
				    < 0) {
					/*
					  Application can't add new
					  client, disconnect immediately.
					*/
					close(newsock);
					nfds --;

				} else {
					if (client_connect_handler != NULL)
						client_connect_handler(
							nfds - 1
							- FIXED_FD_INDEXES);
				}
			} else {
				/*
				  Can't add new client, disconnect
				  immediately.
				*/
				close(newsock);
			}
		}
		
		/*
		  Don't accept new connections if maximum number of
		  clients is reached.
		*/
		if ((nfds - FIXED_FD_INDEXES) < NCLIENTS)
			fds[SOCKFD_INDEX].events = POLLIN;
		else
			fds[SOCKFD_INDEX].events = 0;
	}

	/* All clients */
	for (i = 0; i < (nfds - FIXED_FD_INDEXES); i++) {
		fd_closing = 0;

		fd_ind = i + FIXED_FD_INDEXES;


		if (fds[fd_ind].revents & POLLIN) {
			/* Data arrived */
			l = read_client_data(i, fds[fd_ind].fd);
			if (l == 0) {
				/* Client disconnected, closing socket. */
				clear_client_pending_data(i);
				set_client_flags(i,
						 CLIENT_FLAG_CLOSE);
				fd_closing = 1;
			}
		}

		if (fd_closing == 0) {
			if (fds[fd_ind].revents & POLLOUT) {
				/* Pending data, send it, process errors. */
				if (size_client_pending_data(i) > 0) {
					if ((send_client_pending_data(i,
							 fds[fd_ind].fd) < 0)
					    &&(errno != EINTR)
					    && (errno != EAGAIN)
					    && (errno != EWOULDBLOCK)) {
						/* Error, closing socket. */
						clear_client_pending_data(i);
						set_client_flags(i,
							 CLIENT_FLAG_CLOSE);
					}
				}
				if (size_client_pending_data(i) == 0)
					fds[fd_ind].events &= ~POLLOUT;
				else
					pending_data_flag = 1;
			} else {
				if (fds[fd_ind].events & POLLOUT)
					pending_data_flag = 1;
			}
		}

		if ((get_client_flags(i) & CLIENT_FLAG_CLOSE)
		    && (size_client_pending_data(i) == 0)) {
			/* This socket should be closed. */
			if (client_disconnect_handler != NULL)
				client_disconnect_handler(i);
			delete_client_desc(i);
			close(fds[fd_ind].fd);
			if ((nfds - fd_ind) > 1)
				memmove(&fds[fd_ind], &fds[fd_ind + 1],
					(nfds - fd_ind - 1)
					* sizeof(struct pollfd));
			i--;
			nfds--;
		}
	}
	return 0;
}
