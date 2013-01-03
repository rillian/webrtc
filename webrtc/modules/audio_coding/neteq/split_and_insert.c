/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*
 * Split an RTP payload (if possible and suitable) and insert into packet buffer.
 */

#include "mcu.h"

#include <string.h>

#include "signal_processing_library.h"
#include "neteq_error_codes.h"
#include "opus.h"

#ifndef MAX
#define MAX(x, y) (((x)>(y)) ? (x) : (y))
#endif

int WebRtcNetEQ_SplitAndInsertPayload(RTPPacket_t *packet, PacketBuf_t *Buffer_inst,
                                      SplitInfo_t *split_inst, WebRtc_Word16 *flushed)
{

    int i_ok;
    int len;
    int i;
    RTPPacket_t temp_packet;
    WebRtc_Word16 localFlushed = 0;
    const WebRtc_Word16 *pw16_startPayload;
    *flushed = 0;

    len = packet->payloadLen;

    /* Copy to temp packet that can be modified. */

    WEBRTC_SPL_MEMCPY_W8(&temp_packet,packet,sizeof(RTPPacket_t));

    if (split_inst->deltaBytes == NO_SPLIT)
    {
        /* Not splittable codec */
        i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, packet, &localFlushed);
        *flushed |= localFlushed;
        if (i_ok < 0)
        {
            return PBUFFER_INSERT_ERROR5;
        }
    }
    else if (split_inst->deltaBytes == OPUS_SPLIT)
    {
        /* Special case opus split */
        unsigned char* payload = (unsigned char*)packet->payload;
        int samples = opus_packet_get_samples_per_frame(payload, 48000);
        int frames = opus_packet_get_nb_frames(payload, len);
        /* Allocate temporary storage for constructed payloads.
         * FIXME: Does it need to outlive this function?? */
#define OPUS_RP_MAXLEN (1277+48)
        unsigned char data[OPUS_RP_MAXLEN];
        /* Don't split packets smaller than 40 ms */
        if (samples*frames < 40*48)
        {
             printf("directly inserting %d ms opus packet.\n", samples*frames/48);
             i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, packet, &localFlushed);
             *flushed |= localFlushed;
             if (i_ok < 0)
             {
                 return PBUFFER_INSERT_ERROR5;
             }

             return 0;
        }
        /* FIXME: OpusRepacketizer should be in SplitInfo_t to avoid malloc. */
        OpusRepacketizer* rp = opus_repacketizer_create();
        if (!rp)
        {
            return PBUFFER_INSERT_ERROR1;
        }
        i_ok = opus_repacketizer_cat(rp, payload, len);
        if (i_ok < 0)
        {
            opus_repacketizer_destroy(rp);
            return PBUFFER_INSERT_ERROR2;
        }
        frames = opus_repacketizer_get_nb_frames(rp);
        for (i = 0; i < frames; i++)
        {
            int bytes = opus_repacketizer_out_range(rp, i, i+1, data, OPUS_RP_MAXLEN);
            if (bytes < 0) {
                printf("ERROR splitting opus packet!\n");
                opus_repacketizer_destroy(rp);
                return PBUFFER_INSERT_ERROR3;
            }
            /* insert the split packets */
            temp_packet.payload = (WebRtc_Word16*)data;
            temp_packet.payloadLen = bytes;
            temp_packet.timeStamp = packet->timeStamp + samples * i;
            temp_packet.starts_byte1 = 0;
            printf("  opus 0x%p %d bytes %d samples %d\n",
                   temp_packet.payload,
                   temp_packet.payloadLen,
                   samples,
                   temp_packet.timeStamp);
            i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, &temp_packet, &localFlushed);
            *flushed |= localFlushed;
            if (i_ok < 0) {
                opus_repacketizer_destroy(rp);
                return PBUFFER_INSERT_ERROR4;
            }
        }
        printf("split %d ms opus packet into %d.\n",
               samples*frames/48, frames);
        opus_repacketizer_destroy(rp);
    }
    else if (split_inst->deltaBytes < -10)
    {
        /* G711, PCM16B or G722, use "soft splitting" */
        int split_size = packet->payloadLen;
        int mult = WEBRTC_SPL_ABS_W32(split_inst->deltaBytes) - 10;

        /* Find "chunk size" >= 20 ms and < 40 ms
         * split_inst->deltaTime in this case contains the number of bytes per
         * timestamp unit times 2
         */
        while (split_size >= ((80 << split_inst->deltaTime) * mult))
        {
            split_size >>= 1;
        }

        /* Make the size an even value. */
        if (split_size > 1)
        {
            split_size >>= 1;
            split_size *= 2;
        }

        temp_packet.payloadLen = split_size;
        pw16_startPayload = temp_packet.payload;
        i = 0;
        while (len >= (2 * split_size))
        {
            /* insert every chunk */
            i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, &temp_packet, &localFlushed);
            *flushed |= localFlushed;
            temp_packet.timeStamp += ((2 * split_size) >> split_inst->deltaTime);
            i++;
            temp_packet.payload = &(pw16_startPayload[(i * split_size) >> 1]);
            temp_packet.starts_byte1 = temp_packet.starts_byte1 ^ (split_size & 0x1);

            len -= split_size;
            if (i_ok < 0)
            {
                return PBUFFER_INSERT_ERROR1;
            }
        }

        /* Insert the rest */
        temp_packet.payloadLen = len;
        i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, &temp_packet, &localFlushed);
        *flushed |= localFlushed;
        if (i_ok < 0)
        {
            return PBUFFER_INSERT_ERROR2;
        }
    }
    else
    {
        /* Frame based codec, use hard splitting. */
        i = 0;
        pw16_startPayload = temp_packet.payload;
        while (len >= split_inst->deltaBytes)
        {

            temp_packet.payloadLen = split_inst->deltaBytes;
            i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, &temp_packet, &localFlushed);
            *flushed |= localFlushed;
            i++;
            temp_packet.payload = &(pw16_startPayload[(i * split_inst->deltaBytes) >> 1]);
            temp_packet.timeStamp += split_inst->deltaTime;
            temp_packet.starts_byte1 = temp_packet.starts_byte1 ^ (split_inst->deltaBytes
                & 0x1);

            if (i_ok < 0)
            {
                return PBUFFER_INSERT_ERROR3;
            }
            len -= split_inst->deltaBytes;

        }
        if (len > 0)
        {
            /* Must be a either an error or a SID frame at the end of the packet. */
            temp_packet.payloadLen = len;
            i_ok = WebRtcNetEQ_PacketBufferInsert(Buffer_inst, &temp_packet, &localFlushed);
            *flushed |= localFlushed;
            if (i_ok < 0)
            {
                return PBUFFER_INSERT_ERROR4;
            }
        }
    }

    return 0;
}

