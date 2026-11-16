"""Compile the actual render-drain routines with a competing drain and pending arena reference."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];root=repo.parents[1];out=root/'outputs/UniXNU_marble/ios27_24B119/host-tests/render-drain';out.mkdir(parents=True,exist_ok=True)
relative='crates/metal2vulkan-runtime/src/render/deferral.rs'
current=(repo/'ios27/sources/metal2vulkan'/relative).read_text()
reference=root/'work/UniXNU/.cache/gpu-reference/metal2vulkan'
previous=subprocess.check_output(['git','-C',str(reference),'show',
    '7ef17497d3980a846d71f314feeb976fa7e0d242:'+relative],text=True)
def method(src,name):
 start=src.index('    fn '+name);begin=src.index('{',start);depth=1;end=begin+1
 while depth:
  depth+=(src[end]=='{')-(src[end]=='}');end+=1
 return src[start:end]
head='''use std::sync::{Arc,Mutex,MutexGuard,Condvar,atomic::{AtomicUsize,Ordering}};
use std::{collections::VecDeque,thread,time::Duration};
fn lock<T>(m:&Mutex<T>)->MutexGuard<'_,T>{m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)}
struct Batch{first:u32,arena:Vec<u64>}
impl Batch{fn next(&self)->u32{self.first} fn lower(&self,_:&mut Lowerer,_:bool){} fn empty(&mut self,next:u32){self.arena.clear();self.first=next;}}
struct Lowerer;
struct CommandBuffer;
impl CommandBuffer{fn end_lowered(&self,_:bool){}}
fn end_lowering(_:&mut Lowerer,_:u32,_:bool)->Option<CommandBuffer>{None}
struct SlotState{draining:bool,busy:bool,panicked:bool,handed:VecDeque<Batch>,filling:Batch,lowered:Vec<Batch>,ending:Option<u32>}
struct LoweringSlot{state:Mutex<SlotState>,lowerer:Mutex<Lowerer>,handed_back:Condvar,active:AtomicUsize}
'''
test='''
fn main(){let slot=Arc::new(LoweringSlot{state:Mutex::new(SlotState{draining:false,busy:true,panicked:false,handed:VecDeque::new(),filling:Batch{first:0,arena:vec![0xcafe]},lowered:vec![],ending:None}),lowerer:Mutex::new(Lowerer),handed_back:Condvar::new(),active:AtomicUsize::new(0)});
let a=slot.clone();let first=thread::spawn(move||{let guard=a.writable();a.drain_locked(guard,1,|_,batch|assert_eq!(batch.arena[0],0xcafe));});
for n in 0..1000{if lock(&slot.state).draining{break}assert!(n<999);thread::sleep(Duration::from_millis(1));}
let b=slot.clone();let second=thread::spawn(move||{b.drain_locked(lock(&b.state),0,|_,_|());});
thread::sleep(Duration::from_millis(20));
if second.is_finished(){if second.join().is_err(){std::process::exit(17)}std::process::exit(18)}
{lock(&slot.state).busy=false;}slot.handed_back.notify_all();
if first.join().is_err()||second.join().is_err(){std::process::exit(17)}
assert_eq!(slot.active.load(Ordering::SeqCst),0);println!("PASS: competing drain waits while an unlogged command owns the filling arena");}
'''
for label,src in [('fixed',current),('before',previous)]:
 body=method(src,'drain_locked')
 body=body.replace('guard.draining = true;','guard.draining = true;\nassert_eq!(self.active.fetch_add(1,Ordering::SeqCst),0,"overlapping arena drains");',1)
 body=body.replace('state.draining = false;','self.active.fetch_sub(1,Ordering::SeqCst);\nstate.draining = false;',1)
 helpers=(method(current,'wait_for_drainer')+'\n'+method(current,'writable')) if label=='fixed' else "fn writable(&self)->MutexGuard<'_,SlotState>{lock(&self.state)}"
 path=out/(label+'.rs');path.write_text(head+'impl LoweringSlot{'+helpers+'\n'+body+'}\n'+test)
 subprocess.run(['rustc','+1.98.0','--edition=2024',str(path),'-o',str(out/label)],check=True)
 result=subprocess.run([str(out/label)],capture_output=True,text=True,timeout=10)
 if label=='fixed':assert result.returncode==0,result.stderr;print(result.stdout.strip())
 else:assert result.returncode==17,(result.returncode,result.stderr);print('Previous source reproduces overlapping arena drains under the same worker wait')
