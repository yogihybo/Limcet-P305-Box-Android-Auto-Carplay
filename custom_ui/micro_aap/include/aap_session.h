#ifndef AAP_SESSION_H
#define AAP_SESSION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "aap_wire.h"
#include "aap_cryptor.h"
#include "aap_audio.h"
#include "aap_video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AAP_SESSION_STATE_IDLE,
    AAP_SESSION_STATE_CONNECTING,
    AAP_SESSION_STATE_VERSION_HANDSHAKE,
    AAP_SESSION_STATE_TLS_HANDSHAKE,
    AAP_SESSION_STATE_AUTH,
    AAP_SESSION_STATE_SERVICE_DISCOVERY,
    AAP_SESSION_STATE_CHANNELS_OPENING,
    AAP_SESSION_STATE_RUNNING,
    AAP_SESSION_STATE_DISCONNECTED,
    AAP_SESSION_STATE_ERROR
} aap_session_state_t;

typedef struct aap_session aap_session_t;

aap_session_t *aap_session_create(int tcp_fd);
void aap_session_destroy(aap_session_t *session);

aap_session_state_t aap_session_get_state(const aap_session_t *session);
const char *aap_session_get_status_message(const aap_session_t *session);

/* Step/process incoming data on socket. Returns false on socket closed or fatal error */
bool aap_session_process_incoming(aap_session_t *session);

/* Periodic tick (call every 100ms) for ping timers and watchdog keepalives */
void aap_session_tick(aap_session_t *session);

/* Forward UI / Car events into the session */
void aap_session_send_key(aap_session_t *session, uint32_t keycode);
void aap_session_send_touch(aap_session_t *session, uint32_t x, uint32_t y, uint32_t action);
void aap_session_send_night_mode(aap_session_t *session, bool night_mode);
void aap_session_set_video_visible(aap_session_t *session, bool visible);

int aap_session_get_socket_fd(const aap_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* AAP_SESSION_H */
