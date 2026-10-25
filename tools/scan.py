import struct,sys,collections
from capstone import *
from capstone.arm import *
BASE=0x08003C00
d=open(sys.argv[1],'rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB+CS_MODE_MCLASS); md.detail=True
lit=lambda a: struct.unpack('<I',d[a-BASE:a-BASE+4])[0] if 0<=a-BASE<=len(d)-4 else None
periph={0x40010800:'GPIOA',0x40010C00:'GPIOB',0x40011000:'GPIOC',0x40011400:'GPIOD',0x40011800:'GPIOE',
0x40012400:'ADC1',0x40012800:'ADC2',0x40020000:'DMA1',0x40021000:'RCC',0x40010000:'AFIO',0x40010400:'EXTI',
0x40000000:'TIM2',0x40000400:'TIM3',0x40000800:'TIM4',0x40012C00:'TIM1',0x40013000:'SPI1',0x40003800:'SPI2',
0x40013800:'USART1',0x40004400:'USART2',0x40005C00:'USB',0x40006000:'USBRAM',0x40005400:'I2C1',0x40007400:'DAC',0x40002800:'RTC',0x40007000:'PWR',0x40022000:'FLASH',0xE000E010:'SysTick',0x40002C00:'WWDG',0x40003000:'IWDG',0x40000C00:'TIM5'}
def name(v):
  for b,n in periph.items():
    if b<=v<b+0x400: return n,v-b
  return None
# linear sweep (skip undecodable halfwords)
hits=collections.defaultdict(list)
off=0
code=[]
while off<len(d):
  got=False
  for i in md.disasm(d[off:],BASE+off):
    code.append(i); off=i.address-BASE+i.size; got=True
  if not got or off<len(d): off+=2
for i in code:
  if i.id==ARM_INS_LDR and len(i.operands)==2 and i.operands[1].type==ARM_OP_MEM and i.operands[1].mem.base==ARM_REG_PC:
    a=((i.address+4)&~3)+i.operands[1].mem.disp
    v=lit(a)
    if v and name(v): hits[name(v)[0]].append((i.address,v))
for k in sorted(hits): print(k,len(hits[k]),sorted(set(hex(v) for _,v in hits[k]))[:12])
