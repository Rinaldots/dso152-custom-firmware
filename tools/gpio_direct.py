import struct,sys,collections
from capstone import *
from capstone.arm import *
BASE=0x08003C00
d=open(sys.argv[1],'rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB+CS_MODE_MCLASS); md.detail=True
lit=lambda a: struct.unpack('<I',d[a-BASE:a-BASE+4])[0] if 0<=a-BASE<=len(d)-4 else 0
G={0x40010800:'GPIOA',0x40010C00:'GPIOB',0x40011000:'GPIOC',0x40010810:'GPIOA+0x10',0x40010c0c:'GPIOB+0x0C'}
REG={0:'CRL',4:'CRH',8:'IDR',0xc:'ODR',0x10:'BSRR',0x14:'BRR'}
code=[];off=0
while off<len(d):
  for i in md.disasm(d[off:],BASE+off): code.append(i); off=i.address-BASE+i.size
  if off<len(d): off+=2
regs={}
out=collections.Counter()
for i in code:
  ops=i.operands;m=i.mnemonic
  if m.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
    regs[ops[0].reg]=('lit',lit(((i.address+4)&~3)+ops[1].mem.disp))
  elif m.startswith(('mov','movs','mov.w')) and len(ops)==2 and ops[1].type==ARM_OP_IMM:
    regs[ops[0].reg]=('imm',ops[1].imm)
  elif m.startswith(('str','ldr')) and len(ops)==2 and ops[1].type==ARM_OP_MEM:
    b=regs.get(ops[1].mem.base)
    if b and b[0]=='lit' and b[1] in G:
      base=b[1]+ops[1].mem.disp
      port=[k for k in (0x40010800,0x40010C00,0x40011000) if k<=base<k+0x20]
      if port:
        rn=REG.get(base-port[0],hex(base-port[0]));pn={0x40010800:'A',0x40010C00:'B',0x40011000:'C'}[port[0]]
        val=regs.get(ops[0].reg) if m.startswith('str') else None
        v=hex(val[1]) if val and val[0]=='imm' else '?'
        out[(pn,m[:3],rn,v)]+=1
  elif m in('bl','pop','bx') : regs={}
  elif ops and ops[0].type==ARM_OP_REG and m not in('cmp','tst','cmn') and not m.startswith('str'):
    regs.pop(ops[0].reg,None)
for k,v in sorted(out.items()): print(k,v)
