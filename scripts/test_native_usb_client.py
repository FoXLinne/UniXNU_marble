#!/usr/bin/env python3
"""Check command/reply ordering and paged uploads on the native EP0 transport."""
from array import array
from root_control import RootUsb


class Device:
    idProduct=0xd00e

    def __init__(self):
        self.calls=[]
        self.data=bytes((i*17)&255 for i in range(70001))
        self.answer=b''
        self.short=False

    def ctrl_transfer(self,kind,request,value,index,data):
        self.calls.append((kind,request,value,index,len(data) if isinstance(data,bytes) else data))
        if kind==0x40:
            assert request==0x30 and value==index==0
            assert len(data)==64
            packet_length=len(data)
            data=data.rstrip(b'\0')
            if data==b'getvar:version':self.answer=b'OKAYRootStage2'
            elif data==b'upload':self.answer=f'DATA{len(self.data):08x}'.encode()
            elif data==b'failure':self.answer=b'FAILdevice error'
            else:self.answer=b'OKAYaccepted'
            return packet_length
        if request==0x31:return array('B',self.answer)
        assert request==0x32 and kind==0xc0 and data<=4096
        offset=value|(index<<16)
        result=self.data[offset:offset+data]
        if offset+len(result)==len(self.data):self.answer=b'OKAY'
        return array('B',b'' if self.short else result)

    def get_active_configuration(self):
        return object()


device=Device()
channel=RootUsb(device=device)
assert channel.native and device.calls[:2]==[(0x40,0x30,0,0,64),(0xc0,0x31,0,0,512)]
assert channel.command('oem input:02:0100:0200')=='accepted'
assert channel.read_back()==device.data
assert any(call[1]==0x32 and call[3]==1 for call in device.calls)
try:channel.command('failure')
except RuntimeError as error:assert str(error)=='device error'
else:raise AssertionError('FAIL response accepted')
device.short=True
try:channel.read_back()
except RuntimeError as error:assert str(error)=='Short RootStage upload'
else:raise AssertionError('short upload accepted')
try:channel.write(b'kernel')
except RuntimeError as error:assert 'firmware phase' in str(error)
else:raise AssertionError('native upload accepted')
print('Native USB client: ordered command replies, 4 KiB chunks, high offsets, FAIL and short-read handling passed')
