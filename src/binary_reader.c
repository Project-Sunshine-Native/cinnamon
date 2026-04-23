#include "binary_reader.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

//Big endian read for Wii U
#ifdef __WIIU__
uint16_t BinaryReader_readUint16LE(const void* data) {
    const uint8_t* bytes = (const uint8_t*) data;
    return (uint16_t) ((uint16_t) bytes[0] |
                       ((uint16_t) bytes[1] << 8));
}

int16_t BinaryReader_readInt16LE(const void* data) {
    return (int16_t) BinaryReader_readUint16LE(data);
}

uint32_t BinaryReader_readUint32LE(const void* data) {
    const uint8_t* bytes = (const uint8_t*) data;
    return (uint32_t) ((uint32_t) bytes[0] |
                       ((uint32_t) bytes[1] << 8) |
                       ((uint32_t) bytes[2] << 16) |
                       ((uint32_t) bytes[3] << 24));
}

int32_t BinaryReader_readInt32LE(const void* data) {
    return (int32_t) BinaryReader_readUint32LE(data);
}

uint64_t BinaryReader_readUint64LE(const void* data) {
    const uint8_t* bytes = (const uint8_t*) data;
    return (uint64_t) ((uint64_t) bytes[0] |
                       ((uint64_t) bytes[1] << 8) |
                       ((uint64_t) bytes[2] << 16) |
                       ((uint64_t) bytes[3] << 24) |
                       ((uint64_t) bytes[4] << 32) |
                       ((uint64_t) bytes[5] << 40) |
                       ((uint64_t) bytes[6] << 48) |
                       ((uint64_t) bytes[7] << 56));
}

float BinaryReader_readFloat32LE(const void* data) {
    uint32_t bits = BinaryReader_readUint32LE(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
#endif

BinaryReader BinaryReader_create(FILE* file, size_t fileSize) {
    return (BinaryReader){.file = file, .fileSize = fileSize, .buffer = NULL, .bufferBase = 0, .bufferSize = 0, .bufferPos = 0};
}

void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size) {
    if (!reader->file) {
        fprintf(stderr, "BinaryReader: file pointer is invalid or file is gone\n");
        exit(1);
    }
    
    reader->buffer = buffer;
    reader->bufferBase = baseOffset;
    reader->bufferSize = size;
    reader->bufferPos = 0;
}

void BinaryReader_clearBuffer(BinaryReader* reader) {
    if (!reader->file) {
        fprintf(stderr, "BinaryReader: file pointer is invalid or file is gone\n");
        exit(1);
    }

    reader->buffer = NULL;
    reader->bufferBase = 0;
    reader->bufferSize = 0;
    reader->bufferPos = 0;
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
    if (!reader->file) {
        fprintf(stderr, "BinaryReader: file pointer is invalid or file is gone\n");
        exit(1);
    }
    
    if (reader->buffer != NULL) {
        if (reader->bufferPos + bytes > reader->bufferSize) {
            size_t absPos = reader->bufferBase + reader->bufferPos;
            fprintf(stderr, "BinaryReader: buffer read error at position 0x%zX (requested %zu bytes, buffer has %zu remaining)\n", absPos, bytes, reader->bufferSize - reader->bufferPos);
            exit(1);
        }
        memcpy(dest, reader->buffer + reader->bufferPos, bytes);
        reader->bufferPos += bytes;
        return;
    }

    size_t read = fread(dest, 1, bytes, reader->file);
    if (read != bytes) {
        long pos = ftell(reader->file) - (long) read;
        fprintf(stderr, "BinaryReader: read error at position 0x%lX (requested %zu bytes, got %zu, file size 0x%zX)\n", pos, bytes, read, reader->fileSize);
        exit(1);
    }
}

uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t value;
    readCheck(reader, &value, 1);
    return value;
}

int16_t BinaryReader_readInt16(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[2];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readInt16LE(bytes);
#else
    int16_t value;
    readCheck(reader, &value, 2);
    return value;
#endif
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[2];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readUint16LE(bytes);
#else
    uint16_t value;
    readCheck(reader, &value, 2);
    return value;
#endif
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[4];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readInt32LE(bytes);
#else
    int32_t value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[4];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readUint32LE(bytes);
#else
    uint32_t value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

float BinaryReader_readFloat32(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[4];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readFloat32LE(bytes);
#else
    float value;
    readCheck(reader, &value, 4);
    return value;
#endif
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
#ifdef __WIIU__
    uint8_t bytes[8];
    readCheck(reader, bytes, sizeof(bytes));
    return BinaryReader_readUint64LE(bytes);
#else
    uint64_t value;
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

    if (reader->buffer != NULL) {
        if (offset < reader->bufferBase || offset + count > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: readBytesAt offset 0x%zX+%zu out of buffer range [0x%zX, 0x%zX)\n", offset, count, reader->bufferBase, reader->bufferBase + reader->bufferSize);
            exit(1);
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
    if (reader->buffer != NULL) {
        reader->bufferPos += bytes;
        return;
    }
    fseek(reader->file, (long) bytes, SEEK_CUR);
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    if (reader->buffer != NULL) {
        if (position < reader->bufferBase || position > reader->bufferBase + reader->bufferSize) {
            fprintf(stderr, "BinaryReader: buffer seek to 0x%zX out of buffer range [0x%zX, 0x%zX]\n", position, reader->bufferBase, reader->bufferBase + reader->bufferSize);
            exit(1);
        }
        reader->bufferPos = position - reader->bufferBase;
        return;
    }

    if (position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek to 0x%zX out of bounds (file size 0x%zX)\n", position, reader->fileSize);
        exit(1);
    }
    fseek(reader->file, (long) position, SEEK_SET);
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    if (reader->buffer != NULL) {
        return reader->bufferBase + reader->bufferPos;
    }
    return (size_t) ftell(reader->file);
}
