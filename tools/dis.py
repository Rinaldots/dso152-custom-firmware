import sys
from capstone import *
BASE=0x08003C00
d=open(sys.argv[1],'rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB+CS_MODE_MCLASS)
a=int(sys.argv[2],16); n=int(sys.argv[3],16)
for i in md.disasm(d[a-BASE:a-BASE+n],a):
  print(hex(i.address),i.mnemonic,i.op_str)
