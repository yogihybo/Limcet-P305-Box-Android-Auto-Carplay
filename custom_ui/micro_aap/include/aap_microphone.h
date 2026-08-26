#ifndef AAP_MICROPHONE_H
#define AAP_MICROPHONE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct aap_microphone aap_microphone_t;
typedef struct aap_session aap_session_t;

typedef void (*aap_microphone_data_cb_t)(aap_session_t *session, uint64_t timestamp_us, const uint8_t *data, size_t len);

aap_microphone_t *aap_microphone_create(const char *device_name);
void aap_microphone_destroy(aap_microphone_t *mic);

bool aap_microphone_start(aap_microphone_t *mic, aap_session_t *session, aap_microphone_data_cb_t callback);
void aap_microphone_stop(aap_microphone_t *mic);
bool aap_microphone_is_capturing(const aap_microphone_t *mic);

#endif /* AAP_MICROPHONE_H */
