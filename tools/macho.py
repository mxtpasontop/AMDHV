"""Strict thin x86_64 Mach-O metadata parser; never follows file pointers."""
import struct
import uuid


def x86_slice(data):
    """Return the unique bounded x86_64 slice of a thin or universal image."""
    if len(data) < 8:
        raise ValueError('truncated universal header')
    magic, count = struct.unpack_from('>II', data)
    if magic not in (0xcafebabe, 0xcafebabf):
        metadata(data)
        return data
    stride = 20 if magic == 0xcafebabe else 32
    if not 0 < count <= 32 or count > (len(data)-8)//stride:
        raise ValueError('invalid universal architecture count')
    header_end = 8+count*stride
    found, spans = None, []
    for index in range(count):
        start = 8+stride*index
        if stride == 20:
            cpu, _, offset, size, alignment = struct.unpack_from('>5I', data, start)
        else:
            cpu, _, offset, size, alignment, reserved = struct.unpack_from('>IIQQII', data, start)
            if reserved:
                raise ValueError('reserved universal header field')
        if alignment > 31 or offset & ((1 << alignment)-1) or offset < header_end or not size or offset > len(data) or size > len(data)-offset:
            raise ValueError('invalid universal slice extent')
        if any(offset < end and offset+size > first for first, end in spans):
            raise ValueError('overlapping universal slices')
        spans.append((offset, offset+size))
        if cpu == 0x01000007:
            if found is not None:
                raise ValueError('duplicate x86_64 slices')
            found = data[offset:offset+size]
    if found is None:
        raise ValueError('missing x86_64 slice')
    metadata(found)
    return found


def metadata(data):
    if len(data) < 32:
        raise ValueError('truncated Mach-O header')
    magic, cpu, _, kind, count, size, _, _ = struct.unpack_from('<8I', data)
    if magic != 0xfeedfacf or cpu != 0x01000007:
        raise ValueError('expected thin x86_64 Mach-O')
    if size > len(data)-32 or count > size//8:
        raise ValueError('invalid load command extent')
    cursor, end, identity = 32, 32+size, None
    for _ in range(count):
        if end-cursor < 8:
            raise ValueError('truncated load command')
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length % 8 or length > end-cursor:
            raise ValueError('invalid load command size')
        if command == 0x1b:
            if identity or length != 24:
                raise ValueError('invalid/duplicate UUID command')
            identity = str(uuid.UUID(bytes=data[cursor+8:cursor+24])).upper()
        cursor += length
    if cursor != end or not identity:
        raise ValueError('missing UUID or inconsistent load commands')
    return {'uuid': identity, 'filetype': kind, 'architecture': 'x86_64'}


def address_bytes(data, address, size):
    """Read an entire virtual span from one file-backed LC_SEGMENT_64."""
    metadata(data)
    if type(address) is not int or type(size) is not int or address < 0 or size <= 0 or address+size > 1 << 64:
        raise ValueError('invalid virtual span')
    count = struct.unpack_from('<I', data, 16)[0]
    cursor = 32
    found = None
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, cursor)
        if command == 0x19:
            if length < 72:
                raise ValueError('truncated segment')
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, cursor+24)
            sections = struct.unpack_from('<I', data, cursor+64)[0]
            if length != 72+80*sections or vmaddr+vmsize > 1 << 64 or (vmsize and filesize > vmsize) or fileoff > len(data) or filesize > len(data)-fileoff:
                raise ValueError('invalid segment extent')
            # The installed kernel has file-backed __CTF metadata with vmsize=0.
            # Validate its file bounds, but never treat it as mapped instructions.
            if vmsize and address >= vmaddr and address-vmaddr <= filesize and size <= filesize-(address-vmaddr):
                start = fileoff+(address-vmaddr)
                if found is not None:
                    raise ValueError('ambiguous virtual span')
                found = data[start:start+size]
        cursor += length
    if found is not None:
        return found
    raise ValueError('virtual span is not file-backed')
