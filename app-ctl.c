/*
 * Task manager client.
 *
 * By Alex Belits <abelits@marvell.com>
 *
 * This is an implementation of a client using AF_UNIX socket.
 * It uses syntax based on FTP control connection.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>


#define SERVER_SOCKET_NAME "/var/run/isol_server"

/*
 * Connect to server in a blocking mode (identical to the same function
 * in isol-server.c).
 */
static int isol_client_connect_to_server(const char *name)
{
	struct sockaddr_un server_socket_addr;
	int sockfd;
	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0)	{
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
 * Read response from server, show it on stdout, capture the response code.
 * Response code is set to -1 if none is received, otherwise the last
 * response code in the sequence of lines.
 *
 * output_style values:
 *   0: Message only (omit response code and continuation mark).
 *   1: Message with response code and continuation mark.
 *   2: Message printed in normal text, response code and continuation
 *      mark in bold, assuming that VT100-compatible terminal is used.
 *   3: No output.
 *
 * Returns nonzero on closed connection.
 */
static int read_show_response(FILE *f, int output_style, int *responsecode)
{
	char buffer[1024], *line, *endline;
	unsigned rcode_value = 1000;
	int rv = 0, cont;
	do {
		line = fgets(buffer, sizeof(buffer), f);
		if (line != NULL) {
			endline = strchr(line, '\n');
			if (endline != NULL) {
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
					switch (output_style) {
					case 0:
						/* Show message only */
						printf("%s\n", line);
						break;
					case 1:
						/* Show re-built response. */
						printf("%03d%c%s\n",
						       rcode_value,
						       cont?'-':' ',
						       line);
						break;
					case 2:
						/*
						  Show human-readable
						  response with bold
						  response code and
						  continuation mark.
						*/
						printf(
						"\033[1m%03d%c\033[0m%s\n",
						       rcode_value,
						       cont?'-':' ',
						       line);
						break;
					default:
						/* Show nothing. */
						break;
					}
				} else {
					/*
					  Assume that lines without
					  response code are always
					  followed by ones with response
					  code.
					*/
					cont = 1;
					*endline = '\0';
					/*
					  Show lines only for output
					  styles up to 2.
					*/
					if (output_style <= 2)
						printf("%s\n", line);
				}
			} else {
				/*
				  Ignore the last line if it does not have
				  a newline, assume that it is the last
				  line before the connection is closed.
				*/
				cont = 0;
				rv = -1;
			}
		} else {
			/* Connection is closed */
			cont = 0;
			rv = -1;
		}
	}
	while (cont);

	if (responsecode != NULL) {
		if (rcode_value == 1000)
			*responsecode = -1;
		else
			*responsecode = (int)rcode_value;
	}
	return rv;
}

#define START_TIMEOUT 10
#define DELAY_MS 200
#define MAX_FD 1024

static int start_application(const char *cmd,
			     char * const *argv, char * const *env,
			     const char *console_uart)
{
	int fd = -1, i, status;
	pid_t pid;
	struct timespec start_time, current_time;

	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);

	pid = fork();

	if (pid < 0)
		return -1;
	if (pid > 0) {
		waitpid(pid, &status, 0);

		signal(SIGTTOU, SIG_DFL);
		signal(SIGTTIN, SIG_DFL);
		signal(SIGTSTP, SIG_DFL);
		if (status != 0)
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
		do {
			/* Try to connect to the running server. */
			fd = isol_client_connect_to_server(SERVER_SOCKET_NAME);
			if (fd < 0)
				poll(NULL, 0, DELAY_MS);
			clock_gettime(CLOCK_MONOTONIC, &current_time);
		}
		while ((fd < 0) &&
		      ((current_time.tv_sec - start_time.tv_sec)
		       < START_TIMEOUT));
		return fd;
	}
	
	if (setpgrp())
		_exit(1);
	signal(SIGHUP, SIG_IGN);

	pid = fork();
	if (pid < 0)
		_exit(1);
	if (pid > 0)
		_exit(0);

	if (console_uart != NULL)
		for (i = 0; i < 3; i++)
			close(i);

	for (i = 3; i < MAX_FD; i++)
		close(i);

	if (console_uart != NULL) {
		open(console_uart, O_RDWR | O_APPEND);
		if (dup(0) < 0)
			_exit(1);
		if (dup(0) < 0)
			_exit(1);
	}
	execve(cmd, argv, env);
	_exit(1);
	return -1;
}

/*
 * List of lines to be sent to the server.
 */
struct cmd_line {
	struct cmd_line *prev;
	struct cmd_line *next;
	char *line;
} *send_list = NULL, *last_list = NULL;

/*
 * Add command to be sent to the server.
 */
static int add_cmd_line(const char *line)
{
	int l;
	struct cmd_line *new_line;

	l = strlen(line) + 1;
	new_line = malloc(sizeof(struct cmd_line) + l);
	if (new_line == NULL)
		return -1;
	new_line->line = ((char*)new_line) + sizeof(struct cmd_line);
	memcpy(new_line->line, line, l);
	new_line->next = NULL;
	new_line->prev = last_list;
	if (last_list != NULL)
		last_list->next = new_line;
	if (send_list == NULL)
		send_list = new_line;
	last_list = new_line;
	return 0;
}


/*
 * Print usage information.
 */
static void usage(const char *name)
{
	fprintf(stderr,
		"Usage: %s <command> [options] [application [options]]\n",
		name);
}

/* Skip whitespace. */
static void skip_whitespace(const char **p)
{
	while ((**p != 0) && ((unsigned char)(**p) <= ' '))
		(*p)++;
}

/* Get value from a hex digit. */
static unsigned char unhex(char c)
{
	if ((c >= '0') && (c <= '9'))
		return (unsigned char)c - '0';
	if ((c >= 'a') && (c <= 'f'))
		return (unsigned char)c - 'a' + 10;
	if ((c >='A') && (c <= 'F'))
		return (unsigned char)c - 'A' + 10;
	return 0;
}

/*
 * Get CPU set from its hex representation.
 */
static int get_cpuset(const char *s, cpu_set_t *cpuset)
{
	const char *start, *p;
	unsigned char val;
	int i, n, cpus_in_set, count;
	start = s;
	skip_whitespace(&start);
	if ((start[0] == '0') && (start[1] == 'x'))
		start += 2;
	p = start;
	while ((*p) &&
	       (((*p >= '0') && (*p <= '9'))
		|| ((*p >='A') && (*p <= 'F'))
		|| ((*p >= 'a') && (*p <= 'f'))))
		p++;
	n = (p - start) * 4;
	cpus_in_set = (n > CPU_SETSIZE)?CPU_SETSIZE:n;

	CPU_ZERO(cpuset);
	for (i = 0, count = 0; i < (p - start); i++) {
		val = unhex(*(p - i - 1));
		if ((val & 1) && ((i * 4) < cpus_in_set)) {
			CPU_SET((i * 4), cpuset);
			count++;
		}
		if ((val & 2) && ((i * 4 + 1) < cpus_in_set)) {
			CPU_SET((i * 4 + 1), cpuset);
			count++;
		}
		if ((val & 4) && ((i * 4 + 2) < cpus_in_set)) {
			CPU_SET((i * 4 + 2), cpuset);
			count++;
		}
		if ((val & 8) && ((i * 4 + 3) < cpus_in_set)) {
			CPU_SET((i * 4 + 3), cpuset);
			count++;
		}
	}
	return count;
}

/*
 * Check if the option is an abbreviation of the reference string.
 * The options use unique first characters, so there is only one valid
 * reference string.
 * Returns 0 on success, nonzero on failure.
 */
static int check_option(const char *option, const char *ref)
{
	const char *p;
	for (p = option; ((*p >= 'a') && (*p <= 'z')) || (*p == '_'); p++);
	if (((*p == '\0') || (*p == '='))
	    && ((p - option) <= (int)strlen(ref))
	    && !memcmp(option, ref, p - option))
		return 0;
	fprintf(stderr, "Invalid option \"-%s\" (is it \"%s\" ?)\n",
		option, ref);
	return 1;
}

/*
 * Get unsigned long value.
 * Returns 0 on success, nonzero on failure.
 */
static int get_param_ul(const char *option, const char *param,
			unsigned long *value)
{
	char *endptr;
	errno = 0;

	*value = strtoul(param, &endptr, 0);
	if ((errno == ERANGE)
	    || (errno != 0 && *value == 0)
	    || (endptr == param)) {
		fprintf(stderr, "Invalid value \"%s=%s\".\n", option, param);
		return 1;
	} else
		return 0;
}

/*
 * Print "Option "<option>" requires a parameter."
 */
static void error_option(const char *ref)
{
	fprintf(stderr, "Option \"-%s\" requires a parameter.\n", ref);
}

/*
 * main()
 */
int main(int argc, char **argv, char **env)
{
	int i, fd, l, l_cmd, arg_left, exit_flag,
		output_style, input_terminal, responsecode, rv;
	FILE *f;
	char *eq, *param, cmdbuffer[1024], *line, *endline,
		*console_uart = NULL;
	int break_flag = 0, debug_flag = 0;
	unsigned long heap_size = 0, stack_size = 0, index = 0,
		numcores = 0, verbose = 0;
	cpu_set_t mask;
	struct stat statbuf;
	char * const *task_argv;

#define NCOMMANDS 14
	const char *commands[NCOMMANDS]= {
	    "boot", "start",
	    "halt", "kill", "shut",
	    "del", "rm", "unplug", "remove",
	    "add", "plug",
	    "info", "show",
	    "interactive"
	};

	enum {
	      APP_CMD_INFO,
	      APP_CMD_BOOT,
	      APP_CMD_DEL,
	      APP_CMD_ADD,
	      APP_CMD_KILL,
	      APP_CMD_INTERACTIVE,
	      APP_CMD_NONE
	} cmdtable [NCOMMANDS] = {
	      APP_CMD_BOOT, APP_CMD_BOOT,
	      APP_CMD_KILL, APP_CMD_KILL, APP_CMD_KILL,
	      APP_CMD_DEL, APP_CMD_DEL, APP_CMD_DEL, APP_CMD_DEL,
	      APP_CMD_ADD, APP_CMD_ADD,
	      APP_CMD_INFO, APP_CMD_INFO,
	      APP_CMD_INTERACTIVE
	}, command;

	CPU_ZERO(&mask);

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

    /* Match the command as unambiguous abbreviation or extension */
	l = strlen(argv[1]);
	for (command = APP_CMD_NONE, i = 0;
	     command == APP_CMD_NONE && i < NCOMMANDS ;
	     i++) {
		l_cmd = strlen(commands[i]);
		if (l < l_cmd)
			l_cmd = l;
		if (!strncmp(argv[1], commands[i], l_cmd))
			command = cmdtable[i];
	}
	for (;
	     command != APP_CMD_NONE && i < NCOMMANDS ;
	     i++) {
		if (!strncmp(argv[1], commands[i], l))
			command = APP_CMD_NONE;
	}

	if(command == APP_CMD_NONE) {
		usage(argv[0]);
		return 1;
	}

	/*
	  Process options in a way that resembles the SE oct-app-ctl
	  utility.
	*/
	for (i = 2; (i < argc) && (argv[i][0]) == '-'; i++) {
		arg_left = argc - i - 1;
		eq = strchr(argv[i], '=');
		param = NULL;
		if (eq)
			param = eq + 1;
		else
			if (arg_left > 0)
				param = argv[i + 1];
		switch (argv[i][1]) {
		case 'b':
			if (check_option(argv[i] + 1, "break"))
				return 1;
			if (eq) {
				fprintf(stderr,
				"Option \"break\" has no parameters.\n");
				return 1;
			}
			break_flag = 1;
			break;
		case 'c':
			if (param == NULL) {
				error_option("console_uart");
				return 1;
			} else
				if (check_option(argv[i] + 1, "console_uart"))
					return 1;
			console_uart = param;
			if (eq == NULL)
				i++;
			break;
		case 'd':
			if (check_option(argv[i] + 1, "debug"))
				return 1;
			if (eq) {
				fprintf(stderr,
				"Option \"debug\" has no parameters.\n");
				return 1;
			}
			debug_flag = 1;
			break;
		case 'h':
			if (param == NULL) {
				error_option("heapsize");
				return 1;
			} else
				if (check_option(argv[i] + 1, "heapsize"))
					return 1;
			if (get_param_ul(argv[i], param, &heap_size))
				return 1;
			if (heap_size < (3 << 20))
				heap_size = 3 << 20;
			if (eq == NULL)
				i++;
			break;
		case 'i':
			if(param == NULL) {
				error_option("index");
				return 1;
			} else
				if (check_option(argv[i] + 1, "index"))
					return 1;
			if (get_param_ul(argv[i], param, &index))
				return 1;
			if (eq == NULL)
				i++;
			break;
		case 'm':
			if (param == NULL) {
				error_option("mask");
				return 1;
			} else
				if (check_option(argv[i] + 1, "mask"))
					return 1;
			if (get_cpuset(param, &mask) == 0) {
				fprintf(stderr, "Invalid mask \"%s\"\n", param);
				return 1;
			}
			if (eq == NULL)
				i++;
			break;
		case 'n':
			if (param == NULL) {
				error_option("numcores");
				return 1;
			} else
				if (check_option(argv[i] + 1, "numcores"))
					return 1;
			if (get_param_ul(argv[i], param, &numcores))
				return 1;
			if (eq == NULL)
				i++;
			break;
		case 's':
			if (param == NULL) {
				error_option("stacksize");
				return 1;
			} else
				if (check_option(argv[i] + 1, "stacksize"))
					return 1;
			if (get_param_ul(argv[i], param, &stack_size))
				return 1;
			if (stack_size < (1 << 20))
				stack_size = 1 << 20;
			if (eq == NULL)
				i++;
			break;
		case 'v':
			if (param == NULL) {
				error_option("verbose");
				return 1;
			} else
				if (check_option(argv[i] + 1, "verbose"))
					return 1;
			if (get_param_ul(argv[i], param, &verbose))
				return 1;
			if (eq == NULL)
				i++;
			break;
		default:
			fprintf(stderr, "Invalid option \"%s\".\n", argv[i]);
			return 1;
		}
	}

	/* The following two variables are unused, for now. */
	debug_flag = debug_flag;
	break_flag = break_flag;

	/* Arguments that will be passed to the task. */
	task_argv = argv + i;

	if (command == APP_CMD_BOOT) {
		if (i == argc) {
			fprintf(stderr, "No application to run.\n");
			return 1;
		} else {
			if (stat(task_argv[0], &statbuf) != 0) {
				perror("Can't access executable file");
				return 1;
			}
			if (!S_ISREG(statbuf.st_mode)
			    || ((statbuf.st_mode & (S_IXUSR
						    | S_IXGRP | S_IXOTH))
				== 0)) {
				fprintf(stderr, "File is not executable.\n");
				return 1;
			}
		}
	}

	/* Try to connect to the running server. */
	fd = isol_client_connect_to_server(SERVER_SOCKET_NAME);

	if (fd < 0) {
		if (command == APP_CMD_BOOT) {
			fd = start_application(task_argv[0],
					       task_argv, env,
					       console_uart);
			if (fd < 0) {
				fprintf(stderr, "Can't start task.\n");
				return 1;
			}
		} else {
			fprintf(stderr, "Task is not running.\n");
			return 1;
		}
	} else {
		if (command == APP_CMD_BOOT) {
			fprintf(stderr, "Can't start task, "
				"it is already running.\n");
			return 1;
		}
	}

	switch (command) {
	case APP_CMD_INFO:
		rv = add_cmd_line("info\n");
		rv |= add_cmd_line("quit\n");
		break;
	case APP_CMD_BOOT:
		rv = add_cmd_line("quit\n");
		break;
	case APP_CMD_DEL:
		rv = add_cmd_line("del\n");
		rv |= add_cmd_line("quit\n");
		break;
	case APP_CMD_ADD:
		rv = add_cmd_line("add\n");
		rv |= add_cmd_line("quit\n");
		break;
	case APP_CMD_KILL:
		rv = add_cmd_line("terminate\n");
		break;
	case APP_CMD_INTERACTIVE:
		rv = 0;
		break;
	default:
		rv = 0;
		break;
	}

	if (rv != 0) {
		fprintf(stderr,
			"Insufficient memory for communication with server.\n");
		return 1;
	}

	f = fdopen(fd, "r+");
	if (f == NULL)
		return 1;

	exit_flag = 0;

	if (command == APP_CMD_INTERACTIVE) {
		if (isatty(1))
			output_style = 2;
		else
			output_style = 1;
	} else {
		switch (verbose) {
		case 0:
			if (command == APP_CMD_INFO)
				output_style = 0;
			else
				output_style = 3;
			break;
		case 1:
			output_style = 0;
			break;
		case 2:
			output_style = 1;
			break;
		default:
			output_style = 1;
			break;
		}
	}

	if ((command == APP_CMD_INTERACTIVE) && isatty(0))
		input_terminal = 1;
	else
		input_terminal = 0;

	while ((exit_flag == 0) &&
	       (read_show_response(f, output_style, &responsecode) == 0)) {
		if ((responsecode == 221) || (responsecode == 421))
			exit_flag = 1;
		else {
			if (send_list != NULL) {
				struct cmd_line *tmp_list;
				fputs(send_list->line, f);
				if (send_list->next != NULL)
					send_list->next->prev = NULL;
				else
					last_list = NULL;
				tmp_list = send_list;
				send_list = send_list->next;
				free(tmp_list);
			} else {
				if (command == APP_CMD_INTERACTIVE) {
					if (input_terminal)
						printf("\033[1m> \033[0m");
					
					line = fgets(cmdbuffer,
						     sizeof(cmdbuffer) - 1,
						     stdin);
					if (line != NULL) {
						endline = strchr(line, '\n');
						if (endline == NULL) {
							/* Append a newline */
							l = strlen(line);
							line[l] = '\n';
							line[l + 1] = '\0';
						}
						fputs(line, f);
					} else
						exit_flag = 1;
				} else
					exit_flag = 1;
			}
		}
	}
	fclose(f);
	return 0;
}
