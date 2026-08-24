#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>

#include "aap_session.h"

#define SIDECAR_SOCK_PATH "/tmp/androidauto-sidecar.sock"
#define SIDECAR_LOCK_PATH "/tmp/androidauto-sidecar.lock"
#define AAP_TCP_PORT      5000

#define MAX_POLL_FDS      16

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
    listen(ipc_listen_fd, 4);

    /* Open AAP TCP server socket on port 5000 */
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
            perror("bind(TCP 5000)");
        }
    }

    printf("micro_aap: listening on %s\n", SIDECAR_SOCK_PATH);

    aap_session_t *session = NULL;
    int client_ipc_fds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

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
        int idx_ipc_clients[8];
        for (int i = 0; i < 8; i++) {
            idx_ipc_clients[i] = -1;
            if (client_ipc_fds[i] >= 0) {
                fds[nfds].fd = client_ipc_fds[i];
                fds[nfds].events = POLLIN;
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
            }
        }

        if (ret == 0) continue;

        /* Handle new IPC connection */
        if (fds[idx_ipc_listen].revents & POLLIN) {
            int new_client = accept(ipc_listen_fd, NULL, NULL);
            if (new_client >= 0) {
                for (int i = 0; i < 8; i++) {
                    if (client_ipc_fds[i] < 0) {
                        client_ipc_fds[i] = new_client;
                        break;
                    }
                }
            }
        }

        /* Handle incoming AAP TCP connection */
        if (idx_tcp_listen >= 0 && (fds[idx_tcp_listen].revents & POLLIN)) {
            int new_tcp = accept(tcp_listen_fd, NULL, NULL);
            if (new_tcp >= 0) {
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
            }
        }

        /* Handle IPC commands */
        for (int i = 0; i < 8; i++) {
            if (idx_ipc_clients[i] >= 0 && (fds[idx_ipc_clients[i]].revents & POLLIN)) {
                char cmd_buf[512] = {0};
                int recvd_fd = -1;
                ssize_t n = recv_ancillary_fd(client_ipc_fds[i], cmd_buf, sizeof(cmd_buf) - 1, &recvd_fd);
                if (n <= 0) {
                    close(client_ipc_fds[i]);
                    client_ipc_fds[i] = -1;
                    continue;
                }

                if (recvd_fd >= 0) {
                    printf("micro_aap: received CONNECT_FD ancillary fd=%d\n", recvd_fd);
                    close(recvd_fd); /* Managed via TCP server */
                }

                const char *reply = "OK\n";
                if (strncmp(cmd_buf, "STATUS", 6) == 0) {
                    static char status_reply[256];
                    const char *st_name = "Idle";
                    if (session) {
                        switch (aap_session_get_state(session)) {
                            case AAP_SESSION_STATE_RUNNING: st_name = "Connected"; break;
                            case AAP_SESSION_STATE_TLS_HANDSHAKE:
                            case AAP_SESSION_STATE_AUTH:
                            case AAP_SESSION_STATE_SERVICE_DISCOVERY: st_name = "Connecting"; break;
                            default: st_name = "Idle"; break;
                        }
                    }
                    snprintf(status_reply, sizeof(status_reply), "STATE %s %s\n", st_name,
                             session ? aap_session_get_status_message(session) : "Waiting for phone");
                    reply = status_reply;
                } else if (strncmp(cmd_buf, "SHOW", 4) == 0) {
                    if (session) aap_session_set_video_visible(session, true);
                    reply = "OK\n";
                } else if (strncmp(cmd_buf, "HIDE", 4) == 0) {
                    if (session) aap_session_set_video_visible(session, false);
                    reply = "OK\n";
                } else if (strncmp(cmd_buf, "FOCUS", 5) == 0) {
                    reply = "PROJECTED\n";
                } else if (strncmp(cmd_buf, "KEY ", 4) == 0) {
                    uint32_t code = (uint32_t)strtoul(cmd_buf + 4, NULL, 10);
                    if (session) aap_session_send_key(session, code);
                    reply = "OK\n";
                } else if (strncmp(cmd_buf, "TOUCH ", 6) == 0) {
                    unsigned int x = 0, y = 0;
                    char act[16] = {0};
                    if (sscanf(cmd_buf + 6, "%u %u %15s", &x, &y, act) == 3 && session) {
                        uint32_t action_code = 0; /* DOWN */
                        if (strcmp(act, "MOVE") == 0) action_code = 1;
                        else if (strcmp(act, "UP") == 0) action_code = 2;
                        aap_session_send_touch(session, x, y, action_code);
                    }
                    reply = "OK\n";
                } else if (strncmp(cmd_buf, "NIGHT ", 6) == 0) {
                    if (session) aap_session_send_night_mode(session, cmd_buf[6] == '1');
                    reply = "OK\n";
                }

                write(client_ipc_fds[i], reply, strlen(reply));
            }
        }
    }

    if (session) aap_session_destroy(session);
    if (ipc_listen_fd >= 0) close(ipc_listen_fd);
    if (tcp_listen_fd >= 0) close(tcp_listen_fd);
    unlink(SIDECAR_SOCK_PATH);
    return 0;
}
