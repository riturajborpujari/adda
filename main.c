#include <bits/time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 4097
#define BUFFER_SIZE 256

typedef union ip_address {
    in_addr_t s_addr;
    uint8_t   parts[4];
} ip_address;

typedef struct net_address {
    sa_family_t sin_family;
    in_port_t   sin_port;
    ip_address  sin_addr;
    uint8_t     sin_zero[8];
} net_address;

struct message_writer_config {
    int           *channel_fd;
    struct pollfd *pfds;
    uint16_t      *num_pfds;
};

struct message_reader_config {
    int           *channel_fd;
    struct pollfd *pfds;
    uint16_t      *num_pfds;
};

int get_key_for_client(int fd) {
    // client_fd would start from 6, after
    // 0-stdin
    // 1-stdout
    // 2-stderr
    // 3-server_fd
    // 4-w_channel_fd
    // 5-r_channel_fd
    // and index 0 is reserved for server_fd
    // index should start from 1
    return fd - 5;
}

void *message_reader(void *data) {
    struct message_reader_config config =
        *((struct message_reader_config *)data);

    ssize_t nread = 0;
    char    buf[BUFFER_SIZE];
    int     nready = 0;

    while (1) {
        nready = poll(config.pfds, *config.num_pfds, 500);
        if (nready == -1) {
            perror("message_reader: poll failed");
            continue;
        }
        if (nready == 0) {
            continue;
        }

        for (int i = 1; i < *config.num_pfds; i++) {
            if (config.pfds[i].revents & POLLIN) {
                nread = read(config.pfds[i].fd, buf, BUFFER_SIZE - 1);
                if (nread < 0) {
                    perror("read failed");
                    continue;
                }
                if (nread == 0) {
                    close(config.pfds[i].fd);
                    config.pfds[i].fd = -2;
                    continue;
                }

                buf[nread] = '\0';
                // printf("client %d: %s", config.pfds[i].fd, buf);
                //  write message to channel
                if (dprintf(
                        *config.channel_fd, "%d: %s", config.pfds[i].fd, buf) <
                    0) {
                    perror("dprintf failed");
                    continue;
                }
                config.pfds[i].revents = 0;
            }
        }
    }
    return 0;
}

void *message_writer(void *data) {
    struct message_writer_config config =
        *((struct message_writer_config *)data);

    ssize_t       nread = 0;
    char          buf[BUFFER_SIZE];
    int           nready = 0;
    struct pollfd pfds[1];
    pfds[0].fd      = *config.channel_fd;
    pfds[0].events  = POLLIN;
    pfds[0].revents = 0;

    int             author_fd = 0;
    ssize_t         num_msgs  = 0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (1) {
        nready = poll(pfds, 1, -1);
        if (nready == -1) {
            perror("read channel: poll failed");
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            nread = read(pfds[0].fd, buf, BUFFER_SIZE - 1);
            if (nread < 0) {
                perror("message writer: read failed");
                continue;
            }
            if (nread == 0) {
                continue;
            }

            buf[nread] = 0;
            sscanf(buf, "%d", &author_fd);
            // forward message to all fds
            for (int i = 1; i < *config.num_pfds; i++) {
                if (config.pfds[i].fd != -2 && author_fd != config.pfds[i].fd) {
                    num_msgs++;
                    if (num_msgs == 1000000) {
                        clock_gettime(CLOCK_MONOTONIC, &end);
                        double start_sec =
                            start.tv_sec + (start.tv_nsec * 1e-9);
                        double end_sec = end.tv_sec + (end.tv_nsec * 1e-9);
                        printf("1M msgs in %f seconds\n", end_sec - start_sec);
                        start    = end;
                        num_msgs = 0;
                    }
                    if (dprintf(config.pfds[i].fd, "client %s", buf) < 0) {
                        perror("message_writer: dprintf failed");
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int PORT = 9999;
    if (argc > 1) {
        sscanf(argv[1], "%d", &PORT);
    }
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("create socket failed");
        return 1;
    }
    net_address address = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
        .sin_addr   = {.parts = {0, 0, 0, 0}}};
    const socklen_t addr_size = sizeof(net_address);
    if (bind(server_fd, (struct sockaddr *)&address, addr_size) != 0) {
        perror("bind failed");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 2048) < 0) {
        perror("listen failed");
        close(server_fd);
        return 1;
    }
    printf("server listening on port: %d\n", PORT);

    int channel_fds[2] = {0};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel_fds) < 0) {
        perror("channel create failed");
        close(server_fd);
        return 1;
    }

    ssize_t       nread                 = 0;
    char          buf[BUFFER_SIZE]      = {0};
    uint16_t      num_pfds              = 1;
    uint16_t      num_active_fds        = 0;
    struct pollfd pfds[MAX_CLIENTS + 1] = {};
    pfds[0].fd                          = server_fd;
    pfds[0].events                      = POLLIN;
    pfds[0].revents                     = 0;

    struct message_writer_config writer_config;
    writer_config.channel_fd = channel_fds;
    writer_config.pfds       = pfds;
    writer_config.num_pfds   = &num_pfds;
    pthread_t writer_thread;
    if (pthread_create(
            &writer_thread, 0, &message_writer, (void *)&writer_config) != 0) {
        perror("message_writer: thread create failed");
        close(server_fd);
        close(channel_fds[0]);
        close(channel_fds[1]);
        return 1;
    }

    struct message_reader_config reader_config;
    reader_config.channel_fd = channel_fds + 1;
    reader_config.pfds       = pfds;
    reader_config.num_pfds   = &num_pfds;
    pthread_t reader_thread;
    if (pthread_create(
            &reader_thread, 0, &message_reader, (void *)&reader_config) != 0) {
        perror("message_reader: thread create failed");
        close(server_fd);
        close(channel_fds[0]);
        close(channel_fds[1]);
        return 1;
    }

    while (1) {
        // wait for new connections
        printf("waiting for connection\n");
        int nready = poll(pfds, 1, -1);
        if (nready == -1) {
            perror("polling failed");
            for (int i = 0; i < num_pfds; i++) {
                shutdown(pfds[i].fd, SHUT_RDWR);
            }
            return 1;
        }

        if (pfds[0].revents & POLLIN) {
            int client_fd = accept(pfds[0].fd, 0, 0);
            if (client_fd < 0) {
                perror("could not accept");
                continue;
            }
            int index           = get_key_for_client(client_fd);
            pfds[index].fd      = client_fd;
            pfds[index].events  = POLLIN;
            pfds[index].revents = 0;
            num_pfds++;
            num_active_fds++;
            printf(
                "accepted: %d [total: %d] [pfds: %d]\n", client_fd,
                num_active_fds, num_pfds);
            dprintf(client_fd, "welcome friend!\n");
            continue;
        }
    }

    printf("cleaning up...\n");
    close(server_fd);
    close(channel_fds[0]);
    close(channel_fds[1]);
    return 0;
}
