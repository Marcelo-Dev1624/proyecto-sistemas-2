/*
 * Servidor central del monitor inteligente de sistema.
 * Acepta conexiones concurrentes de varios clientes (select()),
 * recibe lineas de metricas por socket TCP y las agrega a metrics.csv.
 *
 * Uso: ./server <puerto> [archivo_salida.csv]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define MAX_CLIENTS FD_SETSIZE
#define BUF_SIZE 1024

typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    char buf[BUF_SIZE];
    size_t len;
} client_t;

static client_t clients[MAX_CLIENTS];
static FILE *out_fp;

static void timestamp_now(char *dst, size_t n) {
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(dst, n, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static void handle_line(const char *client_ip, char *line) {
    char recv_ts[32];
    timestamp_now(recv_ts, sizeof(recv_ts));

    /* line ya viene como: ts_cliente,cpu_pct,mem_pct,proc_count,net_bps */
    printf("[%s] %s -> %s\n", recv_ts, client_ip, line);
    fflush(stdout);

    fprintf(out_fp, "%s,%s,%s\n", recv_ts, client_ip, line);
    fflush(out_fp);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <puerto> [archivo_salida.csv]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *out_path = argc >= 3 ? argv[2] : "metrics.csv";

    int existed = access(out_path, F_OK) == 0;
    out_fp = fopen(out_path, "a");
    if (!out_fp) {
        perror("fopen");
        return 1;
    }
    if (!existed) {
        fprintf(out_fp, "recv_ts,client_ip,client_ts,cpu_pct,mem_pct,proc_count,net_bps\n");
        fflush(out_fp);
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    printf("Servidor escuchando en puerto %d, guardando en %s\n", port, out_path);

    fd_set read_fds;
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &read_fds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cfd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == -1) { slot = i; break; }
                }
                if (slot == -1) {
                    fprintf(stderr, "Maximo de clientes alcanzado, rechazando conexion\n");
                    close(cfd);
                } else {
                    clients[slot].fd = cfd;
                    clients[slot].len = 0;
                    inet_ntop(AF_INET, &cli_addr.sin_addr, clients[slot].ip, sizeof(clients[slot].ip));
                    printf("Nuevo cliente conectado: %s (slot %d)\n", clients[slot].ip, slot);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1 || !FD_ISSET(clients[i].fd, &read_fds)) continue;

            char tmp[BUF_SIZE];
            ssize_t n = read(clients[i].fd, tmp, sizeof(tmp) - 1);
            if (n <= 0) {
                printf("Cliente desconectado: %s\n", clients[i].ip);
                close(clients[i].fd);
                clients[i].fd = -1;
                continue;
            }
            tmp[n] = '\0';

            if (clients[i].len + (size_t)n < BUF_SIZE) {
                memcpy(clients[i].buf + clients[i].len, tmp, n);
                clients[i].len += n;
                clients[i].buf[clients[i].len] = '\0';
            } else {
                clients[i].len = 0;
                continue;
            }

            char *start = clients[i].buf;
            char *nl;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                if (strlen(start) > 0) handle_line(clients[i].ip, start);
                start = nl + 1;
            }
            size_t remaining = strlen(start);
            memmove(clients[i].buf, start, remaining + 1);
            clients[i].len = remaining;
        }
    }

    fclose(out_fp);
    close(listen_fd);
    return 0;
}
