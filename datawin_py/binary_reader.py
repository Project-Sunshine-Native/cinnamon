"""Binary reader for parsing data.win files"""

import struct
from typing import BinaryIO, Tuple


class BinaryReader:
    """Reads binary data from a file with little-endian byte order"""

    def __init__(self, file: BinaryIO, file_size: int):
        self.file = file
        self.file_size = file_size
        self.position = 0

    def read_bytes(self, count: int) -> bytes:
        """Read a fixed number of bytes"""
        data = self.file.read(count)
        self.position += len(data)
        return data

    def read_uint8(self) -> int:
        """Read unsigned 8-bit integer"""
        data = self.read_bytes(1)
        return struct.unpack('<B', data)[0]

    def read_uint16(self) -> int:
        """Read unsigned 16-bit integer (little-endian)"""
        data = self.read_bytes(2)
        return struct.unpack('<H', data)[0]

    def read_int16(self) -> int:
        """Read signed 16-bit integer (little-endian)"""
        data = self.read_bytes(2)
        return struct.unpack('<h', data)[0]

    def read_uint32(self) -> int:
        """Read unsigned 32-bit integer (little-endian)"""
        data = self.read_bytes(4)
        return struct.unpack('<I', data)[0]

    def read_int32(self) -> int:
        """Read signed 32-bit integer (little-endian)"""
        data = self.read_bytes(4)
        return struct.unpack('<i', data)[0]

    def read_uint64(self) -> int:
        """Read unsigned 64-bit integer (little-endian)"""
        data = self.read_bytes(8)
        return struct.unpack('<Q', data)[0]

    def read_int64(self) -> int:
        """Read signed 64-bit integer (little-endian)"""
        data = self.read_bytes(8)
        return struct.unpack('<q', data)[0]

    def read_float32(self) -> float:
        """Read 32-bit float (little-endian)"""
        data = self.read_bytes(4)
        return struct.unpack('<f', data)[0]

    def read_float64(self) -> float:
        """Read 64-bit float (little-endian)"""
        data = self.read_bytes(8)
        return struct.unpack('<d', data)[0]

    def read_bool32(self) -> bool:
        """Read 32-bit boolean (nonzero = True)"""
        return self.read_uint32() != 0

    def read_cstring(self, max_length: int = 256) -> str:
        """Read null-terminated string"""
        chars = b''
        for _ in range(max_length):
            byte = self.read_bytes(1)
            if not byte or byte[0] == 0:
                break
            chars += byte
        return chars.decode('utf-8', errors='replace')

    def skip(self, count: int):
        """Skip bytes without reading"""
        self.file.seek(count, 1)
        self.position += count

    def seek(self, offset: int):
        """Seek to absolute file position"""
        self.file.seek(offset)
        self.position = offset

    def tell(self) -> int:
        """Get current position"""
        return self.position

    def read_bytes_at(self, offset: int, count: int) -> bytes:
        """Read bytes at specific offset"""
        current_pos = self.position
        self.seek(offset)
        data = self.read_bytes(count)
        self.seek(current_pos)
        return data

    def at_end(self) -> bool:
        """Check if at end of file"""
        return self.position >= self.file_size
