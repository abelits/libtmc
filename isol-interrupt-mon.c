#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <errno.h>

#include <string.h>
#define FILE_BLOCK_SIZE 4096
#define FILE_BUF_SIZE 1024

void process_eol(char *buf, size_t *len)
{
	char *r, *w;
	r = buf;
	w = buf;
	while ((size_t)(r - buf) < *len) {
		if (((size_t)(r + 1 - buf) < *len) && (*r == '\r') &&
		    ((r[1] == '\n') || (r[1] == '\r')))
			r++;
		else
			*w++ = *r++;
	}
	*len = w - buf;
	r = buf;
	w = buf;
	while ((size_t)(r - buf) < *len) {
		if (((size_t)(r + 1 - buf) < *len) && (*r == '\\') &&
		   (r[1] == '\n'))
			r += 2;
		else
			*w++ = *r++;
	}
	*len = w - buf;
}

char *loadfile(const char *name, size_t *len)
{
	int h, buf_is_small;
	struct stat statbuf;
	off_t l, offset;
	ssize_t blocksize, recvsize, buf_size;
	char *buf;

	buf_size = FILE_BUF_SIZE;
	do {
		buf_is_small = 0;
		h = open(name, O_RDONLY, 0);
		if (h < 0)
			return NULL;
		if (fstat(h, &statbuf)) {
			perror("Can't open input file");
			close(h);
			return NULL;
		}
		l = statbuf.st_size;
		if (l < 0) {
			fprintf(stderr,
				"Can't determine length of input file\n");
			close(h);
			return NULL;
		}
		if (l == 0) {
			l = buf_size;
			buf_is_small = 1;
		}
		buf = malloc(l + 1);
		if (buf == NULL) {
			fprintf(stderr,
				"Can't allocate memory for file buffer\n");
			close(h);
			return NULL;
		}
		offset = 0;
		blocksize = FILE_BLOCK_SIZE;
		while (offset < l) {
			if ((l - offset) < blocksize)
				blocksize = l - offset;
			recvsize = read(h, buf + offset, blocksize);
			if ((recvsize < 0) && (errno != EAGAIN)
			    && (errno != EINTR)) {
				perror("Error while reading input file");
				free(buf);
				close(h);
				return NULL;
			}
			if (recvsize == 0) {
				l = offset;
				buf_is_small = 0;
			} else
				offset += recvsize;
		}
		close(h);
		if (buf_is_small) {
			free(buf);
			buf_size <<= 1;
		}
	} while (buf_is_small);
	*len = l;
	process_eol(buf, len);
	buf[*len] = '\0';
	return buf;
}

void find_end_whitespace(char **ptr, size_t len)
{
	char *endptr;

	endptr = *ptr + len;
	while ((*ptr < endptr) && **ptr && ((unsigned char)(**ptr) <= ' '))
		(*ptr)++;
}

void find_start_whitespace(char **ptr, size_t len)
{
	char *endptr;

	endptr = *ptr + len;
	while ((*ptr < endptr) && **ptr && ((unsigned char)(**ptr) > ' '))
		(*ptr)++;
}

int valid_dec_num(char c) {
	return ((c >= '0') && (c <= '9'));
}

int valid_num(char c) {
	/* No support for floating point here */
	return ((c >= '0') && (c <= '9'))
		|| ((c >= 'a') && (c <= 'f'))
		|| ((c >= 'A') && (c <= 'F'))
		|| (c == 'u') || (c == 'U')
		|| (c == 'l') || (c == 'L');
}

int valid_name(char c) {
	return ((c >= '0') && (c <= '9'))
		|| ((c >= 'a') && (c <= 'z'))
		|| ((c >= 'A') && (c <= 'Z'))
		|| (c == '_');
}

char *load_bin_file(const char *name, size_t *len)
{
	int h;
	struct stat statbuf;
	off_t l, offset;
	ssize_t blocksize, recvsize;
	char *buf;

	h = open(name, O_RDONLY, 0);
	if (h < 0)
		return NULL;
	if (fstat(h, &statbuf)) {
		perror("Can't open input file");
		close(h);
		return NULL;
	}
	l = statbuf.st_size;
	if (l < 0) {
		fprintf(stderr, "Can't determine length of input file\n");
		close(h);
		return NULL;
	}
	buf = malloc(l);
	if (buf == NULL) {
		fprintf(stderr, "Can't allocate memory for file buffer\n");
		close(h);
		return NULL;
	}
	offset = 0;
	blocksize = FILE_BLOCK_SIZE;
	while (offset < l) {
		if ((l - offset) < blocksize)
			blocksize = l - offset;
		recvsize = read(h, buf + offset, blocksize);
		if ((recvsize < 0) && (errno != EAGAIN) && (errno != EINTR)) {
			perror("Error while reading input file");
			free(buf);
			close(h);
			return NULL;
		}
		if (recvsize == 0)
			l = offset;
		else
			offset += recvsize;
		offset += recvsize;
	}
	close(h);
	*len = l;
	return buf;
}

/*
 * Compare unsigned integers for qsort().
 */
static int uintcmp(const void *v1, const void *v2)
{
	return (*((const unsigned int*)v1)
		> *((const unsigned int*)v2)) * 2 - 1;
}

/*
 * Allocate and fill a list of CPUs from a string.
 */
static int string_to_cpulist(const char *s, unsigned int **retbuf)
{
	const char *p;
	char *nextp;
	unsigned int *buf = NULL;
	int cpunum, last_cpu, last_oper, n_cpus, pass, i;
	for (pass = 0; pass < 2; pass++)	{
		p = s;
		nextp = NULL;
		cpunum = 0;
		last_cpu = -1;
		last_oper = 0;
		n_cpus = 0;

		while (*p) {
			cpunum=strtol(p, (char ** restrict)&nextp, 0);
			if (nextp != p) {
				if (cpunum >= 0) {
					if ((last_oper == 1)
					    && (last_cpu >= 0)
					    && (cpunum > last_cpu)) {
						for (i = last_cpu + 1;
						     i <= cpunum; i++) {
							if (pass == 1)
								buf[n_cpus] =
								(unsigned int)i;
							n_cpus++;
						}
					} else {
						if (pass == 1)
							buf[n_cpus] =
							(unsigned int)cpunum;
						n_cpus++;
					}
					last_cpu = cpunum;
					last_oper = 0;
				}
			}
			if (*nextp) {
				if (*nextp == '-')
					last_oper = 1;
				nextp++;
			}
			p = nextp;
		}
		if (pass == 0) {
			if (n_cpus == 0)
				return -1;
			buf = (unsigned int *)malloc(n_cpus
						     * sizeof(unsigned int));
			if (buf == NULL)
				return -1;
		}
	}

	qsort(buf, n_cpus, sizeof(unsigned int), uintcmp);

	for (i = 0; i < n_cpus - 1; i++) {
		if (buf[i] == buf[i+1]) {
			if ((n_cpus - i) > 2)
				memmove(&buf[i + 1],
					&buf[i + 2],
					(n_cpus - i - 2)
					* sizeof(unsigned int));
			i--;
			n_cpus--;
		}
	}
	*retbuf = buf;
	return n_cpus;
}

struct int_def {
	int intr_num;
	unsigned int cpu_count;
	char *intr_name;
	unsigned long long *cpu_intrs_count;
};

void free_count_interrupts(struct int_def *counts)
{
	if (counts[0].cpu_intrs_count != NULL)
		free(counts[0].cpu_intrs_count);
	free(counts);
}

int count_interrupts(struct int_def **counts)
{
	char *int_text, *endl, *cputext, *lineptr, *colon_pos, *curr_pos;
	size_t int_text_len, intname_len;
	int i, n_cpu, n_intr, curr_intr, is_int;
	unsigned int curr_cpu;
	struct int_def *inttable;
	unsigned long long *inttable_data, curr_intr_count;

	int_text = loadfile("/proc/interrupts", &int_text_len);
	if (int_text == NULL) {
		perror("Can't open /proc/interrupts");
		*counts = NULL;
		return -1;
	}

	for (endl = int_text;
	     ((size_t)(endl - int_text) < int_text_len) && (*endl != '\n');
	     endl ++);
	if ((size_t)(endl - int_text) == int_text_len) {
		fprintf(stderr, "No CPU list in /proc/interrupts\n");
		free(int_text);
		*counts = NULL;
		return -1;
	}
	*endl = '\0';
	for (n_cpu = 0, cputext = strstr(int_text, "CPU");
	     cputext;
	     cputext = strstr(cputext + 3, "CPU")) {
		n_cpu ++;
	}
	for (n_intr = 0, intname_len = 0, lineptr = endl;
	     lineptr && ((size_t)(lineptr - int_text) < int_text_len);
	     lineptr = strchr(lineptr + 1, '\n')) {
		colon_pos = strchr(lineptr + 1, ':');
		if (colon_pos) {
			if (intname_len < (size_t)(colon_pos - lineptr - 1))
				intname_len = (size_t)(colon_pos - lineptr - 1);
			n_intr ++;
		}
	}
	if (n_intr == 0) {
		fprintf(stderr, "No interrupts in /proc/interrupts\n");
		free(int_text);
		*counts = NULL;
		return -1;
	}

	inttable = (struct int_def*) malloc(n_intr * (sizeof(struct int_def)
						      + intname_len + 1));
	if (inttable == NULL) {
		fprintf(stderr, "Can't allocate interrupts table\n");
		free(int_text);
		*counts = NULL;
		return -1;
	}

	inttable_data = (unsigned long long *) malloc(n_intr * n_cpu *
						sizeof(unsigned long long));
	if (inttable_data == NULL) {
		fprintf(stderr, "Can't allocate interrupts table\n");
		free(inttable);
		free(int_text);
		*counts = NULL;
		return -1;
	}

	memset(((char *)inttable)  + n_intr * sizeof(struct int_def), 0,
	       n_intr * (intname_len + 1));

	for (i = 0; i < n_intr; i++) {
		inttable[i].intr_name = ((char *)inttable)
			+ n_intr * sizeof(struct int_def)
			+ i * (intname_len + 1);
		inttable[i].cpu_count = n_cpu;
		inttable[i].cpu_intrs_count = inttable_data + i * n_cpu;
	}
	for (curr_intr = 0, lineptr = endl;
	     lineptr && ((size_t)(lineptr - int_text) < int_text_len)
		     && (curr_intr < n_intr);
	     lineptr = strchr(lineptr + 1, '\n')) {
		colon_pos = strchr(lineptr + 1, ':');
		if (colon_pos) {
			curr_pos = lineptr + 1;
			find_end_whitespace(&curr_pos, colon_pos - curr_pos);
			memcpy(inttable[curr_intr].intr_name, curr_pos,
			       colon_pos - curr_pos);
			inttable[curr_intr].intr_name[colon_pos - curr_pos]
				= '\0';
			for (i = 0, is_int = 1; i < colon_pos - curr_pos; i++)
				if (!valid_dec_num(inttable[curr_intr].
						   intr_name[i])) {
					is_int = 0;
				}
			inttable[curr_intr].intr_num = -1;
			if (is_int)
				sscanf(inttable[curr_intr].intr_name, "%d",
				       &inttable[curr_intr].intr_num);
			
			for (curr_cpu = 0, curr_pos = colon_pos + 1;
			    ((size_t)(curr_pos - int_text) < int_text_len)
				    && (*curr_pos != '\n');) {
				find_end_whitespace(&curr_pos,
						    int_text + int_text_len
						    - curr_pos);
				if (curr_pos < (int_text + int_text_len)) {
					if (curr_cpu <
					    inttable[curr_intr].cpu_count) {
						for (curr_intr_count = 0;
						    ((curr_pos <
						      (int_text + int_text_len))
						   && valid_dec_num(*curr_pos));
						    curr_pos ++) {
							curr_intr_count *= 10;
							curr_intr_count
							+= *curr_pos - '0';
						}
						find_start_whitespace(&curr_pos,
								      int_text
								  + int_text_len
								  - curr_pos);
						inttable[curr_intr].
						cpu_intrs_count[curr_cpu] =
							curr_intr_count;
						curr_cpu ++;
					} else {
						while ((curr_pos <
						(int_text + int_text_len))
						       && (*curr_pos != '\n'))
							curr_pos ++;
					}
				}
			}
			for (; curr_cpu < inttable[curr_intr].cpu_count;
			     curr_cpu++)
				inttable[curr_intr].cpu_intrs_count[curr_cpu]
					= 0;
			curr_intr ++;
		}
	}
	free(int_text);
	*counts = inttable;
	return n_intr;
}

void print_count_interrupts(int n, struct int_def *counts)
{
	int i;
	unsigned int j;

	printf("%d interrupts, %d CPUs\n", n, counts[0].cpu_count);
	for (i = 0; i < n; i++) {
		if (counts[i].intr_num >= 0)
			printf("%d \t:",
			       counts[i].intr_num);
		else
			printf("\"%s\" \t:",
			       counts[i].intr_name);
		for (j = 0; j < counts[i].cpu_count; j++)
			printf(" %llu", counts[i].cpu_intrs_count[j]);
		printf("\n");
	}
}

void print_count_interrupts_diff(int n_new, struct int_def *counts_new,
				 int n_old, struct int_def *counts_old,
				 int n_cpulist, unsigned int *cpulist)
{
	int cpuind, cpu_printed, intind, intind_old;
	unsigned int cpu;
	unsigned long long count_diff;
	
	for (cpuind = 0; cpuind < n_cpulist; cpuind ++) {
		cpu = cpulist[cpuind];
		cpu_printed = 0;
		for (intind = 0; intind < n_new; intind ++) {
			for (intind_old = 0; intind_old < n_old
				    && strcmp(counts_new[intind].intr_name,
					      counts_old[intind_old].intr_name);
			    intind_old ++);
			if (intind_old >= n_old) {
				if ((cpu < counts_new[intind].cpu_count)
				    && (counts_new[intind].cpu_intrs_count[cpu]
				       != 0)) {
					if (!cpu_printed) {
						printf("CPU%d:\n", cpu);
						cpu_printed = 1;
					} else {
						printf(",");
					}
					printf(" New interrupt: \"%s\" x%llu",
					       counts_new[intind].intr_name,
					       counts_new[intind].
					       cpu_intrs_count[cpu]);
				}
			} else {
				if ((cpu < counts_new[intind].cpu_count)
				    && (cpu <
					counts_old[intind_old].cpu_count)) {
					count_diff =
				counts_new[intind].cpu_intrs_count[cpu] -
				counts_old[intind_old].cpu_intrs_count[cpu];
					if (count_diff) {
						if (!cpu_printed) {
							printf("CPU%d:", cpu);
							cpu_printed = 1;
						} else {
							printf(",");
						}
						if (count_diff == 1) {
							printf(" %s",
						counts_new[intind].intr_name);
						} else {
							printf(" %s x%llu",
						counts_new[intind].intr_name,
							       count_diff);
						}
					}
				}
			}
		}
		if (cpu_printed)
			printf("\n");
	}
}


void usage(void)
{
	printf(
	"isol_interrupt_mon -- Interrupt monitor for isolation testing.\n\n"
	"Usage: isol_interrupt_mon <options>\n"
	"Options:\n"
	"--help or -h                       -- this message\n"
	"--cpus=<cpu list> or -c <cpu list> -- list of CPUs to monitor\n");
}

int main(int argc, char **argv)
{
	struct int_def *counts, *counts_new;
	int i, n, n_new, n_cpus, options_left, opt_index, opt;
	unsigned int *cpus;
	char *options_short = "hc:";
	static struct option options_long[] = {
		{ "help", no_argument, 0, 'h' },
		{ "cpus", required_argument, 0, 'c' },
		{ NULL, 0, 0, 0 }
	};

	options_left = 1;
	cpus = NULL;
	while (options_left) {
		opt_index = 0;
		opt = getopt_long(argc, argv, options_short,
				  options_long, &opt_index);
		switch (opt) {
		case 'h':
			usage();
			return 0;
			break;
		case 'c':
			if (cpus)
				free(cpus);
			cpus = NULL;
			n_cpus = string_to_cpulist(optarg, &cpus);
			if (n_cpus <= 0) {
				fprintf(stderr, "No CPUs defined\n");
			}
			break;
		case -1:
			options_left = 0;
			break;
		default:
			fprintf(stderr, "Invalid option\n");
			return 1;
		}
	}

	n = count_interrupts(&counts);
	if (n < 0) {
		fprintf(stderr,
			"Can't determine interrupts and CPUs numbers\n");
		return 1;
	}

	if (cpus == NULL) {
		n_cpus =  counts[0].cpu_count;
		cpus = (unsigned int *) malloc(n_cpus * sizeof(unsigned int));
		if (cpus == NULL) {
			free_count_interrupts(counts);
			fprintf(stderr, "Can't allocate CPU list\n");
			return 1;
		}
	for (i = 0; i < n_cpus; i++)
		cpus[i] = i;
	}
	printf("CPUs: ");
	for (i = 0; i < n_cpus; i++)
		printf("CPU%d%s", cpus[i], i < (n_cpus - 1)?", ":"\n");

	while (1) {
		sleep(1);
		n_new = count_interrupts(&counts_new);
		if (n < 0) {
			free_count_interrupts(counts);
			free(cpus);
			fprintf(stderr,
				"Error reading new interrupts counts\n");
		}
		/*
		  print_count_interrupts(n_new, counts_new);
		*/
		print_count_interrupts_diff(n_new, counts_new, n, counts,
					    n_cpus, cpus);

		free_count_interrupts(counts);
		counts = counts_new;
		n = n_new;
	}
	free_count_interrupts(counts);
	free(cpus);
	return 0;
}
