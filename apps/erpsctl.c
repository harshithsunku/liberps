/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TRAILER '\x1e'

typedef struct {
    int fd;
    char data[4096];
    size_t pos;
    size_t len;
} reader_t;

static void usage(FILE *out)
{
    fprintf(out, "usage: erpsctl [-s socket] [command words...]\n");
}

static int write_all(int fd, const char *s, size_t len)
{
    while (len) {
        ssize_t n = write(fd, s, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EPIPE;
            return -1;
        }
        s += n;
        len -= (size_t)n;
    }
    return 0;
}

static int next_byte(reader_t *r)
{
    if (r->pos == r->len) {
        ssize_t n;
        do {
            n = read(r->fd, r->data, sizeof r->data);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }
        r->pos = 0;
        r->len = (size_t)n;
    }
    return (unsigned char)r->data[r->pos++];
}

static int response(reader_t *r, int *remote_rc)
{
    for (;;) {
        int ch = next_byte(r);
        if (ch < 0)
            return -1;
        if (ch == TRAILER)
            break;
        if (putchar(ch) == EOF)
            return -1;
    }
    char number[32];
    size_t len = 0;
    for (;;) {
        int ch = next_byte(r);
        if (ch < 0)
            return -1;
        if (ch == '\n')
            break;
        if (len + 1 >= sizeof number) {
            errno = EPROTO;
            return -1;
        }
        number[len++] = (char)ch;
    }
    number[len] = '\0';
    char *end;
    errno = 0;
    long parsed = strtol(number, &end, 10);
    if (errno || end == number || *end || parsed < INT_MIN || parsed > INT_MAX) {
        errno = EPROTO;
        return -1;
    }
    *remote_rc = (int)parsed;
    return fflush(stdout) == 0 ? 0 : -1;
}

static int exchange(reader_t *r, const char *line, size_t len, int *remote_rc)
{
    if (write_all(r->fd, line, len) < 0 || write_all(r->fd, "\n", 1) < 0)
        return -1;
    return response(r, remote_rc);
}

int main(int argc, char **argv)
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("erpsctl: signal SIGPIPE");
        return 1;
    }
    const char *path = "/run/erpsd.sock";
    int opt;
    while ((opt = getopt(argc, argv, "s:h")) != -1) {
        if (opt == 's')
            path = optarg;
        else if (opt == 'h') {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 1;
        }
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    if (!*path || strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "erpsctl: invalid socket path\n");
        return 1;
    }
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("erpsctl: socket");
        return 1;
    }
    int connected;
    do {
        connected = connect(fd, (const struct sockaddr *)&addr, sizeof addr);
    } while (connected < 0 && errno == EINTR);
    if (connected < 0) {
        fprintf(stderr, "erpsctl: connect %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    reader_t r = { .fd = fd };
    int status = 0;
    if (optind < argc) {
        char command[2048];
        size_t len = 0;
        for (int i = optind; i < argc; i++) {
            size_t word = strlen(argv[i]);
            if (len + word + (i != optind ? 1u : 0u) >= sizeof command) {
                fprintf(stderr, "erpsctl: command too long\n");
                close(fd);
                return 1;
            }
            if (i != optind)
                command[len++] = ' ';
            memcpy(command + len, argv[i], word);
            len += word;
        }
        int remote_rc = 0;
        if (exchange(&r, command, len, &remote_rc) < 0) {
            perror("erpsctl: request");
            status = 1;
        } else if (remote_rc != 0) {
            status = 1;
        }
    } else {
        char line[2049];
        bool prompt = isatty(STDIN_FILENO) != 0;
        for (;;) {
            if (prompt) {
                fputs("erpsctl> ", stdout);
                fflush(stdout);
            }
            if (!fgets(line, sizeof line, stdin))
                break;
            size_t len = strlen(line);
            bool newline = len && line[len - 1] == '\n';
            if (!newline && len == sizeof line - 1) {
                int ch;
                do {
                    ch = fgetc(stdin);
                } while (ch != '\n' && ch != EOF);
                fprintf(stderr, "erpsctl: command too long\n");
                status = 1;
                continue;
            }
            if (newline)
                len--;
            int remote_rc = 0;
            if (exchange(&r, line, len, &remote_rc) < 0) {
                perror("erpsctl: request");
                status = 1;
                break;
            }
            if (remote_rc != 0)
                status = 1;
        }
        if (ferror(stdin))
            status = 1;
    }
    if (close(fd) < 0)
        status = 1;
    return status;
}
