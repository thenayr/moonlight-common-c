#include "Limelight-internal.h"

static uint32_t readLe(const unsigned char* p, int bytes) {
    uint32_t value = 0;
    for (int i = 0; i < bytes; i++) {
        value |= (uint32_t)p[i] << (8 * i);
    }
    return value;
}

static bool reserve(unsigned char** buffer, uint32_t* capacity, uint32_t needed) {
    if (*capacity >= needed) {
        return true;
    }

    unsigned char* grown = realloc(*buffer, needed);
    if (grown == NULL) {
        return false;
    }
    *buffer = grown;
    *capacity = needed;
    return true;
}

void AbInitialize(PAUDIO_BITSTREAM_STATE state) {
    memset(state, 0, sizeof(*state));
}

void AbCleanup(PAUDIO_BITSTREAM_STATE state) {
    free(state->packed);
    free(state->raw);
    memset(state, 0, sizeof(*state));
}

bool AbUnpackFrame(const unsigned char* packed, uint32_t packedLength,
                   unsigned char* out, uint32_t outCapacity, uint32_t* outLength, int* flags) {
    if (packedLength < AB_FRAME_HEADER_BYTES || packed[0] != AB_FRAME_VERSION) {
        return false;
    }

    uint32_t segments = readLe(&packed[2], 2);
    uint32_t rawLength = readLe(&packed[4], 4);
    if (rawLength > AB_MAX_RAW_FRAME_BYTES || rawLength > outCapacity) {
        return false;
    }

    memset(out, 0, rawLength);

    uint32_t at = AB_FRAME_HEADER_BYTES;
    uint32_t cursor = 0;
    for (uint32_t s = 0; s < segments; s++) {
        if (packedLength - at < AB_SEGMENT_HEADER_BYTES) {
            return false;
        }

        uint32_t skip = readLe(&packed[at], 2);
        uint32_t length = readLe(&packed[at + 2], 2);
        at += AB_SEGMENT_HEADER_BYTES;
        cursor += skip;
        if (cursor > rawLength || rawLength - cursor < length || packedLength - at < length) {
            return false;
        }

        memcpy(&out[cursor], &packed[at], length);
        at += length;
        cursor += length;
    }

    // Trailing bytes mean the frame isn't what the header describes
    if (at != packedLength) {
        return false;
    }

    *outLength = rawLength;
    *flags = packed[1];
    return true;
}

// The packed frame repeats the flags its chunks carry, so they come from it.
static bool deliverFrame(PAUDIO_BITSTREAM_STATE state, AbFrameCallback callback) {
    uint32_t rawLength;
    int frameFlags;

    if (!reserve(&state->raw, &state->rawCapacity, AB_MAX_RAW_FRAME_BYTES) ||
        !AbUnpackFrame(state->packed, state->frameLength, state->raw, state->rawCapacity, &rawLength, &frameFlags)) {
        state->badFrames++;
        state->pendingDiscontinuity = true;
        Limelog("Dropping malformed audio bitstream frame %u\n", state->frameId);
        return false;
    }

    if (state->pendingDiscontinuity) {
        frameFlags |= AUDIO_BITSTREAM_FLAG_DISCONTINUITY;
        state->pendingDiscontinuity = false;
    }
    callback(state->raw, (int)rawLength, frameFlags);
    return true;
}

bool AbPushChunk(PAUDIO_BITSTREAM_STATE state, const unsigned char* chunk, int length, AbFrameCallback callback) {
    if (length != AB_CHUNK_BYTES || chunk[0] != AB_CHUNK_VERSION) {
        state->badChunks++;
        return false;
    }

    uint16_t frameId = (uint16_t)readLe(&chunk[2], 2);
    uint32_t index = readLe(&chunk[4], 2);
    uint32_t count = readLe(&chunk[6], 2);
    uint32_t frameLength = readLe(&chunk[8], 4);
    uint32_t expectedCount = frameLength ? (frameLength + AB_CHUNK_DATA_BYTES - 1) / AB_CHUNK_DATA_BYTES : 1;
    if (frameLength > AB_MAX_PACKED_FRAME_BYTES || count != expectedCount || index >= count) {
        state->badChunks++;
        return false;
    }

    if (!state->active || frameId != state->frameId) {
        if (state->active) {
            // The previous frame is lost if it never completed, and so is every
            // frame whose id was skipped entirely (e.g. a one-chunk frame).
            uint16_t skipped = (uint16_t)(frameId - state->frameId - 1);
            uint32_t lost = state->done ? 0 : 1;
            if (skipped < 0x8000) {
                lost += skipped;
            }
            else {
                // Older than the last frame: out of order, so don't splice it in
                lost++;
            }
            if (lost) {
                state->lostFrames += lost;
                state->pendingDiscontinuity = true;
            }
        }

        if (!reserve(&state->packed, &state->packedCapacity, count * AB_CHUNK_DATA_BYTES)) {
            state->active = false;
            state->pendingDiscontinuity = true;
            return false;
        }

        state->active = true;
        state->done = false;
        state->frameId = frameId;
        state->chunkCount = (uint16_t)count;
        state->frameLength = frameLength;
        state->chunksReceived = 0;
        memset(state->have, 0, count);
    }
    else if (count != state->chunkCount || frameLength != state->frameLength) {
        // Disagrees with the frame's other chunks
        state->badChunks++;
        return false;
    }
    else if (state->done) {
        // Duplicate of a chunk of a frame already delivered
        return true;
    }

    if (state->have[index]) {
        return true;
    }
    state->have[index] = 1;
    memcpy(&state->packed[index * AB_CHUNK_DATA_BYTES], &chunk[AB_CHUNK_HEADER_BYTES], AB_CHUNK_DATA_BYTES);

    if (++state->chunksReceived == state->chunkCount) {
        state->done = true;
        return deliverFrame(state, callback);
    }
    return true;
}
