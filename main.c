#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct server {
    int listen_fd;
    int local_listen_fd;
} server_t;

typedef struct buffer {
    void *remote_to_local_buffer;
    int remote_to_local_len;
    bool remote_to_local_read;
    void *local_to_remote_buffer;
    int local_to_remote_len;
    bool local_to_remote_read;
} buffer_t;

typedef struct thread_args {
    bool local;
    int conn_fd;
} thread_args_t;

int server_listen(server_t *server, bool local);
int server_accept(server_t *server, bool local);

void *read_thread(void *args);
void *write_thread(void *args);

int port, port_local;
buffer_t *buffer;
pthread_mutex_t buffer_lock;

int main(int argc, char *argv[]) {
    port = atoi(argv[1]);
    port_local = atoi(argv[2]);

    server_t server = {0};
    buffer = malloc(sizeof(buffer_t));

    int err = 0;

    err = server_listen(&server, false);
    if (err) {
        printf("Failed to listen on address 0.0.0.0:%d, err: %s\n", port, strerror(err));
        return err;
    }

    int conn_fd = -1;
    while (conn_fd == -1) {
        conn_fd = server_accept(&server, false);
        if (conn_fd == -1)
            printf("Failed accepting connection, err: %s\n", strerror(err));
    }

    err = server_listen(&server, true);
    if (err) {
        printf("Failed to listen on address 0.0.0.0:%d, err: %s\n", port_local, strerror(err));
        return err;
    }

    int local_conn_fd = -1;
    while (local_conn_fd == -1) {
        local_conn_fd = server_accept(&server, true);
        if (local_conn_fd == -1)
            printf("Failed accepting connection, err: %s\n", strerror(err));
    }

    err = pthread_mutex_init(&buffer_lock, NULL);
    if (err != 0) {
        printf("pthread_mutex_init failed, err: %s\n", strerror(err));
        return err;
    }

    pthread_t remote_read_thread, local_read_thread, remote_write_thread, local_write_thread;

    pthread_create(&remote_read_thread, NULL, &read_thread, (void *)(&(thread_args_t){.local = false, .conn_fd = conn_fd}));
    pthread_create(&local_read_thread, NULL, &read_thread, (void *)(&(thread_args_t){.local = true, .conn_fd = local_conn_fd}));
    pthread_create(&remote_read_thread, NULL, &write_thread, (void *)(&(thread_args_t){.local = false, .conn_fd = local_conn_fd}));
    pthread_create(&local_read_thread, NULL, &write_thread, (void *)(&(thread_args_t){.local = true, .conn_fd = conn_fd}));

    pthread_join(remote_read_thread, NULL);
    pthread_join(local_read_thread, NULL);
    pthread_join(remote_write_thread, NULL);
    pthread_join(local_write_thread, NULL);

    pthread_mutex_destroy(&buffer_lock);
}

int server_listen(server_t *server, bool local) {
    int err = 0;

    if (!local)
        err = (server->listen_fd = socket(AF_INET, SOCK_STREAM, 0));
    else
        err = (server->local_listen_fd = socket(AF_INET, SOCK_STREAM, 0));
    if (err == -1) {
        perror("socket");
        printf("Failed to create a socket endpoint, err: %s\n", strerror(err));
        return err;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!local)
        server_addr.sin_port = htons(port);
    else
        server_addr.sin_port = htons(port_local);

    if (!local)
        err = bind(server->listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    else
        err = bind(server->local_listen_fd, (struct sockaddr *)&server_addr, sizeof (server_addr));
    if (err == -1) {
        perror("bind");
        printf("Failed to bind socket to address, err: %s\n", strerror(err));
        return err;
    }

    if (!local)
        err = listen(server->listen_fd, 1);
    else
        err = listen(server->local_listen_fd, 1);
    if (err == -1) {
        perror("listen");
        printf("Failed to put socket in passive mode, err: %s\n", strerror(err));
        return err;
    }
}

int server_accept(server_t *server, bool local) {
    int err = 0;
    int conn_fd;
    socklen_t client_len;
    struct sockaddr_in client_addr;

    client_len = sizeof(client_addr);

    if (!local)
        err = (conn_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len));
    else
        err = (conn_fd = accept(server->local_listen_fd, (struct sockaddr *)&client_addr, &client_len));
    if (err == -1) {
        perror("accept");
        printf("Failed accepting a connection, err: %s\n", strerror(err));
        return err;
    }

    if (!local)
        printf("Remote end connected!\n");
    else
        printf("Local end connected!\n");

    return conn_fd;
}

void *read_thread(void *args) {
    thread_args_t *arguments = (thread_args_t *)args;
    char *read_buffer[1024];

    for (;;) {
        int recv_resp = recv(arguments->conn_fd, read_buffer, 1024, 0);

        if (recv_resp == 0) {
            if (!arguments->local)
                printf("Remote port connection has been closed\n");
            else
                printf("Local port connection has been closed\n");
            return NULL;
        }

        if (recv_resp == -1) {
            printf("recv failed, err: %s\n", strerror(recv_resp));
            continue;
        }

        for (;;) {
            if (!arguments->local && buffer->remote_to_local_read)
                break;
            if (arguments->local && buffer->local_to_remote_read)
                break;
            sleep(1);
        }

        pthread_mutex_lock(&buffer_lock);
        if (!arguments->local) {
            buffer->remote_to_local_buffer = read_buffer;
            buffer->remote_to_local_len = recv_resp;
            buffer->remote_to_local_read = false;
        } else {
            buffer->local_to_remote_buffer = read_buffer;
            buffer->local_to_remote_len = recv_resp;
            buffer->local_to_remote_read = false;
        }
        pthread_mutex_unlock(&buffer_lock);
    }
}

void *write_thread(void *args) {
    thread_args_t *arguments = (thread_args_t *)args;

    for (;;) {
        if (!arguments->local && buffer->remote_to_local_read) {
            sleep(1);
            continue;
        }
        if (arguments->local && buffer->local_to_remote_read) {
            sleep(1);
            continue;
        }

        int send_resp;
        int sent = 0;

        pthread_mutex_lock(&buffer_lock);
        while ((!arguments->local && sent != buffer->remote_to_local_len) || (arguments->local && sent != buffer->local_to_remote_len)) {
            if (!arguments->local)
                send_resp = send(arguments->conn_fd, buffer->remote_to_local_buffer + sent, buffer->remote_to_local_len - sent, 0);
            else
                send_resp = send(arguments->conn_fd, buffer->local_to_remote_buffer + sent, buffer->local_to_remote_len - sent, 0);

            if (send_resp == -1) {
                printf("send failed, err: %s\n", strerror(send_resp));
                continue;
            }

            sent += send_resp;
        }
        pthread_mutex_unlock(&buffer_lock);

        pthread_mutex_lock(&buffer_lock);
        if (!arguments->local)
            buffer->remote_to_local_read = true;
        else
            buffer->local_to_remote_read = true;
        pthread_mutex_unlock(&buffer_lock);
    }
}
