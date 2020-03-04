#ifndef __ISOL_SERVER_H__
#define __ISOL_SERVER_H__

/*
 * Task isolation server.
 *
 * By Alex Belits <abelits@marvell.com>
 */

#define SERVER_SOCKET_NAME "/var/run/isol_server"
#define ISOL_SERVER_IDLE_POLL_TIMEOUT 200

struct tx_text_chunk {
    unsigned char *buffer;
    size_t size;
    struct tx_text_chunk *next;

};

struct tx_text {
    struct tx_text_chunk *first;
    struct tx_text_chunk *last;
};

struct rx_buffer
{
    char *input_buffer;
    size_t input_buffer_len;
    size_t input_buffer_alloc;
};

struct kv_rx {
    char *key;
    enum {
	KV_TYPE_INT,
	KV_TYPE_ENUM,
	KV_TYPE_STRING
    } val_type;
    char **enum_strings;
    int set;
    union {
	long int val_int;
	void *val_ptr;
    } val;
};

/*
  Set client line handler.
*/
void set_client_line_handler(int (*handler)(int client_index,
					    const char *line));
/*
  Set client connect handler.
*/
void set_client_connect_handler(int (*handler)(int client_index));

/*
  Set client disconnect handler.
*/
void set_client_disconnect_handler(int (*handler)(int client_index));

/*
  Return nonzero if there is data to be sent to the clients.
*/
int is_pending_data_present(void);

/*
  Close client connection after all data is sent.
*/
void close_client_connection(int client_index);

/*
  Send data to the client. As much as possible will be sent
  immediately, the rest is buffered. This function may produce short
  write if there is no sufficient space in the buffer.
*/
ssize_t send_data_nonblock(int client_index, const char *data, size_t size);

/*
  Send data to the client, persist on transient errors until either
  all data is sent, or data can not be sent because of an error.  This
  function returns either size on success or negative number on
  failure. On success, the data might be buffered and not yet sent, on
  failure some data may be still sent before the error was
  encountered.
*/
ssize_t send_data_persist(int client_index, const char *data, size_t size);

void tx_init(struct tx_text *tx);
int tx_add_text(struct tx_text *tx, char *text);
int tx_add_text_num(struct tx_text *tx, long v);
int send_tx_persist(int client_index, struct tx_text *tx);
int send_tx_fd_persist(int fd, struct tx_text *tx);

void *get_client_task(int client_index);
void set_client_task(int client_index, void *task);
int get_client_index(void *task);

void clear_kv_rx(struct kv_rx *kvpairs);
int init_rx_buffer(struct rx_buffer *rx);
void free_rx_buffer(struct rx_buffer *rx);
int read_rx_data(struct rx_buffer *rx, int fd, struct kv_rx *kvpairs);

/*
  Create AF_UNIX socket.
*/
int isol_server_socket_create(const char *name);

/*
  Connect to server in a blocking mode.
*/

int isol_client_connect_to_server(const char *name);
/*
  One pass of the application loop.
*/
int isol_server_poll_pass(int timeout);

#endif
