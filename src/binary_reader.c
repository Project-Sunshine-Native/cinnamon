#include "binary_reader.h"

#include <stdlib.h>
#include <string.h>

BinaryReader BinaryReader_create(FILE* file, size_t fileSize) {
    return (BinaryReader){.file = file, .fileSize = fileSize};
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
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
    int16_t value;
    readCheck(reader, &value, 2);
    return value;
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
    uint16_t value;
    readCheck(reader, &value, 2);
    return value;
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
    int32_t value;
    readCheck(reader, &value, 4);
    return value;
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
    uint32_t value;
    readCheck(reader, &value, 4);
    return value;
}

float BinaryReader_readFloat32(BinaryReader* reader) {
    float value;
    readCheck(reader, &value, 4);
    return value;
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
    uint64_t value;
    readCheck(reader, &value, 8);
    return value;
}

bool BinaryReader_readBool32(BinaryReader* reader) {
    return BinaryReader_readUint32(reader) != 0;
}

void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count) {
    readCheck(reader, dest, count);
}

uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count) {
    uint8_t* buf = malloc(count);
    long savedPos = ftell(reader->file);
    fseek(reader->file, (long) offset, SEEK_SET);
    readCheck(reader, buf, count);
    fseek(reader->file, savedPos, SEEK_SET);
    return buf;
}

void BinaryReader_skip(BinaryReader* reader, size_t bytes) {
    fseek(reader->file, (long) bytes, SEEK_CUR);
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    if (position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek to 0x%zX out of bounds (file size 0x%zX)\n", position, reader->fileSize);
        exit(1);
    }
    fseek(reader->file, (long) position, SEEK_SET);
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    return (size_t) ftell(reader->file);
}
