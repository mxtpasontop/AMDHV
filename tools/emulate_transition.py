#!/usr/bin/env python3
"""Build and run the privileged transition tests inside QEMU TCG only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from datetime import datetime,timezone
from diagnose import probe
from build_identity import identity
ROOT=Path(__file__).resolve().parent.parent
BASE=0x100000


def image_from_object(data):
    if len(data)<64 or data[:6]!=b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64 object')
    header=struct.unpack_from('<16sHHIQQQIHHHHHH',data)
    if header[1]!=1 or header[2]!=62 or header[11]!=64:
        raise ValueError('unsupported test object format')
    offset,count=header[6],header[12]
    if offset>len(data) or count>(len(data)-offset)//64:
        raise ValueError('invalid section table')
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,offset+i*64) for i in range(count)]
    if not count or header[13]>=count:
        raise ValueError('invalid string table index')
    def contents(section):
        start,size=section[4:6]
        if start>len(data) or size>len(data)-start:
            raise ValueError('invalid section extent')
        return data[start:start+size]
    names=contents(sections[header[13]])
    if sections[header[13]][1]!=3 or any(s[0]>=len(names) for s in sections):
        raise ValueError('invalid section name table')
    text_index=next((i for i,s in enumerate(sections) if names[s[0]:].split(b'\0',1)[0]==b'.text'),None)
    if text_index is None:
        raise ValueError('missing test text')
    image=bytearray(contents(sections[text_index]))
    if sections[text_index][1]!=1 or any(s[2]&2 and s[5] and i!=text_index for i,s in enumerate(sections)):
        raise ValueError('test image must contain one allocated text section')
    relocated=[]
    for section in sections:
        if section[1]!=4 or section[7]!=text_index:
            continue
        if section[6]>=count:
            raise ValueError('invalid symbol section index')
        symsec=sections[section[6]];symbol_data=contents(symsec)
        if symsec[1]!=2 or symsec[9]!=24 or len(symbol_data)%24 or section[9]!=24 or len(contents(section))%24:
            raise ValueError('invalid relocation/symbol record')
        symbols=[struct.unpack_from('<IBBHQQ',symbol_data,i) for i in range(0,len(symbol_data),24)]
        for start in range(0,section[5],24):
            position,info,addend=struct.unpack_from('<QQq',contents(section),start)
            index,kind=info>>32,info&0xffffffff
            if index>=len(symbols):
                raise ValueError('invalid relocation symbol')
            symbol=symbols[index]
            if symbol[3] not in (text_index,0xfff1):
                raise ValueError('test object has an unresolved/external reference')
            value=symbol[4]+addend+(BASE if symbol[3]==text_index else 0)
            if kind in (2,4):
                value-=BASE+position;fmt='<i'
            elif kind==1:
                fmt='<Q'
            elif kind==10:
                fmt='<I'
            elif kind==11:
                fmt='<i'
            else:
                raise ValueError('unsupported test relocation '+str(kind))
            size=struct.calcsize(fmt)
            if position>len(image) or size>len(image)-position:
                raise ValueError('invalid relocation extent')
            if any(position<last and position+size>first for first,last in relocated):
                raise ValueError('overlapping test relocations')
            try:
                struct.pack_into(fmt,image,position,value)
            except struct.error as exc:
                raise ValueError('test relocation overflows its field') from exc
            relocated.append((position,position+size))
    if not image or len(image)>0x100000:
        raise ValueError('invalid test image size')
    return bytes(image)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',type=Path,default=ROOT/'build/emulation/qemu-build/qemu-system-x86_64')
    args=parser.parse_args()
    directory=ROOT/'build/emulation';directory.mkdir(parents=True,exist_ok=True)
    source_id=identity(ROOT)
    obj=directory/'transition-harness.o';binary=directory/'transition-harness.bin'
    compiler=['xcrun','clang'] if sys.platform=='darwin' else ['clang']
    subprocess.run(compiler+['-target','x86_64-none-elf','-nostdinc','-I',str(ROOT/'src'),'-c',str(ROOT/'tests/emulation/svm_transition.S'),'-o',str(obj)],check=True,timeout=30)
    binary.write_bytes(image_from_object(obj.read_bytes()))
    command=[str(args.qemu.resolve()),'-machine','pc,accel=tcg','-cpu','phenom,-fxsr-opt','-m','128M',
             '-display','none','-monitor','none','-serial','none','-debugcon','stdio','-no-reboot',
             '-device','isa-debug-exit,iobase=0xf4,iosize=0x04','-kernel',str(binary)]
    result=probe(*command)
    version=subprocess.check_output([str(args.qemu.resolve()),'--version'],text=True,timeout=5).strip()
    record={'schema':1,'kind':'amdhv-emulated-transition','hardware_execution':False,'source_id':source_id,'command':command,
            'timestamp_utc':datetime.now(timezone.utc).isoformat(),'emulator_version':version,
            'limitations':['Software CPU model; no live XNU, physical CPU, SMI/NMI acceptance or microarchitectural mitigation proof.',
                          'QEMU 9.2 zero-extends the failed-entry code; this harness checks its low word, not AMD high-word conformance.'],
            'image_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'emulator_sha256':hashlib.sha256(args.qemu.read_bytes()).hexdigest(),**result}
    results=directory/'results';results.mkdir(exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w',dir=results,prefix='transition-',suffix='.json',delete=False) as output:
        output.write(json.dumps(record,indent=2)+'\n');record_path=Path(output.name)
    print(result['stdout'],end='')
    if result['returncode']!=1 or 'PASS:' not in result['stdout']:
        raise SystemExit('Emulated transition test failed: '+result['stderr'])
    if identity(ROOT)!=source_id:
        raise SystemExit('Source changed during the emulated test; repeat it on a stable snapshot.')
    print('Evidence:',record_path)
    print('Software emulation passed; live kernel/hardware acceptance remains false.')


if __name__=='__main__':
    main()
