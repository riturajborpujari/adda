#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 4096
#define BUFFER_SIZE 256

const unsigned int PORT         = 9999;
const char        *CHANNEL_FILE = "general.channel";

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
        nready = poll(config.pfds, *config.num_pfds, -1);
        if (nready == -1) {
            perror("message_reader: poll failed");
            break;
        }

        for (int i = 0; i < *config.num_pfds; i++) {
            if (config.pfds[i].revents & POLLIN) {
                nread = read(config.pfds[i].fd, buf, BUFFER_SIZE);
                if (nread == 0) {
                    close(config.pfds[i].fd);
                    config.pfds[i].fd = -2;
                    continue;
                }

                buf[nread] = '\0';
                // write message to channel
                dprintf(
                    *config.channel_fd, "client%d: %s", config.pfds[i].fd, buf);
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

    while (1) {
        nready = poll(pfds, 1, -1);
        if (nready == -1) {
            perror("read channel: poll failed");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            nread = read(pfds[0].fd, buf, BUFFER_SIZE);
            if (nread == 0) {
                continue;
            }
            // forward message to all fds
            for (int i = 1; i < *config.num_pfds; i++) {
                if (config.pfds[i].fd != -2) {
                    dprintf(config.pfds[i].fd, "%s", buf);
                }
            }
        }
    }
    return 0;
}

int main() {
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
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        return 1;
    }
    printf("server listening on port: %d\n", PORT);

    int w_channel_fd =
        open(CHANNEL_FILE, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (w_channel_fd == -1) {
        perror("write channel: open failed");
        close(server_fd);
        return 1;
    }
    int r_channel_fd = open(CHANNEL_FILE, O_RDONLY);
    if (r_channel_fd == -1) {
        perror("read channel: open failed");
        close(server_fd);
        close(w_channel_fd);
        return 1;
    }

    net_address   client_addr           = {};
    socklen_t     client_addr_size      = addr_size;
    ssize_t       nread                 = 0;
    char          buf[BUFFER_SIZE]      = {0};
    uint16_t      num_pfds              = 1;
    uint16_t      num_active_fds        = 0;
    struct pollfd pfds[MAX_CLIENTS + 1] = {};
    pfds[0].fd                          = server_fd;
    pfds[0].events                      = POLLIN;
    pfds[0].revents                     = 0;

    struct message_writer_config writer_config;
    writer_config.channel_fd = &r_channel_fd;
    writer_config.pfds       = pfds;
    writer_config.num_pfds   = &num_pfds;
    pthread_t writer_thread;
    if (pthread_create(
            &writer_thread, 0, &message_writer, (void *)&writer_config) != 0) {
        perror("message_writer: thread create failed");
        close(server_fd);
        close(w_channel_fd);
        close(r_channel_fd);
        return 1;
    }

    struct message_reader_config reader_config;
    reader_config.channel_fd = &w_channel_fd;
    reader_config.pfds       = pfds;
    reader_config.num_pfds   = &num_pfds;
    pthread_t reader_thread;
    if (pthread_create(
            &reader_thread, 0, &message_reader, (void *)&reader_config) != 0) {
        perror("message_reader: thread create failed");
        close(server_fd);
        close(w_channel_fd);
        close(r_channel_fd);
        return 1;
    }

    while (1) {
        // wait for new connections
        int nready = poll(pfds, 1, -1);
        if (nready == -1) {
            perror("polling failed");
            for (int i = 0; i < num_pfds; i++) {
                shutdown(pfds[i].fd, SHUT_RDWR);
            }
            return 1;
        }

        if (pfds[0].revents & POLLIN) {
            int client_fd = accept(
                pfds[0].fd, (struct sockaddr *)&client_addr, &client_addr_size);
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
            printf("accepted: %d [total: %d]\n", client_fd, num_active_fds);
            dprintf(client_fd, "welcome friend!\n");
            continue;
        }
    }

    close(server_fd);
    return 0;
}
