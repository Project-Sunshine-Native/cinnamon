#include "binary_reader.h"
#include "utils.h"
#include "binary_utils.h"

#include <stdlib.h>
#include <string.h>

BinaryReader BinaryReader_create(FILE* file, size_t fileSize) {
    return (BinaryReader){.file = file, .fileSize = fileSize, .buffer = nullptr, .bufferBase = 0, .bufferSize = 0, .bufferPos = 0};
}

void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size) {
    reader->buffer = buffer;
    reader->bufferBase = baseOffset;
    reader->bufferSize = size;
    reader->bufferPos = 0;
}

void BinaryReader_clearBuffer(BinaryReader* reader) {
    reader->buffer = nullptr;
    reader->bufferBase = 0;
    reader->bufferSize = 0;
    reader->bufferPos = 0;
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
    if (reader->buffer != nullptr) {
        if (reader->bufferPos + bytes > reader->bufferSize) {
            size_t absPos = reader->bufferBase + reader->bufferPos;
            fprintf(stderr, "BinaryReader: buffer read error at position 0x%zX (requested %zu bytes, buffer has %zu remaining)\n", absPos, bytes, reader->bufferSize - reader->bufferPos);
            abort();
        }
        memcpy(dest, reader->buffer + reader->bufferPos, bytes);
        reader->bufferPos += bytes;
        return;
    }

    size_t read = fread(dest, 1, bytes, reader->file);
    if (read != bytes) {
        long pos = ftell(reader->file) - (long) read;
        fprintf(stderr, "BinaryReader: read error at position 0x%lX (requested %zu bytes, got %zu, file size 0x%zX)\n", pos, bytes, read, reader->fileSize);
        abort();
    }
}

uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t value;
    readCheck(reader, &value, 1);
    return value;
}

int16_t BinaryReader_readInt16(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[2];
    readCheck(reader, raw, 2);
    return BinaryUtils_readInt16(raw);
#else
    int16_t value;
    readCheck(reader, &value, 2);
    return value;
#endif
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[2];
    readCheck(reader, raw, 2);
    return BinaryUtils_readUint16(raw);
#else
    uint16_t value;
    readCheck(reader, &value, 2);
    return value;
#endif
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[4];
    readCheck(reader, raw, 4);
    return BinaryUtils_readInt32(raw);
#else
    int32_t value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[4];
    readCheck(reader, raw, 4);
    return BinaryUtils_readUint32(raw);
#else
    uint32_t value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

float BinaryReader_readFloat32(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[4];
    readCheck(reader, raw, 4);
    return BinaryUtils_readFloat32(raw);
#else
    float value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[8];
    readCheck(reader, raw, 8);
    return (uint64_t) BinaryUtils_readInt64(raw);
#else
    uint64_t value;
    readCheck(reader, &value, 8);
    return value;
#endif
}

int64_t BinaryReader_readInt64(BinaryReader* reader) {
#if CINNAMON_BIG_ENDIAN
    uint8_t raw[8];
    readCheck(reader, raw, 8);
    return BinaryUtils_readInt64(raw);
#else
    int64_t value;
    readCheck(reader, &value, 8);
    return value;
#endif
}

bool BinaryReader_readBool32(BinaryReader* reader) {
    return BinaryReader_readUint32(reader) != 0;
}

void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count) {
    readCheck(reader, dest, count);
}

uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count) {
    uint8_t* buf = safeMalloc(count);

    if (reader->buffer != nullptr) {
        if (offset < reader->bufferBase || offset + count > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: readBytesAt offset 0x%zX+%zu out of buffer range [0x%zX, 0x%zX)\n", offset, count, reader->bufferBase, reader->bufferBase + reader->bufferSize);
            abort();
        }
        size_t savedPos = reader->bufferPos;
        memcpy(buf, reader->buffer + (offset - reader->bufferBase), count);
        reader->bufferPos = savedPos;
        return buf;
    }

    long savedPos = ftell(reader->file);
    fseek(reader->file, (long) offset, SEEK_SET);
    readCheck(reader, buf, count);
    fseek(reader->file, savedPos, SEEK_SET);
    return buf;
}

void BinaryReader_skip(BinaryReader* reader, size_t bytes) {
    if (reader->buffer != nullptr) {
        reader->bufferPos += bytes;
        return;
    }
    fseek(reader->file, (long) bytes, SEEK_CUR);
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    if (reader->buffer != nullptr) {
        if (position < reader->bufferBase || position > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: buffer seek to 0x%zX out of buffer range [0x%zX, 0x%zX]\n", position, reader->bufferBase, reader->bufferBase + reader->bufferSize);
            abort();
        }
        reader->bufferPos = position - reader->bufferBase;
        return;
    }

    if (position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek to 0x%zX out of bounds (file size 0x%zX)\n", position, reader->fileSize);
        abort();
    }
    fseek(reader->file, (long) position, SEEK_SET);
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    if (reader->buffer != nullptr) {
        return reader->bufferBase + reader->bufferPos;
    }
    return (size_t) ftell(reader->file);
}
