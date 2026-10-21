/*
 * aesdsocket: stream socket server on port 9000.
 * Each newline terminated packet received is appended to DATA_FILE, then the
 * full content of DATA_FILE is sent back to the client.
 * Run with -d to start as a daemon once the port is bound.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BACKLOG 10
#define CHUNK_SIZE 1024

static volatile sig_atomic_t exit_requested = 0;

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static int setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: accept() and recv() must return EINTR on a signal */
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Open a stream socket bound to PORT, ready for listen(). Returns -1 on error. */
static int open_server_socket(void)
{
    struct addrinfo hints;
    struct addrinfo *res;
    int sockfd;
    int yes = 1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(sockfd);
        freeaddrinfo(res);
        return -1;
    }

    if (bind(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(sockfd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sockfd;
}

static int daemonize(void)
{
    pid_t pid;
    int devnull;

    pid = fork();
    if (pid == -1) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* Parent: the child carries on as the daemon */
        exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }
    if (chdir("/") == -1) {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) {
        syslog(LOG_ERR, "open /dev/null failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }
    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);
        if (sent == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int append_packet(const char *packet, size_t len)
{
    int fd;
    ssize_t written;

    fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    while (len > 0) {
        written = write(fd, packet, len);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "write %s failed: %s", DATA_FILE, strerror(errno));
            close(fd);
            return -1;
        }
        packet += written;
        len -= (size_t)written;
    }
    close(fd);
    return 0;
}

/* Send DATA_FILE to the client in chunks, as it may not fit in memory */
static int send_data_file(int client_fd)
{
    char chunk[CHUNK_SIZE];
    ssize_t nread;
    int fd;
    int rc = 0;

    fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }
    while ((nread = read(fd, chunk, sizeof(chunk))) != 0) {
        if (nread == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "read %s failed: %s", DATA_FILE, strerror(errno));
            rc = -1;
            break;
        }
        if (send_all(client_fd, chunk, (size_t)nread) != 0) {
            rc = -1;
            break;
        }
    }
    close(fd);
    return rc;
}

/* Receive packets until the client closes the connection or a signal arrives */
static void handle_client(int client_fd)
{
    char chunk[CHUNK_SIZE];
    char *packet = NULL;
    size_t packet_len = 0;
    ssize_t nrecv;

    while (!exit_requested) {
        nrecv = recv(client_fd, chunk, sizeof(chunk), 0);
        if (nrecv == 0) {
            break;
        }
        if (nrecv == -1) {
            if (errno != EINTR) {
                syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            }
            break;
        }

        char *grown = realloc(packet, packet_len + (size_t)nrecv);
        if (grown == NULL) {
            syslog(LOG_ERR, "realloc of %zu bytes failed, dropping packet",
                   packet_len + (size_t)nrecv);
            free(packet);
            packet = NULL;
            packet_len = 0;
            continue;
        }
        packet = grown;
        memcpy(packet + packet_len, chunk, (size_t)nrecv);
        packet_len += (size_t)nrecv;

        /* Handle every complete packet in the buffer */
        char *newline;
        while ((newline = memchr(packet, '\n', packet_len)) != NULL) {
            size_t complete_len = (size_t)(newline - packet) + 1;

            if (append_packet(packet, complete_len) != 0 || send_data_file(client_fd) != 0) {
                free(packet);
                return;
            }
            packet_len -= complete_len;
            memmove(packet, packet + complete_len, packet_len);
        }
    }
    free(packet);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int server_fd;
    int opt;

    openlog("aesdsocket", 0, LOG_USER);

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt == 'd') {
            daemon_mode = true;
        } else {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            closelog();
            return -1;
        }
    }

    if (setup_signals() != 0) {
        closelog();
        return -1;
    }

    server_fd = open_server_socket();
    if (server_fd == -1) {
        closelog();
        return -1;
    }

    /* Fork only once the port is bound */
    if (daemon_mode && daemonize() != 0) {
        close(server_fd);
        closelog();
        return -1;
    }

    if (listen(server_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        char client_ip[INET_ADDRSTRLEN];
        int client_fd;

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno != EINTR) {
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
                close(server_fd);
                closelog();
                return -1;
            }
            continue;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        handle_client(client_fd);

        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    syslog(LOG_INFO, "Caught signal, exiting");
    close(server_fd);
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink %s failed: %s", DATA_FILE, strerror(errno));
    }
    closelog();
    return 0;
}
