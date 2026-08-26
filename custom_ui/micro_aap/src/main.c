#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>

#include <netinet/tcp.h>

#include "aap_session.h"
#include "aap_wifi_setup.h"

#define SIDECAR_SOCK_PATH "/tmp/androidauto-sidecar.sock"
#define SIDECAR_LOCK_PATH "/tmp/androidauto-sidecar.lock"
#define AAP_TCP_PORT      5277

#define MAX_IPC_CLIENTS 32
#define MAX_POLL_FDS    (MAX_IPC_CLIENTS + 4)

typedef struct {
    int rfcomm_fd;
} wifi_worker_args_t;

static int g_active_rfcomm_fd = -1;

static void *wifi_setup_thread(void *arg) {
    wifi_worker_args_t *args = (wifi_worker_args_t *)arg;
    int fd = args->rfcomm_fd;
    free(args);

    printf("micro_aap: starting WiFi AP and RFCOMM WPP handshake for fd=%d\n", fd);
    if (!aap_wifi_ensure_ap_up()) {
        fprintf(stderr, "micro_aap: failed to ensure WiFi AP is running\n");
        close(fd);
        if (g_active_rfcomm_fd == fd) g_active_rfcomm_fd = -1;
        return NULL;
    }

    char bssid[32] = {0};
    aap_wifi_get_bssid(bssid, sizeof(bssid));

    bool ok = aap_wifi_setup_handshake(fd, "192.168.43.1", AAP_TCP_PORT,
                                       "custom_ui_wifi", "88888888",
                                       bssid, 5 /* WPA2_PERSONAL */);
    if (!ok) {
        fprintf(stderr, "micro_aap: WPP handshake failed\n");
        close(fd);
        if (g_active_rfcomm_fd == fd) g_active_rfcomm_fd = -1;
        return NULL;
    }

    printf("micro_aap: WPP handshake complete, keeping rfcomm_fd=%d open as tether\n", fd);
    return NULL;
}

static int acquire_lock(void) {
    int fd = open(SIDECAR_LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static ssize_t recv_ancillary_fd(int sock_fd, char *buf, size_t buf_len, int *out_fd) {
    *out_fd = -1;
    struct msghdr msg = {0};
    struct iovec iov = {buf, buf_len};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char ctrl_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    ssize_t n = recvmsg(sock_fd, &msg, 0);
    if (n <= 0) return n;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
        }
    }
    return n;
}

static void process_single_command(aap_session_t *session, const char *cmd, int client_fd) {
    const char *reply = "OK\n";
    static char status_reply[256];

    if (strncmp(cmd, "STATUS", 6) == 0) {
        const char *st_name = "Idle";
        if (session) {
            switch (aap_session_get_state(session)) {
                case AAP_SESSION_STATE_RUNNING: st_name = "Connected"; break;
                case AAP_SESSION_STATE_TLS_HANDSHAKE:
                case AAP_SESSION_STATE_AUTH:
                case AAP_SESSION_STATE_SERVICE_DISCOVERY: st_name = "Connecting"; break;
                default: st_name = "Idle"; break;
            }
        } else if (g_active_rfcomm_fd >= 0) {
            st_name = "Connecting";
        }
        snprintf(status_reply, sizeof(status_reply), "STATE %s %s\n", st_name,
                 session ? aap_session_get_status_message(session) : (g_active_rfcomm_fd >= 0 ? "WiFi Handshake..." : "Waiting for phone"));
        reply = status_reply;
    } else if (strncmp(cmd, "SHOW", 4) == 0) {
        if (session) aap_session_set_video_visible(session, true);
        reply = "OK\n";
    } else if (strncmp(cmd, "HIDE", 4) == 0) {
        if (session) aap_session_set_video_visible(session, false);
        reply = "OK\n";
    } else if (strncmp(cmd, "FOCUS", 5) == 0) {
        bool native_focus = session && aap_session_is_video_focus_native(session);
        reply = native_focus ? "NATIVE\n" : "PROJECTED\n";
    } else if (strncmp(cmd, "RESUME", 6) == 0) {
        if (session) {
            aap_session_request_video_focus(session, true);
            aap_session_set_video_visible(session, true);
        }
        reply = "OK\n";
    } else if (strncmp(cmd, "NATIVE", 6) == 0) {
        if (session) {
            aap_session_request_video_focus(session, false);
            aap_session_set_video_visible(session, false);
        }
        reply = "OK\n";
    } else if (strncmp(cmd, "KEY ", 4) == 0) {
        uint32_t code = (uint32_t)strtoul(cmd + 4, NULL, 10);
        if (session) aap_session_send_key(session, code);
        reply = "OK\n";
    } else if (strncmp(cmd, "TOUCH ", 6) == 0) {
        unsigned int x = 0, y = 0;
        char act[16] = {0};
        if (sscanf(cmd + 6, "%u %u %15s", &x, &y, act) == 3 && session) {
            uint32_t action_code = 0; /* DOWN */
            if (strcmp(act, "MOVE") == 0) action_code = 1;
            else if (strcmp(act, "UP") == 0) action_code = 2;
            aap_session_send_touch(session, x, y, action_code);
        }
        reply = "OK\n";
    } else if (strncmp(cmd, "NIGHT ", 6) == 0) {
        if (session) aap_session_send_night_mode(session, cmd[6] == '1');
        reply = "OK\n";
    }

    if (client_fd >= 0) {
        ssize_t w = write(client_fd, reply, strlen(reply));
        (void)w;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int lock_fd = acquire_lock();
    if (lock_fd < 0) {
        fprintf(stderr, "micro_aap: another sidecar instance is already running\n");
        return 1;
    }

    unlink(SIDECAR_SOCK_PATH);

    int ipc_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_listen_fd < 0) {
        perror("socket(AF_UNIX)");
        return 1;
    }

    struct sockaddr_un sun_addr;
    memset(&sun_addr, 0, sizeof(sun_addr));
    sun_addr.sun_family = AF_UNIX;
    strncpy(sun_addr.sun_path, SIDECAR_SOCK_PATH, sizeof(sun_addr.sun_path) - 1);

    if (bind(ipc_listen_fd, (struct sockaddr *)&sun_addr, sizeof(sun_addr)) != 0) {
        perror("bind(AF_UNIX)");
        return 1;
    }
    listen(ipc_listen_fd, 8);

    /* Open AAP TCP server socket on port 5277 */
    int tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listen_fd >= 0) {
        int opt = 1;
        setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in sin_addr;
        memset(&sin_addr, 0, sizeof(sin_addr));
        sin_addr.sin_family = AF_INET;
        sin_addr.sin_port = htons(AAP_TCP_PORT);
        sin_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(tcp_listen_fd, (struct sockaddr *)&sin_addr, sizeof(sin_addr)) == 0) {
            listen(tcp_listen_fd, 2);
            printf("micro_aap: listening on TCP 0.0.0.0:%d\n", AAP_TCP_PORT);
        } else {
            perror("bind(TCP 5277)");
        }
    }

    printf("micro_aap: listening on %s\n", SIDECAR_SOCK_PATH);

    aap_session_t *session = NULL;
    int client_ipc_fds[MAX_IPC_CLIENTS];
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) client_ipc_fds[i] = -1;

    while (1) {
        struct pollfd fds[MAX_POLL_FDS];
        int nfds = 0;

        /* Index 0: IPC listener */
        fds[nfds].fd = ipc_listen_fd;
        fds[nfds].events = POLLIN;
        int idx_ipc_listen = nfds++;

        /* Index 1: TCP listener */
        int idx_tcp_listen = -1;
        if (tcp_listen_fd >= 0) {
            fds[nfds].fd = tcp_listen_fd;
            fds[nfds].events = POLLIN;
            idx_tcp_listen = nfds++;
        }

        /* Active AAP session socket */
        int idx_session = -1;
        if (session && aap_session_get_socket_fd(session) >= 0) {
            fds[nfds].fd = aap_session_get_socket_fd(session);
            fds[nfds].events = POLLIN;
            idx_session = nfds++;
        }

        /* Active IPC clients */
        int idx_ipc_clients[MAX_IPC_CLIENTS];
        for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
            idx_ipc_clients[i] = -1;
            if (client_ipc_fds[i] >= 0) {
                fds[nfds].fd = client_ipc_fds[i];
                fds[nfds].events = POLLIN | POLLHUP | POLLERR;
                idx_ipc_clients[i] = nfds++;
            }
        }

        int ret = poll(fds, (nfds_t)nfds, 100); /* 100ms timeout */
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll()");
            break;
        }

        if (session) {
            aap_session_tick(session);
            if (aap_session_get_state(session) == AAP_SESSION_STATE_DISCONNECTED ||
                aap_session_get_state(session) == AAP_SESSION_STATE_ERROR) {
                printf("micro_aap: session ended, tearing down\n");
                aap_session_destroy(session);
                session = NULL;
                if (g_active_rfcomm_fd >= 0) {
                    close(g_active_rfcomm_fd);
                    g_active_rfcomm_fd = -1;
                }
            }
        }

        if (ret == 0) continue;

        /* Handle new IPC connection */
        if (fds[idx_ipc_listen].revents & POLLIN) {
            int new_client = accept(ipc_listen_fd, NULL, NULL);
            if (new_client >= 0) {
                int placed = 0;
                for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
                    if (client_ipc_fds[i] < 0) {
                        client_ipc_fds[i] = new_client;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    close(client_ipc_fds[0]);
                    client_ipc_fds[0] = new_client;
                }
            }
        }

        /* Handle incoming AAP TCP connection */
        if (idx_tcp_listen >= 0 && (fds[idx_tcp_listen].revents & POLLIN)) {
            int new_tcp = accept(tcp_listen_fd, NULL, NULL);
            if (new_tcp >= 0) {
                int opt = 1;
                setsockopt(new_tcp, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                setsockopt(new_tcp, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
                int keepidle = 2;
                setsockopt(new_tcp, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
                int keepintvl = 1;
                setsockopt(new_tcp, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
                int keepcnt = 3;
                setsockopt(new_tcp, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

                printf("micro_aap: incoming phone TCP connection accepted (fd=%d)\n", new_tcp);
                if (session) {
                    aap_session_destroy(session);
                }
                session = aap_session_create(new_tcp);
            }
        }

        /* Handle active AAP session data */
        if (idx_session >= 0 && (fds[idx_session].revents & POLLIN)) {
            if (!aap_session_process_incoming(session)) {
                printf("micro_aap: AAP session I/O error or disconnect\n");
                aap_session_destroy(session);
                session = NULL;
                if (g_active_rfcomm_fd >= 0) {
                    close(g_active_rfcomm_fd);
                    g_active_rfcomm_fd = -1;
                }
            }
        }

        /* Handle IPC commands */
        for (int i = 0; i < MAX_IPC_CLIENTS; i++) {
            if (idx_ipc_clients[i] >= 0 && (fds[idx_ipc_clients[i]].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (fds[idx_ipc_clients[i]].revents & (POLLHUP | POLLERR)) {
                    close(client_ipc_fds[i]);
                    client_ipc_fds[i] = -1;
                    continue;
                }

                char cmd_buf[1024] = {0};
                int recvd_fd = -1;
                ssize_t n = recv_ancillary_fd(client_ipc_fds[i], cmd_buf, sizeof(cmd_buf) - 1, &recvd_fd);
                if (n <= 0) {
                    close(client_ipc_fds[i]);
                    client_ipc_fds[i] = -1;
                    continue;
                }

                if (recvd_fd >= 0) {
                    printf("micro_aap: received CONNECT_FD ancillary fd=%d\n", recvd_fd);
                    if (g_active_rfcomm_fd >= 0) {
                        close(g_active_rfcomm_fd);
                    }
                    g_active_rfcomm_fd = recvd_fd;

                    wifi_worker_args_t *wargs = (wifi_worker_args_t *)malloc(sizeof(wifi_worker_args_t));
                    wargs->rfcomm_fd = recvd_fd;

                    pthread_t wthread;
                    pthread_attr_t attr;
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                    pthread_create(&wthread, &attr, wifi_setup_thread, wargs);
                    pthread_attr_destroy(&attr);
                }

                /* Process each newline-delimited command in cmd_buf */
                char *saveptr = NULL;
                char *line = strtok_r(cmd_buf, "\r\n", &saveptr);
                while (line != NULL) {
                    while (*line == ' ') line++;
                    if (*line != '\0') {
                        process_single_command(session, line, client_ipc_fds[i]);
                    }
                    line = strtok_r(NULL, "\r\n", &saveptr);
                }
            }
        }
    }

    if (session) aap_session_destroy(session);
    if (ipc_listen_fd >= 0) close(ipc_listen_fd);
    if (tcp_listen_fd >= 0) close(tcp_listen_fd);
    if (g_active_rfcomm_fd >= 0) close(g_active_rfcomm_fd);
    unlink(SIDECAR_SOCK_PATH);
    return 0;
}
