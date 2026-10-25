import struct,sys,collections
from capstone import *
from capstone.arm import *
sys.path.insert(0,'.')
BASE=0x08003C00
d=open(sys.argv[1],'rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB+CS_MODE_MCLASS); md.detail=True
lit=lambda a: struct.unpack('<I',d[a-BASE:a-BASE+4])[0] if 0<=a-BASE<=len(d)-4 else None
G={0x40010800:'GPIOA',0x40010C00:'GPIOB',0x40011000:'GPIOC',0x40011400:'GPIOD',0x40012400:'ADC1',0x40020008:'DMA1_Ch1',0x40012C00:'TIM1',0x40000400:'TIM3',0x40000800:'TIM4'}
code=[];off=0
while off<len(d):
  got=False
  for i in md.disasm(d[off:],BASE+off): code.append(i); off=i.address-BASE+i.size; got=True
  if off<len(d): off+=2
regs={};stack={}
calls=collections.defaultdict(list)
for i in code:
  m=i.mnemonic; ops=i.operands
  try:
    if m.startswith(('movs','mov','mov.w','movw')) and len(ops)==2 and ops[0].type==ARM_OP_REG:
      if ops[1].type==ARM_OP_IMM: regs[ops[0].reg]=ops[1].imm
      elif ops[1].type==ARM_OP_REG: regs[ops[0].reg]=('sp' if ops[1].reg==ARM_REG_SP else regs.get(ops[1].reg))
    elif m.startswith('ldr') and len(ops)==2 and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_PC:
      regs[ops[0].reg]=lit(((i.address+4)&~3)+ops[1].mem.disp)
    elif m.startswith(('add','adds')) and len(ops)==3 and ops[1].type==ARM_OP_REG and ops[1].reg==ARM_REG_SP and ops[2].type==ARM_OP_IMM:
      regs[ops[0].reg]=('sp',ops[2].imm)
    elif m.startswith(('add','adds')) and len(ops)==2 and ops[1].type==ARM_OP_IMM and isinstance(regs.get(ops[0].reg),int):
      regs[ops[0].reg]+=ops[1].imm
    elif m.startswith('str') and ops[1].type==ARM_OP_MEM and ops[1].mem.base==ARM_REG_SP:
      stack[ops[1].mem.disp]=(m,regs.get(ops[0].reg))
    elif m in('bl',):
      t=ops[0].imm
      calls[t].append((i.address,regs.get(ARM_REG_R0),regs.get(ARM_REG_R1),regs.get(ARM_REG_R2),dict(stack)))
      for r in (ARM_REG_R0,ARM_REG_R1,ARM_REG_R2,ARM_REG_R3,ARM_REG_R12): regs.pop(r,None)
    elif m in('pop','bx','b','b.w') or m.startswith('push'):
      if m.startswith('push'): stack={}
      regs={} if m!='push' else regs
    elif ops and ops[0].type==ARM_OP_REG and m not in('cmp','tst','cmn'):
      regs.pop(ops[0].reg,None)
  except Exception as e: pass
# functions called with r0 = peripheral base
for t,lst in sorted(calls.items()):
  pl=[c for c in lst if c[1] in G]
  if pl:
    print('\n== func %s  (%d calls w/ periph)'%(hex(t),len(pl)))
    for a,r0,r1,r2,st in pl[:40]:
      s=lambda v: hex(v) if isinstance(v,int) else str(v)
      print('  %s %s r1=%s r2=%s'%(hex(a),G[r0],s(r1),s(r2)), {k:(v[0],s(v[1])) for k,v in st.items() if k<8} if r1=='sp' or isinstance(r1,tuple) else '')
