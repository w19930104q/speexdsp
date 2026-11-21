/* Copyright (C) 2002 Jean-Marc Valin */
/**
   @file speex_jitter.h
   @brief Adaptive jitter buffer for Speex
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef SPEEX_JITTER_H
#define SPEEX_JITTER_H
/** @defgroup JitterBuffer JitterBuffer: Adaptive jitter buffer
 *  This is the jitter buffer that reorders UDP/RTP packets and adjusts the buffer size
 * to maintain good quality and low latency.
 *  @{
 */

#include "speexdsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Generic adaptive jitter buffer state */
struct JitterBuffer_;

/** Generic adaptive jitter buffer state */
typedef struct JitterBuffer_ JitterBuffer;

/** Definition of an incoming packet */
typedef struct _JitterBufferPacket JitterBufferPacket;

/** Definition of an incoming packet.

    Compatibility note: the user_data field was widened from spx_uint32_t to
    spx_uintptr_t (an unsigned integer type with the same width as a pointer)
    so that a pointer can be stored in it without truncation on 64-bit
    platforms. On 32-bit platforms the field is still 32 bits wide and the
    layout is unchanged. On 64-bit platforms the struct layout changes, so
    all code using JitterBufferPacket must be recompiled against this header;
    source code that only stores 32-bit values in user_data continues to work
    unmodified (values are zero-extended). Callers that do not use a field
    should initialise it to 0.

    Ownership of the data buffer: the jitter buffer NEVER copies, frees, or
    otherwise takes ownership of the memory pointed to by the data field.
    The caller retains ownership of the buffer at all times and must keep it
    valid from jitter_buffer_put() until the packet has been retrieved with
    jitter_buffer_get()/jitter_buffer_get_another() or discarded with
    jitter_buffer_reset()/jitter_buffer_destroy(). When a packet is
    retrieved, the same pointer the caller passed in is handed back
    (zero-copy); freeing the buffer while the jitter buffer still holds the
    packet results in a dangling pointer being returned later. */
struct _JitterBufferPacket {
   char        *data;       /**< Data bytes contained in the packet (owned by the caller, never copied or freed by the jitter buffer -- see struct documentation) */
   spx_uint32_t len;        /**< Length of the packet in bytes */
   spx_uint32_t timestamp;  /**< Timestamp for the packet */
   spx_uint32_t span;       /**< Time covered by the packet (same units as timestamp) */
   spx_uint16_t sequence;   /**< RTP Sequence number if available (0 otherwise) */
   spx_uintptr_t user_data; /**< Put whatever data you like here (it's copied verbatim by the jitter buffer and otherwise ignored). Wide enough to hold a pointer: same width as void* on the target platform. Initialise to 0 if unused. */
};

/** Packet has been retrieved */
#define JITTER_BUFFER_OK 0
/** Packet is lost or is late */
#define JITTER_BUFFER_MISSING 1
/** A "fake" packet is meant to be inserted here to increase buffering */
#define JITTER_BUFFER_INSERTION 2
/** There was an error in the jitter buffer */
#define JITTER_BUFFER_INTERNAL_ERROR -1
/** Invalid argument */
#define JITTER_BUFFER_BAD_ARGUMENT -2


/** Set minimum amount of extra buffering required (margin) */
#define JITTER_BUFFER_SET_MARGIN 0
/** Get minimum amount of extra buffering required (margin) */
#define JITTER_BUFFER_GET_MARGIN 1
/* JITTER_BUFFER_SET_AVAILABLE_COUNT wouldn't make sense */

/** Get the amount of available packets currently buffered */
#define JITTER_BUFFER_GET_AVAILABLE_COUNT 3
/** Included because of an early misspelling (will remove in next release) */
#define JITTER_BUFFER_GET_AVALIABLE_COUNT 3

/** Deprecated: assign a function to destroy unused packets. The jitter
    buffer never takes ownership of packet data, so this callback is
    accepted for backward compatibility only and is NEVER invoked. The
    caller is always responsible for freeing packet data. */
#define JITTER_BUFFER_SET_DESTROY_CALLBACK 4
/** Deprecated: reads back the pointer set with JITTER_BUFFER_SET_DESTROY_CALLBACK */
#define JITTER_BUFFER_GET_DESTROY_CALLBACK 5

/** Tell the jitter buffer to only adjust the delay in multiples of the step parameter provided */
#define JITTER_BUFFER_SET_DELAY_STEP 6
/**  */
#define JITTER_BUFFER_GET_DELAY_STEP 7

/** Tell the jitter buffer to only do concealment in multiples of the size parameter provided */
#define JITTER_BUFFER_SET_CONCEALMENT_SIZE 8
#define JITTER_BUFFER_GET_CONCEALMENT_SIZE 9

/** Absolute max amount of loss that can be tolerated regardless of the delay. Typical loss
    should be half of that or less. */
#define JITTER_BUFFER_SET_MAX_LATE_RATE 10
#define JITTER_BUFFER_GET_MAX_LATE_RATE 11

/** Equivalent cost of one percent late packet in timestamp units */
#define JITTER_BUFFER_SET_LATE_COST 12
#define JITTER_BUFFER_GET_LATE_COST 13


/** Initialises jitter buffer
 *
 * @param step_size Starting value for the size of concleanment packets and delay
       adjustment steps. Can be changed at any time using JITTER_BUFFER_SET_DELAY_STEP
       and JITTER_BUFFER_GET_CONCEALMENT_SIZE.
 * @return Newly created jitter buffer state
 */
JitterBuffer *jitter_buffer_init(int step_size);

/** Restores jitter buffer to its original state. Any buffered packets are
 * discarded; their data buffers are NOT freed (the caller owns them, see
 * JitterBufferPacket).
 *
 * @param jitter Jitter buffer state
 */
void jitter_buffer_reset(JitterBuffer *jitter);

/** Destroys jitter buffer. Any buffered packets are discarded; their data
 * buffers are NOT freed (the caller owns them, see JitterBufferPacket).
 *
 * @param jitter Jitter buffer state
 */
void jitter_buffer_destroy(JitterBuffer *jitter);

/** Put one packet into the jitter buffer. The packet's data buffer is NOT
 * copied: the jitter buffer only stores the pointer. The caller must keep
 * the buffer valid until the packet is retrieved or discarded (see
 * JitterBufferPacket).
 *
 * @param jitter Jitter buffer state
 * @param packet Incoming packet
*/
void jitter_buffer_put(JitterBuffer *jitter, const JitterBufferPacket *packet);

/** Get one packet from the jitter buffer. On JITTER_BUFFER_OK, packet->data
 * is set to the same pointer that was passed to jitter_buffer_put()
 * (zero-copy) and packet->len to the stored length; any buffer/length the
 * caller placed in packet beforehand is ignored. The data buffer remains
 * owned by the caller.
 *
 * @param jitter Jitter buffer state
 * @param packet Returned packet
 * @param desired_span Number of samples (or units) we wish to get from the buffer (no guarantee)
 * @param current_timestamp Timestamp for the returned packet
*/
int jitter_buffer_get(JitterBuffer *jitter, JitterBufferPacket *packet, spx_int32_t desired_span, spx_int32_t *start_offset);

/** Used right after jitter_buffer_get() to obtain another packet that would have the same timestamp.
 * This is mainly useful for media where a single "frame" can be split into several packets.
 * The data pointer is handed back zero-copy, exactly like jitter_buffer_get().
 *
 * @param jitter Jitter buffer state
 * @param packet Returned packet
 */
int jitter_buffer_get_another(JitterBuffer *jitter, JitterBufferPacket *packet);

/** Get pointer timestamp of jitter buffer
 *
 * @param jitter Jitter buffer state
*/
int jitter_buffer_get_pointer_timestamp(JitterBuffer *jitter);

/** Advance by one tick
 *
 * @param jitter Jitter buffer state
*/
void jitter_buffer_tick(JitterBuffer *jitter);

/** Telling the jitter buffer about the remaining data in the application buffer
 * @param jitter Jitter buffer state
 * @param rem Amount of data buffered by the application (timestamp units)
 */
void jitter_buffer_remaining_span(JitterBuffer *jitter, spx_uint32_t rem);

/** Used like the ioctl function to control the jitter buffer parameters
 *
 * @param jitter Jitter buffer state
 * @param request ioctl-type request (one of the JITTER_BUFFER_* macros)
 * @param ptr Data exchanged to-from function
 * @return 0 if no error, -1 if request in unknown
*/
int jitter_buffer_ctl(JitterBuffer *jitter, int request, void *ptr);

int jitter_buffer_update_delay(JitterBuffer *jitter, JitterBufferPacket *packet, spx_int32_t *start_offset);

/* @} */

#ifdef __cplusplus
}
#endif

#endif
