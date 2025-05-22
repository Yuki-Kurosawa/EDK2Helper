#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h> // For fcntl
#include <errno.h>

#define PORT 12345
#define BUFFER_SIZE 1024

#include <pthread.h>
#include <signal.h>

volatile int keep_running = 1;

void handle_sigint(int sig) {
    (void)sig;
    printf("Received SIGINT, shutting down server...\n");
    keep_running = 0;
}

struct client_info {
    int socket;
    struct sockaddr_in address;
};

void* handle_client(void* arg) {
    struct client_info* cinfo = (struct client_info*)arg;
    char buffer[BUFFER_SIZE] = {0};

    int valread = recv(cinfo->socket, buffer, BUFFER_SIZE - 1, 0);
    if (valread < 0) {
        perror("recv failed");
    } else {
        buffer[valread] = '\0';
        printf("Received: %s\n", buffer);

        const char *response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, World!";
        send(cinfo->socket, response, strlen(response), 0);
        printf("Response sent\n");
    }

    // Always close the client socket when done with it in the thread
    close(cinfo->socket); // This is crucial for resource management
    free(cinfo);
    return NULL;
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    signal(SIGINT, handle_sigint);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    // SO_REUSEADDR allows immediate reuse of the port after server shutdown
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Set the server socket to non-blocking mode
    // This is the key fix for your SIGINT issue
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL O_NONBLOCK failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }


    printf("Server daemon running on port %d\n", PORT);

    while (keep_running) {
        struct client_info* cinfo = malloc(sizeof(struct client_info));
        if (!cinfo) {
            perror("malloc failed");
            // If malloc fails, we can't accept this connection, but should continue trying
            // to accept others if possible. For now, we'll just skip and retry.
            // In a real server, you might log this and potentially exit if it's persistent.
            continue;
        }

        socklen_t clen = sizeof(cinfo->address);
        cinfo->socket = accept(server_fd, (struct sockaddr *)&cinfo->address, &clen);

        if (cinfo->socket < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                // No incoming connections, and the socket is non-blocking.
                // This is expected. We just free cinfo and loop again.
                free(cinfo);
                // Introduce a small sleep to prevent busy-waiting and high CPU usage
                usleep(100000); // Sleep for 100 milliseconds
                continue;
            } else if (errno == EINTR) {
                // If accept was interrupted by a signal (e.g., SIGINT),
                // it might return EINTR. We check keep_running.
                free(cinfo); // Free allocated memory before re-checking loop condition
                continue; // Re-evaluate keep_running
            }
            else {
                perror("accept failed");
                free(cinfo);
                // In a real server, you might want to consider if this error
                // is fatal and if the server should shut down.
                continue;
            }
        } else {
            printf("Accepted connection from %s:%d\n",
                inet_ntoa(cinfo->address.sin_addr),
                ntohs(cinfo->address.sin_port));
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cinfo) != 0) {
            perror("pthread_create failed");
            close(cinfo->socket); // Close client socket if thread creation fails
            free(cinfo);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd); // Close the listening socket when the loop exits
    printf("Server shutting down.\n");
    return 0;
}