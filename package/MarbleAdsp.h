/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Clients of the audio DSP (MarbleAdsp.cpp): a client registers a GLINK channel by name at any time (before or after the DSP is up) and
 * gets its state and messages through callbacks that run on the DSP thread; they must not block (copy what they need and wake their own
 * thread). Sends are queued and go out from the DSP thread, so a caller never waits for the DSP. */
#ifndef MARBLE_ADSP_H
#define MARBLE_ADSP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MARBLE_ADSP_MAX_MESSAGE 512u

typedef struct MarbleAdspChannel MarbleAdspChannel;
typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} MarbleAdspClient;

/* Registers the channel; intent_size/intent_count are the receive buffers offered to the DSP. initiator: we open it (otherwise we answer
 * the DSP's open). Returns null when out of slots or memory. Weak-imported by clients: the DSP code may be absent from a build. */
MarbleAdspChannel *marble_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                     const MarbleAdspClient *client, void *context);
/* Queues one message (at most MARBLE_ADSP_MAX_MESSAGE bytes): 0 queued, -1 queue full or too big, -2 bad arguments. */
int marble_adsp_send(MarbleAdspChannel *channel, const void *data, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
