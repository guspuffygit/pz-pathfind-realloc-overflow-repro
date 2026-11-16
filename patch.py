"""Usage: patch.py <original libPZPathFind64.so> <output>. Rewrites reallocate_aligned to copy min(usable(old), size)."""
import struct, sys
BASE=0x5aff0; END=0x5b060
PLT=dict(aligned_alloc=0x1f120, usable=0x1fa10, memcpy=0x1f2d0, free=0x1fb20)
code=bytearray(); fix=[]; labels={}
def emit(b): code.extend(bytes.fromhex(b))
def call(n):
    code.append(0xe8); ip=BASE+len(code)+4; code.extend(struct.pack('<i',PLT[n]-ip))
def jcc(op,l): code.append(op); fix.append((len(code),l)); code.append(0)
emit('4154 55 4889fd 4883ec08 4885f6'); jcc(0x74,'tail')
emit('48893424 4889d7'); call('aligned_alloc')
emit('4989c4 4885c0'); jcc(0x74,'out')
emit('4889ef'); call('usable')
emit('488b1424 4839c2 480f47d0 4c89e7 4889ee'); call('memcpy')
emit('4889ef'); call('free')
labels['out']=len(code); emit('4883c408 4c89e0 5d 415c c3')
labels['tail']=len(code); call('free'); emit('4531e4'); jcc(0xeb,'out')
for pos,l in fix: code[pos]=(labels[l]-(pos+1))&0xff
assert len(code)<=END-BASE
code.extend(b'\x90'*(END-BASE-len(code)))
d=bytearray(open(sys.argv[1],'rb').read())
off=BASE
assert d[off:off+4]==bytes.fromhex('415455 48')
d[off:off+len(code)]=code
open(sys.argv[2],'wb').write(d)
