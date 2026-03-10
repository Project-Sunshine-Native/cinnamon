#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    FILE* file;
    size_t fileSize;
} BinaryReader;

BinaryReader BinaryReader_create(FILE* file, size_t fileSize);

uint8_t BinaryReader_readUint8(BinaryReader* reader);
int16_t BinaryReader_readInt16(BinaryReader* reader);
uint16_t BinaryReader_readUint16(BinaryReader* reader);
int32_t BinaryReader_readInt32(BinaryReader* reader);
uint32_t BinaryReader_readUint32(BinaryReader* reader);
float BinaryReader_readFloat32(BinaryReader* reader);
uint64_t BinaryReader_readUint64(BinaryReader* reader);
bool BinaryReader_readBool32(BinaryReader* reader);

// Copies 'count' bytes from the current position into 'dest'.
void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count);

// Reads 'count' bytes from position 'offset' into a newly allocated buffer.
// Caller must free the returned buffer.
uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count);

void BinaryReader_skip(BinaryReader* reader, size_t bytes);
void BinaryReader_seek(BinaryReader* reader, size_t position);
size_t BinaryReader_getPosition(BinaryReader* reader);
