import importlib.util,json,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('live_hid',Path(__file__).with_name('live-hid.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MailboxTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
  self.live=m.LiveHid.__new__(m.LiveHid);self.live.directory=Path(self.tmp.name)
  self.live.last_id=0;self.live.events=[];self.live.identity={'kernel_sha256':'synthetic'}
  self.live.fields={name:(0x1000+index*0x1000,size) for index,(name,size) in enumerate(m.FIELDS.items())}
  self.mem={address:bytes(size) for address,size in self.live.fields.values()};self.writes=[]
 def packet(self,command):
  if command.startswith('m'):
   a,n=(int(x,16) for x in command[1:].split(','));return self.mem[a][:n].hex()
  if command.startswith('M'):
   location,data=command[1:].split(':');a,n=(int(x,16) for x in location.split(','));b=bytes.fromhex(data)
   self.assertEqual(n,len(b));self.writes.append((a,b));self.mem[a]=b;return 'OK'
  self.fail(command)
 def send(self,request):
  (self.live.directory/'command.json').write_text(json.dumps(request));return self.live.checkpoint(self.packet,1)
 def test_home_only_changes_owned_mailbox_once(self):
  self.assertFalse(self.send({'id':1,'action':'home'}));self.live.checkpoint(self.packet,2)
  self.assertEqual(self.writes,[(self.live.fields['native_hid_input_command'][0],b'\x01\0\0\0')])
  self.assertEqual(self.live.last_id,1)
 def test_pending_request_not_overwritten(self):
  address=self.live.fields['native_hid_input_command'][0];self.mem[address]=b'\x01\0\0\0'
  self.send({'id':2,'action':'home'});self.assertFalse(self.writes);self.assertEqual(self.live.last_id,0)
  self.mem[address]=bytes(4);self.live.checkpoint(self.packet,2);self.assertEqual(self.live.last_id,2)
 def test_touch_publishes_one_finger_with_even_sequence(self):
  names=('native_touch_seq','native_touch_count','native_touch_xs','native_touch_ys','native_touch_ids')
  self.live.touch={n:0x100000+i*0x100 for i,n in enumerate(names)}
  for a in self.live.touch.values():self.mem[a]=bytes(4)
  self.assertFalse(self.send({'id':1,'action':'touch','x':100,'y':200}))
  w={a:int.from_bytes(b,'little') for a,b in self.writes}
  t=self.live.touch
  self.assertEqual((w[t['native_touch_xs']],w[t['native_touch_ys']],w[t['native_touch_count']],w[t['native_touch_ids']]),(100,200,1,1))
  self.assertEqual(w[t['native_touch_seq']],2)
  self.writes.clear();self.assertFalse(self.send({'id':2,'action':'lift'}))
  w={a:int.from_bytes(b,'little') for a,b in self.writes};self.assertEqual((w[t['native_touch_count']],w[t['native_touch_seq']]),(0,4))
 def test_touch_coordinate_outside_axes_refused(self):
  names=('native_touch_seq','native_touch_count','native_touch_xs','native_touch_ys','native_touch_ids')
  self.live.touch={n:0x100000+i*0x100 for i,n in enumerate(names)}
  for a in self.live.touch.values():self.mem[a]=bytes(4)
  with self.assertRaises(RuntimeError):self.send({'id':1,'action':'touch','x':30480,'y':0})
 def test_stop_does_not_write_guest(self):
  self.assertTrue(self.send({'id':1,'action':'stop'}));self.assertFalse(self.writes)
 def test_refuse_unknown_action_or_fields(self):
  for r in ({'id':1,'action':'write'},{'id':True,'action':'home'},{'id':0,'action':'home'},
            {'id':10001,'action':'home'},{'id':1,'action':'home','address':0x2000}):
   with self.assertRaises(RuntimeError):self.send(r)
  self.assertFalse(self.writes)
 def test_refuse_failed_mailbox_readback(self):
  normal=self.packet
  def reject(cmd):return 'E01' if cmd.startswith('M') else normal(cmd)
  (self.live.directory/'command.json').write_text('{"id":1,"action":"home"}')
  with self.assertRaises(RuntimeError):self.live.checkpoint(reject,1)
 def test_no_request_is_read_only(self):
  self.assertFalse(self.live.checkpoint(self.packet,1));self.assertFalse(self.writes)
 def test_oversized_request_refused(self):
  (self.live.directory/'command.json').write_text(' '*1025)
  with self.assertRaises(RuntimeError):self.live.checkpoint(self.packet,1)
if __name__=='__main__':unittest.main()
