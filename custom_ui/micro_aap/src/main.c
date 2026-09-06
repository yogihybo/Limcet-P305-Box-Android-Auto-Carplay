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
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>

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
/* 2026-09-05: real hardware bug found via code review -- g_active_rfcomm_fd
 * was a plain global int, read/written from both this file's main
 * poll() loop and wifi_setup_thread (a detached, unjoined thread per
 * connection) with zero synchronization. This mutex makes every actual
 * read-modify-write of the variable atomic and consistent -- it does
 * NOT by itself guarantee a still-running OLD wifi_setup_thread can
 * never be mid-blocking-call on an fd the main thread just close()'d
 * out from under it (that would need real cooperative cancellation,
 * e.g. a self-pipe to wake a blocked select()/read(), a bigger change
 * than this pass). What bounds THAT residual risk is the SO_RCVTIMEO
 * fix in aap_wifi_setup.c: any blocking read() on a stale/reused fd
 * now fails within 10s instead of hanging forever, so the real-world
 * exposure is "briefly confused for up to ~10s on a rapid successive
 * reconnect", not "permanent thread/fd leak". */
static pthread_mutex_t g_rfcomm_fd_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Clears g_active_rfcomm_fd only if it still matches `fd` -- used by a
 * wifi_setup_thread instance at its own exit points, so an OLDER
 * thread's belated cleanup can never clobber a NEWER connection's fd
 * that has since replaced it. */
static void clear_active_rfcomm_fd_if_matches(int fd) {
    pthread_mutex_lock(&g_rfcomm_fd_mutex);
    if (g_active_rfcomm_fd == fd) {
        g_active_rfcomm_fd = -1;
    }
    pthread_mutex_unlock(&g_rfcomm_fd_mutex);
}

static int read_active_rfcomm_fd(void) {
    pthread_mutex_lock(&g_rfcomm_fd_mutex);
    int fd = g_active_rfcomm_fd;
    pthread_mutex_unlock(&g_rfcomm_fd_mutex);
    return fd;
}

static void close_and_clear_active_rfcomm_fd(void) {
    pthread_mutex_lock(&g_rfcomm_fd_mutex);
    if (g_active_rfcomm_fd >= 0) {
        close(g_active_rfcomm_fd);
        g_active_rfcomm_fd = -1;
    }
    pthread_mutex_unlock(&g_rfcomm_fd_mutex);
}

/* Closes whatever fd was previously active (if any) and installs
 * new_fd as the new active one, atomically w.r.t. every other reader/
 * writer of g_active_rfcomm_fd. */
static void replace_active_rfcomm_fd(int new_fd) {
    pthread_mutex_lock(&g_rfcomm_fd_mutex);
    if (g_active_rfcomm_fd >= 0) {
        close(g_active_rfcomm_fd);
    }
    g_active_rfcomm_fd = new_fd;
    pthread_mutex_unlock(&g_rfcomm_fd_mutex);
}

/* 2026-09-06: user-requested reconnect-forcing behavior -- an AA
 * session that ends (phone hangs up, backgrounds badly, a transient
 * WiFi/BT glitch, etc.) used to leave the WiFi AP itself running
 * indefinitely. The phone's own WiFi client then stays associated to
 * custom_ui_wifi with nothing on the other end -- from the phone's own
 * point of view it's still "connected" to a network, so pressing
 * Connect again on the head unit has nothing to actually force a retry
 * against (Android Auto's wireless flow doesn't automatically retry a
 * dead TCP session just because the head unit asked to). Tearing the
 * AP down after a short grace period makes the phone see a real WiFi
 * disconnect, so it's in a clean state the next time a connection is
 * actually requested -- which is what makes pressing Connect again
 * actually do something.
 *
 * Grace period (not immediate) because a brief link hiccup that
 * recovers on its own shouldn't force a full re-association + WPP
 * handshake + new AAP session for no reason.
 */
#define AP_TEARDOWN_GRACE_SECONDS 10

static pthread_mutex_t g_ap_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_ap_is_up = false;
static time_t g_session_ended_at = 0; /* 0 == no pending teardown armed */

/* Cancels any pending teardown -- called whenever a new connection
 * attempt or a new live session starts, since the AP is needed again
 * regardless of how long it's been sitting idle. */
static void ap_state_cancel_pending_teardown(void) {
    pthread_mutex_lock(&g_ap_state_mutex);
    g_session_ended_at = 0;
    pthread_mutex_unlock(&g_ap_state_mutex);
}

static void ap_state_note_ap_up(void) {
    pthread_mutex_lock(&g_ap_state_mutex);
    g_ap_is_up = true;
    g_session_ended_at = 0;
    pthread_mutex_unlock(&g_ap_state_mutex);
}

/* Called from the main poll loop whenever there's no actively-
 * projected session -- either the aap_session_t is gone entirely
 * (disconnected/error/I-O failure) OR it's still alive but
 * backgrounded (is_video_focus_native). Only arms the countdown on the
 * FIRST such call (idempotent while the condition persists) -- the
 * actual teardown is polled from the main loop below, so it always
 * runs on that thread even though this itself can be called from more
 * than one call site, and every poll-loop tick while backgrounded (see
 * that call site's own comment on why it must be safe to call
 * repeatedly, not just on a transition edge). */
static void ap_state_note_session_ended(void) {
    pthread_mutex_lock(&g_ap_state_mutex);
    if (g_ap_is_up && g_session_ended_at == 0) {
        g_session_ended_at = time(NULL);
    }
    pthread_mutex_unlock(&g_ap_state_mutex);
}

/* Polled every main-loop iteration (~100ms, bounded by poll()'s own
 * timeout below). Tears the AP down once the grace period elapses with
 * nothing having cancelled it in the meantime. */
static void ap_state_poll_teardown(void) {
    pthread_mutex_lock(&g_ap_state_mutex);
    bool should_teardown = g_ap_is_up && g_session_ended_at != 0 &&
                            (time(NULL) - g_session_ended_at) >= AP_TEARDOWN_GRACE_SECONDS;
    if (should_teardown) {
        g_ap_is_up = false;
        g_session_ended_at = 0;
    }
    pthread_mutex_unlock(&g_ap_state_mutex);
    if (should_teardown) {
        printf("[AA] no active session for %ds -- tearing down WiFi AP\n", AP_TEARDOWN_GRACE_SECONDS);
        aap_wifi_teardown_ap();
    }
}

static void *wifi_setup_thread(void *arg) {
    wifi_worker_args_t *args = (wifi_worker_args_t *)arg;
    int fd = args->rfcomm_fd;
    free(args);

    ap_state_cancel_pending_teardown();

    printf("[AA] starting WiFi AP and RFCOMM WPP handshake for fd=%d\n", fd);
    if (!aap_wifi_ensure_ap_up()) {
        fprintf(stderr, "[AA] failed to ensure WiFi AP is running\n");
        close(fd);
        clear_active_rfcomm_fd_if_matches(fd);
        return NULL;
    }
    ap_state_note_ap_up();

    char bssid[32] = {0};
    aap_wifi_get_bssid(bssid, sizeof(bssid));

    bool ok = aap_wifi_setup_handshake(fd, "192.168.43.1", AAP_TCP_PORT,
                                       "custom_ui_wifi", "88888888",
                                       bssid, 5 /* WPA2_PERSONAL */);
    if (!ok) {
        fprintf(stderr, "[AA] WPP handshake failed\n");
        close(fd);
        clear_active_rfcomm_fd_if_matches(fd);
        return NULL;
    }

    printf("[AA] WPP handshake complete, keeping rfcomm_fd=%d open as tether\n", fd);
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
        } else if (read_active_rfcomm_fd() >= 0) {
            st_name = "Connecting";
        }
        snprintf(status_reply, sizeof(status_reply), "STATE %s %s\n", st_name,
                 session ? aap_session_get_status_message(session) : (read_active_rfcomm_fd() >= 0 ? "WiFi Handshake..." : "Waiting for phone"));
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
        reply = NULL;
    } else if (strncmp(cmd, "ROTARY ", 7) == 0) {
        int ticks = (int)strtol(cmd + 7, NULL, 10);
        if (session) aap_session_send_rotary(session, ticks);
        reply = NULL;
    } else if (strncmp(cmd, "TOUCH ", 6) == 0) {
        unsigned int x = 0, y = 0;
        char act[16] = {0};
        if (sscanf(cmd + 6, "%u %u %15s", &x, &y, act) == 3 && session) {
            uint32_t action_code = 0; /* DOWN */
            if (strcmp(act, "MOVE") == 0) action_code = 1;
            else if (strcmp(act, "UP") == 0) action_code = 2;
            aap_session_send_touch(session, x, y, action_code);
        }
        reply = NULL;
    } else if (strncmp(cmd, "NIGHT ", 6) == 0) {
        if (session) aap_session_send_night_mode(session, cmd[6] == '1');
        reply = "OK\n";
    } else if (strncmp(cmd, "AUDIOFOCUS ", 11) == 0) {
        /* 2026-09-04: real hardware need -- pause/resume the phone's
         * own media playback (channel 4/MEDIA_AUDIO) when the display
         * relay switches to/from the OEM Factory LCD, same real
         * mechanism every Android Auto head unit uses to interrupt
         * media for something else. See aap_session_send_audio_focus()'s
         * own header comment. */
        if (session) aap_session_send_audio_focus(session, cmd[11] == '1');
        reply = "OK\n";
    } else if (strncmp(cmd, "EQ ", 3) == 0) {
        int bass = 0, mid = 0, treble = 0, loud = 0;
        if (sscanf(cmd + 3, "%d %d %d %d", &bass, &mid, &treble, &loud) == 4) {
            if (session) {
                aap_session_set_eq(session, bass, mid, treble, loud != 0);
            }
            printf("[AA] Audio EQ updated: Bass=%d dB, Mid=%d dB, Treble=%d dB, Loudness=%d\n",
                   bass, mid, treble, loud);
        }
        reply = "OK\n";
    }

    if (client_fd >= 0 && reply != NULL) {
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
        fprintf(stderr, "[AA] another sidecar instance is already running\n");
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
            printf("[AA] listening on TCP 0.0.0.0:%d\n", AAP_TCP_PORT);
        } else {
            perror("bind(TCP 5277)");
        }
    }

    printf("[AA] listening on %s\n", SIDECAR_SOCK_PATH);

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
                printf("[AA] session ended, tearing down\n");
                aap_session_destroy(session);
                session = NULL;
                close_and_clear_active_rfcomm_fd();
                ap_state_note_session_ended();
            } else if (aap_session_is_video_focus_native(session)) {
                /* 2026-09-06: real gap found -- the ONLY case this was
                 * originally armed for was the aap_session_t itself
                 * being destroyed (a real TCP/protocol-level
                 * disconnect). But the actual real-world complaint this
                 * feature exists for is the phone staying WiFi-
                 * connected with a perfectly alive TCP session, just
                 * backgrounded (native focus) -- "phone stays connected
                 * but AA session is inactive." That case never touched
                 * either teardown-arming call site at all, since
                 * `session` never becomes NULL for it. Same grace-
                 * countdown, same reasoning -- ap_state_note_session_ended()
                 * is safe to call every tick here (it only arms once,
                 * see its own comment) for as long as this stays true. */
                ap_state_note_session_ended();
            } else {
                /* Actively projected -- cancel a countdown that might
                 * still be running from an earlier, briefer
                 * backgrounding (e.g. the user tapped Resume and the
                 * phone granted it back before the grace period
                 * elapsed). */
                ap_state_cancel_pending_teardown();
            }
        }
        ap_state_poll_teardown();

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
                /* 2026-09-04: real hardware bug -- this whole sidecar is
                 * one poll()-based event loop, and every outbound AAP
                 * frame goes through a raw blocking write() on this
                 * socket (send_raw_frame_locked() in aap_session.c). No
                 * timeout was ever set on it -- if the phone's WiFi link
                 * stalls while the kernel's own send buffer for this fd
                 * is full, that write() can block the ENTIRE event loop
                 * indefinitely (no app-level bound at all), freezing
                 * video/audio/key/touch/rotary forwarding AND the local
                 * IPC socket custom_ui talks to, until the OS's own TCP
                 * retransmission gives up -- which can be far longer
                 * than seconds. Same bug class as custom_ui's own
                 * mcu_input.cpp reader-thread freeze (see that file's
                 * header comment), just at the protocol-socket layer
                 * instead of a UI-forwarding one. 5s matches this
                 * sidecar's own already-generous keepalive/probe
                 * cadence above -- long enough to ride out a brief real
                 * stall, short enough that a genuinely dead link doesn't
                 * wedge the whole process.
                 */
                struct timeval send_timeout = {.tv_sec = 5, .tv_usec = 0};
                setsockopt(new_tcp, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

                printf("[AA] incoming phone TCP connection accepted (fd=%d)\n", new_tcp);
                if (session) {
                    aap_session_destroy(session);
                }
                session = aap_session_create(new_tcp);
                /* A live session is forming right now -- cancel any
                 * pending AP teardown left armed from a previous
                 * session's end (see ap_state_poll_teardown()'s own
                 * comment). */
                ap_state_cancel_pending_teardown();
            }
        }

        /* Handle active AAP session data */
        if (idx_session >= 0 && (fds[idx_session].revents & POLLIN)) {
            if (!aap_session_process_incoming(session)) {
                printf("[AA] AAP session I/O error or disconnect\n");
                aap_session_destroy(session);
                session = NULL;
                close_and_clear_active_rfcomm_fd();
                ap_state_note_session_ended();
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
                    printf("[AA] received CONNECT_FD ancillary fd=%d\n", recvd_fd);
                    replace_active_rfcomm_fd(recvd_fd);

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
    close_and_clear_active_rfcomm_fd();
    unlink(SIDECAR_SOCK_PATH);
    return 0;
}
