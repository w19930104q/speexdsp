#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "speex/speex_jitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

union jbpdata {
  unsigned int idx;
  unsigned char data[4];
};

/* Backing store for the packets we put into the jitter buffer. The jitter
   buffer never copies packet data, so the caller must keep the buffers
   alive until the packets are retrieved or discarded. */
static union jbpdata in_store[160];

static int failures = 0;

#define CHECK(cond, msg) do { \
   if (!(cond)) { \
      printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++; \
   } \
} while (0)

void synthIn(JitterBufferPacket *in, int idx, int span) {
  in_store[idx].idx = idx;

  in->data = (char*)in_store[idx].data;
  in->len = sizeof(in_store[idx]);
  in->timestamp = idx * 10;
  in->span = span * 10;
  in->sequence = idx;
  in->user_data = 0;
}

void jitterFill(JitterBuffer *jb) {
   JitterBufferPacket in, out;
   int i;

   jitter_buffer_reset(jb);

   for(i=0;i<100;++i) {
     synthIn(&in, i, 1);
     jitter_buffer_put(jb, &in);

     if (jitter_buffer_get(jb, &out, 10, NULL) != JITTER_BUFFER_OK) {
       printf("Fill test failed iteration %d\n", i);
       failures++;
     }
     if (out.timestamp != i * 10) {
       printf("Fill test expected %d got %d\n", i*10, out.timestamp);
       failures++;
     }
     /* Zero-copy: the returned packet must reference the caller's buffer */
     CHECK(out.data == in.data, "get() must return the same data pointer passed to put()");
     CHECK(out.len == in.len, "get() must return the same length passed to put()");
     jitter_buffer_tick(jb);
   }
}

/* user_data must be wide enough to hold a pointer and must round-trip
   bit-exactly through put()/get(). */
void userDataRoundtrip(JitterBuffer *jb) {
   JitterBufferPacket in, out;
   char payload[16];
   int sentinel_obj = 0x12345678;
   void *sentinel = &sentinel_obj;
   spx_uintptr_t before;
   int i;

   CHECK(sizeof(spx_uintptr_t) == sizeof(void*),
         "spx_uintptr_t must have the same width as a pointer");
   CHECK(sizeof(in.user_data) == sizeof(void*),
         "user_data must have the same width as a pointer");

   jitter_buffer_reset(jb);

   memset(payload, 0x5a, sizeof(payload));
   in.data = payload;
   in.len = sizeof(payload);
   in.timestamp = 0;
   in.span = 10;
   in.sequence = 0;
   in.user_data = (spx_uintptr_t)sentinel;
   before = in.user_data;

   jitter_buffer_put(jb, &in);
   memset(&out, 0, sizeof(out));
   CHECK(jitter_buffer_get(jb, &out, 10, NULL) == JITTER_BUFFER_OK,
         "get() after put() must succeed");

   /* Bit-exact round trip of a real pointer value */
   CHECK(memcmp(&out.user_data, &before, sizeof(spx_uintptr_t)) == 0,
         "user_data must round-trip bit-exactly");
   CHECK((void*)out.user_data == sentinel,
         "user_data must round-trip a pointer without truncation");
   CHECK(out.data == payload && out.len == sizeof(payload),
         "get() must hand back the caller's buffer zero-copy");
   jitter_buffer_tick(jb);

   /* All-ones pattern must also survive (catches any 32-bit truncation
      of the field on 64-bit platforms) */
   jitter_buffer_reset(jb);
   in.user_data = ~(spx_uintptr_t)0;
   before = in.user_data;
   jitter_buffer_put(jb, &in);
   memset(&out, 0, sizeof(out));
   CHECK(jitter_buffer_get(jb, &out, 10, NULL) == JITTER_BUFFER_OK,
         "get() after put() must succeed");
   for (i = 0; i < (int)sizeof(spx_uintptr_t); i++) {
      CHECK(((unsigned char*)&out.user_data)[i] == 0xFF,
            "every byte of user_data must survive the round trip");
   }
   CHECK(out.user_data == before, "all-ones user_data must round-trip");
   jitter_buffer_tick(jb);

   /* 32-bit values keep working unchanged (source compatibility) */
   jitter_buffer_reset(jb);
   in.user_data = 0xDEADBEEF;
   jitter_buffer_put(jb, &in);
   memset(&out, 0, sizeof(out));
   CHECK(jitter_buffer_get(jb, &out, 10, NULL) == JITTER_BUFFER_OK,
         "get() after put() must succeed");
   CHECK(out.user_data == (spx_uintptr_t)0xDEADBEEF,
         "32-bit user_data values must round-trip");
   jitter_buffer_tick(jb);
}

/* The jitter buffer must not free or otherwise take over the caller's
   data buffers, neither when discarding packets (reset/destroy) nor when
   retrieving them. The caller frees each buffer exactly once here; with a
   library that (double-)frees caller data this test crashes. */
void ownership(JitterBuffer *jb) {
   char *bufs[8];
   JitterBufferPacket in, out;
   int i;

   jitter_buffer_reset(jb);

   for (i = 0; i < 8; i++) {
      bufs[i] = (char*)malloc(32);
      memset(bufs[i], i + 1, 32);
      in.data = bufs[i];
      in.len = 32;
      in.timestamp = i * 10;
      in.span = 10;
      in.sequence = i;
      in.user_data = (spx_uintptr_t)bufs[i];
      jitter_buffer_put(jb, &in);
   }

   /* Discard all buffered packets: the library must not free the data */
   jitter_buffer_reset(jb);
   for (i = 0; i < 8; i++) {
      bufs[i][0] = (char)i; /* still ours, still valid */
      free(bufs[i]);
   }

   /* Same, but retrieve one packet first and let destroy() drop the rest */
   for (i = 0; i < 8; i++) {
      bufs[i] = (char*)malloc(32);
      memset(bufs[i], i + 1, 32);
      in.data = bufs[i];
      in.len = 32;
      in.timestamp = i * 10;
      in.span = 10;
      in.sequence = i;
      in.user_data = (spx_uintptr_t)bufs[i];
      jitter_buffer_put(jb, &in);
   }
   memset(&out, 0, sizeof(out));
   CHECK(jitter_buffer_get(jb, &out, 10, NULL) == JITTER_BUFFER_OK,
         "get() after put() must succeed");
   CHECK(out.data == bufs[0], "get() must return the caller's pointer");
   CHECK((void*)out.user_data == (void*)bufs[0],
         "user_data pointer must survive the round trip");
   jitter_buffer_destroy(jb);
   for (i = 0; i < 8; i++) {
      bufs[i][0] = (char)i; /* still ours, still valid */
      free(bufs[i]);
   }
}

int main()
{
   JitterBufferPacket in, out;
   int i;

   JitterBuffer *jb = jitter_buffer_init(10);

   /* Frozen sender case */
   jitterFill(jb);
   for(i=0;i<100;++i) {
     jitter_buffer_get(jb, &out, 10, NULL);
     jitter_buffer_tick(jb);
   }
   synthIn(&in, 100, 1);
   jitter_buffer_put(jb, &in);
   if (jitter_buffer_get(jb, &out, 10, NULL) != JITTER_BUFFER_OK) {
     printf("Failed frozen sender resynchronize\n");
     failures++;
   } else {
     printf("Frozen sender: Jitter %d\n", out.timestamp - 100*10);
   }

   userDataRoundtrip(jb);
   ownership(jb); /* destroys jb */

   if (failures) {
     printf("%d check(s) FAILED\n", failures);
     return 1;
   }
   printf("All jitter buffer tests passed\n");
   return 0;
}
