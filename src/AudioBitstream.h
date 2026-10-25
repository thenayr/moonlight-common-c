#pragma once

// Audio bitstream passthrough (Dolby MAT / IEC 61937) receive side.
//
// The host sends packed IEC 61937 frames cut into fixed-size chunks on RTP
// payload type 100. This rebuilds each frame's exact raw bytes. The wire
// contract is Vibepollo's docs/audio-bitstream-protocol.md; keep the two in sync.

#include <stdbool.h>
#include <stdint.h>

#define RTP_PAYLOAD_TYPE_AUDIO_BITSTREAM 100

// Every chunk is exactly this long after decryption.
#define AB_CHUNK_BYTES 1200
#define AB_CHUNK_HEADER_BYTES 12
#define AB_CHUNK_DATA_BYTES (AB_CHUNK_BYTES - AB_CHUNK_HEADER_BYTES)
#define AB_CHUNK_VERSION 1
#define AB_MAX_PACKED_FRAME_BYTES (1u << 19)
#define AB_MAX_CHUNKS ((AB_MAX_PACKED_FRAME_BYTES + AB_CHUNK_DATA_BYTES - 1) / AB_CHUNK_DATA_BYTES)

// Packed frame format (zero-skip + data segments).
#define AB_FRAME_VERSION 1
#define AB_FRAME_HEADER_BYTES 8
#define AB_SEGMENT_HEADER_BYTES 4
#define AB_MAX_RAW_FRAME_BYTES (1u << 18)

// Called with each rebuilt frame: the exact raw IEC 61937 bytes, and the
// AUDIO_BITSTREAM_FLAG_* flags from Limelight.h.
typedef void (*AbFrameCallback)(const unsigned char* raw, int length, int flags);

typedef struct _AUDIO_BITSTREAM_STATE {
    bool active;
    bool done;
    bool pendingDiscontinuity;
    uint16_t frameId;
    uint16_t chunkCount;
    uint16_t chunksReceived;
    uint32_t frameLength;
    uint8_t have[AB_MAX_CHUNKS];

    unsigned char* packed;
    uint32_t packedCapacity;
    unsigned char* raw;
    uint32_t rawCapacity;

    uint32_t lostFrames;
    uint32_t badChunks;
    uint32_t badFrames;
} AUDIO_BITSTREAM_STATE, *PAUDIO_BITSTREAM_STATE;

void AbInitialize(PAUDIO_BITSTREAM_STATE state);
void AbCleanup(PAUDIO_BITSTREAM_STATE state);

// Takes one decrypted chunk. A frame missing any chunk is dropped and the next
// rebuilt frame is flagged as a discontinuity. Returns false if the chunk or
// the frame it completed was malformed (and ignored).
bool AbPushChunk(PAUDIO_BITSTREAM_STATE state, const unsigned char* chunk, int length, AbFrameCallback callback);

// Rebuilds one packed frame's raw bytes into out. Returns false, without a
// usable result, if the frame is malformed or larger than outCapacity.
bool AbUnpackFrame(const unsigned char* packed, uint32_t packedLength,
                   unsigned char* out, uint32_t outCapacity, uint32_t* outLength, int* flags);
