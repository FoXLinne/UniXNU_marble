/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* macOS front end of the transport: the serial device /dev/cu.BLTH.
 *
 * macOS bluetoothd reaches an H4 controller through a BSD serial device (its "POSIX" transport: open, TIOCEXCL, tcgetattr,
 * cfmakeraw, tcsetattr, IOSSIOSPEED, then select + read on one thread and write on another). iPadOS uses the UART.SK kernel
 * control and a Skywalk pipe instead (qbt_transport.c). Both front ends implement qbt_transport.h over the same two byte FIFOs;
 * the UART, its speed and the controller's bring-up stay with the board's worker, so the line settings a client sets here are
 * recorded and answered, not applied. */
#include "qbt_transport.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <miscfs/devfs/devfs.h>
extern kern_return_t thread_terminate(thread_t);
#define BT_TTY_FIFO 8192u
#define BT_TTY_NAME "cu.BLTH"
/* The ioctls of a 64-bit client (sys/ttycom.h, sys/filio.h, IOKit/serial/ioss.h). */
#define BT_TIOCGETA   0x40487413u
#define BT_TIOCSETA   0x80487414u
#define BT_TIOCSETAW  0x80487415u
#define BT_TIOCSETAF  0x80487416u
#define BT_TIOCEXCL   0x2000740du
#define BT_TIOCNXCL   0x2000740eu
#define BT_TIOCFLUSH  0x80047410u
#define BT_TIOCDRAIN  0x2000745eu
#define BT_TIOCOUTQ   0x40047473u
#define BT_TIOCMGET   0x4004746au
#define BT_TIOCMSET   0x8004746du
#define BT_TIOCMBIS   0x8004746cu
#define BT_TIOCMBIC   0x8004746bu
#define BT_TIOCSDTR   0x20007479u
#define BT_TIOCCDTR   0x20007478u
#define BT_FIONREAD   0x4004667fu
#define BT_FIONBIO    0x8004667eu
#define BT_FIOASYNC   0x8004667du
#define BT_IOSSIOSPEED 0x80085402u
struct bt_termios64 {
    uint64_t iflag,oflag,cflag,lflag;
    uint8_t cc[20];
    uint32_t pad;
    uint64_t ispeed,ospeed;
};
static struct qbt_transport_ops bt_ops;
static LCK_GRP_DECLARE(bt_tty_grp,"unixnu.bt.tty");
static LCK_MTX_DECLARE(bt_tty_lock,&bt_tty_grp);
static struct {
    int major;
    void *node;
    bool open,closing,node_thread;
    bool rsel_used,wsel_used;
    struct selinfo rsel,wsel;
    struct bt_termios64 termios;
    uint32_t speed;
    uint32_t rx_head,rx_len,tx_head,tx_len;
    uint8_t rx[BT_TTY_FIFO],tx[BT_TTY_FIFO];
    int boot;           /* -1 pending, 0 ready, else errno */
    bool power_pending;
    int power_result;
    char firmware[64];
    uint64_t rx_dropped;
} bt_tty={.major=-1,.boot=-1,.speed=115200};

/* Native stack activity: power requests, boot-state reads, opens, speed requests. */
volatile uint32_t qbt_transport_activity[4];
static inline void bt_tty_count(unsigned index)
{
    (void)__atomic_add_fetch(&qbt_transport_activity[index],1u,__ATOMIC_RELAXED);
}
static void bt_kick(void)
{
    if(bt_ops.wake)bt_ops.wake(bt_ops.context);
}
static void bt_tty_termios_default(void)
{
    memset(&bt_tty.termios,0,sizeof(bt_tty.termios));
    /* The hardware line: 8N1, CREAD, RTS/CTS. */
    bt_tty.termios.cflag=0x300|0x800|0x8000|0x10000|0x20000;
    bt_tty.termios.ispeed=bt_tty.termios.ospeed=bt_tty.speed;
}
static void bt_tty_fifo_put(uint8_t *fifo,uint32_t head,uint32_t *len,const uint8_t *data,uint32_t n)
{
    uint32_t at=(head+*len)%BT_TTY_FIFO,first=MIN(n,BT_TTY_FIFO-at);
    memcpy(fifo+at,data,first);memcpy(fifo,data+first,n-first);
    *len+=n;
}
static void bt_tty_fifo_get(const uint8_t *fifo,uint32_t *head,uint32_t *len,uint8_t *data,uint32_t n)
{
    uint32_t first=MIN(n,BT_TTY_FIFO-*head);
    memcpy(data,fifo+*head,first);memcpy(data+first,fifo,n-first);
    *head=(*head+n)%BT_TTY_FIFO;*len-=n;
}
/* Called with the lock held: readers, then select and kqueue. */
static void bt_tty_wake_readers(void)
{
    wakeup(&bt_tty.rx_len);
    if(bt_tty.rsel_used){bt_tty.rsel_used=false;selwakeup(&bt_tty.rsel);}
}
static void bt_tty_wake_writers(void)
{
    wakeup(&bt_tty.tx_len);
    if(bt_tty.wsel_used){bt_tty.wsel_used=false;selwakeup(&bt_tty.wsel);}
}

/* UART owner side. receive returns the bytes taken: all of them while nobody has the device open (discarded), else what
 * fits in the FIFO; the owner keeps the rest and offers it again. */
uint32_t qbt_transport_receive(const uint8_t *data,uint32_t n)
{
    if(!n) return 0;
    lck_mtx_lock(&bt_tty_lock);
    if(!bt_tty.open || bt_tty.closing || bt_tty.boot) {
        bt_tty.rx_dropped+=n;lck_mtx_unlock(&bt_tty_lock);return n;
    }
    if(n>BT_TTY_FIFO-bt_tty.rx_len) n=BT_TTY_FIFO-bt_tty.rx_len;
    if(n) {
        bt_tty_fifo_put(bt_tty.rx,bt_tty.rx_head,&bt_tty.rx_len,data,n);
        bt_tty_wake_readers();
    }
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
uint32_t qbt_transport_tx_pending(void)
{
    lck_mtx_lock(&bt_tty_lock);
    uint32_t n=bt_tty.tx_len;
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
uint32_t qbt_transport_tx_take(uint8_t *data,uint32_t max)
{
    lck_mtx_lock(&bt_tty_lock);
    uint32_t n=bt_tty.tx_len<max?bt_tty.tx_len:max;
    bt_tty_fifo_get(bt_tty.tx,&bt_tty.tx_head,&bt_tty.tx_len,data,n);
    if(n) bt_tty_wake_writers();
    lck_mtx_unlock(&bt_tty_lock);
    return n;
}
bool qbt_transport_active(void)
{
    lck_mtx_lock(&bt_tty_lock);
    bool active=bt_tty.open && !bt_tty.closing && !bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);return active;
}
bool qbt_transport_connected(void)
{
    lck_mtx_lock(&bt_tty_lock);
    bool connected=bt_tty.open && !bt_tty.closing;
    lck_mtx_unlock(&bt_tty_lock);return connected;
}

/* Asks the worker to start the controller and waits for its result (the worker keeps a running controller as it is).
 * Called without the lock; returns 0 or an errno. */
static int bt_tty_power_on(void)
{
    int error=0,result;
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.power_pending || !bt_ops.power){lck_mtx_unlock(&bt_tty_lock);return EBUSY;}
    bt_tty.power_pending=true;bt_tty.power_result=EINPROGRESS;
    lck_mtx_unlock(&bt_tty_lock);
    error=bt_ops.power(bt_ops.context,true);
    lck_mtx_lock(&bt_tty_lock);
    if(error){bt_tty.power_pending=false;bt_tty.power_result=error;}
    struct timespec timeout={30,0};
    while(bt_tty.power_pending && !error)error=msleep(&bt_tty.power_pending,&bt_tty_lock,PZERO|PCATCH,"btpower",&timeout);
    result=bt_tty.power_result;lck_mtx_unlock(&bt_tty_lock);
    return error?error:result;
}

/* The serial device. One client at a time, as the daemon asks with TIOCEXCL. */
static int bt_tty_open(dev_t dev __unused,int flags __unused,int devtype __unused,struct proc *p)
{
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.open){lck_mtx_unlock(&bt_tty_lock);return EBUSY;}
    bt_tty.open=true;bt_tty.closing=false;
    bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    bt_tty_termios_default();
    int boot=bt_tty.boot;
    /* The worker's first bring-up may still be running: wait for its result before deciding. */
    struct timespec timeout={30,0};
    int error=0;
    while(bt_tty.boot<0 && !error)error=msleep(&bt_tty.boot,&bt_tty_lock,PZERO|PCATCH,"btopen",&timeout);
    boot=bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);
    bt_tty_count(2);
    /* A client on a stopped controller restarts it. */
    if(!error && boot>0)error=bt_tty_power_on();
    printf("[bluetooth tty] open pid=%d boot=%d error=%d\n",proc_pid(p),boot,error);
    if(error) {
        lck_mtx_lock(&bt_tty_lock);bt_tty.open=false;lck_mtx_unlock(&bt_tty_lock);
        return error==EWOULDBLOCK?EIO:error;
    }
    bt_kick();
    return 0;
}
static int bt_tty_close(dev_t dev __unused,int flags __unused,int devtype __unused,struct proc *p __unused)
{
    lck_mtx_lock(&bt_tty_lock);
    bt_tty.closing=true;
    bt_tty_wake_readers();bt_tty_wake_writers();
    printf("[bluetooth tty] close rx_dropped=%llu\n",bt_tty.rx_dropped);
    bt_tty.open=bt_tty.closing=false;
    bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    lck_mtx_unlock(&bt_tty_lock);
    bt_kick();
    return 0;
}
/* Called with the lock held. 0 while I/O may go on, EAGAIN while the controller starts. */
static int bt_tty_io_status(void)
{
    if(!bt_tty.open || bt_tty.closing) return ENXIO;
    return bt_tty.boot<0?EAGAIN:bt_tty.boot?EIO:0;
}
static int bt_tty_read(dev_t dev __unused,struct uio *uio,int ioflag)
{
    uint8_t chunk[512];
    int error=0;
    lck_mtx_lock(&bt_tty_lock);
    while(!bt_tty.rx_len) {
        error=bt_tty_io_status();
        if(error && error!=EAGAIN) break;
        if(ioflag&IO_NDELAY){error=EAGAIN;break;}
        error=msleep(&bt_tty.rx_len,&bt_tty_lock,PZERO|PCATCH,"btread",NULL);
        if(error) break;
    }
    if(error){lck_mtx_unlock(&bt_tty_lock);return error;}
    bool taken=false;
    while(bt_tty.rx_len && uio_resid(uio)>0) {
        uint32_t n=(uint32_t)MIN((user_ssize_t)MIN(bt_tty.rx_len,sizeof(chunk)),uio_resid(uio));
        bt_tty_fifo_get(bt_tty.rx,&bt_tty.rx_head,&bt_tty.rx_len,chunk,n);
        taken=true;
        lck_mtx_unlock(&bt_tty_lock);
        error=uiomove((const char *)chunk,(int)n,uio);
        lck_mtx_lock(&bt_tty_lock);
        if(error) break;
    }
    lck_mtx_unlock(&bt_tty_lock);
    /* Room in the FIFO: the owner offers what it held back. */
    if(taken) bt_kick();
    return error;
}
static int bt_tty_write(dev_t dev __unused,struct uio *uio,int ioflag)
{
    uint8_t chunk[512];
    int error=0;
    while(uio_resid(uio)>0 && !error) {
        uint32_t n=(uint32_t)MIN((user_ssize_t)sizeof(chunk),uio_resid(uio));
        lck_mtx_lock(&bt_tty_lock);
        /* Whole chunks only, so a failed wait leaves the client's data untouched. */
        while(!(error=bt_tty_io_status()) && n>BT_TTY_FIFO-bt_tty.tx_len) {
            if(ioflag&IO_NDELAY){error=EAGAIN;break;}
            error=msleep(&bt_tty.tx_len,&bt_tty_lock,PZERO|PCATCH,"btwrite",NULL);
            if(error) break;
        }
        lck_mtx_unlock(&bt_tty_lock);
        if(error) break;
        error=uiomove((char *)chunk,(int)n,uio);
        if(error) break;
        lck_mtx_lock(&bt_tty_lock);
        error=bt_tty_io_status();
        /* This device has one writer (the daemon's HCI thread); another writer that took the room is reported, not interleaved. */
        if(!error && n>BT_TTY_FIFO-bt_tty.tx_len) error=EIO;
        if(!error) bt_tty_fifo_put(bt_tty.tx,bt_tty.tx_head,&bt_tty.tx_len,chunk,n);
        lck_mtx_unlock(&bt_tty_lock);
        if(!error) bt_kick();
    }
    return error;
}
static int bt_tty_select(dev_t dev __unused,int which,void *wql,struct proc *p)
{
    int ready=0;
    lck_mtx_lock(&bt_tty_lock);
    int status=bt_tty_io_status();
    if(which==FREAD) {
        if(bt_tty.rx_len || (status && status!=EAGAIN)) ready=1;
        else {bt_tty.rsel_used=true;selrecord(p,&bt_tty.rsel,wql);}
    } else if(which==FWRITE) {
        if(bt_tty.tx_len<BT_TTY_FIFO || (status && status!=EAGAIN)) ready=1;
        else {bt_tty.wsel_used=true;selrecord(p,&bt_tty.wsel,wql);}
    }
    lck_mtx_unlock(&bt_tty_lock);
    return ready;
}
static int bt_tty_ioctl(dev_t dev __unused,u_long cmd,caddr_t data,int fflag __unused,struct proc *p __unused)
{
    int error=0;
    lck_mtx_lock(&bt_tty_lock);
    switch((uint32_t)cmd) {
    case BT_TIOCGETA:
        memcpy(data,&bt_tty.termios,sizeof(bt_tty.termios));break;
    case BT_TIOCSETA: case BT_TIOCSETAW: case BT_TIOCSETAF:
        /* Recorded for the next TIOCGETA. The line is fixed at 8N1 with RTS/CTS at the speed of the bring-up. */
        memcpy(&bt_tty.termios,data,sizeof(bt_tty.termios));break;
    case BT_IOSSIOSPEED: {
        uint64_t speed;memcpy(&speed,data,sizeof(speed));
        bt_tty.termios.ispeed=bt_tty.termios.ospeed=speed;
        bt_tty_count(3);
        printf("[bluetooth tty] speed request %llu, line %u\n",speed,bt_tty.speed);
        break;
    }
    case BT_TIOCFLUSH: {
        int what;memcpy(&what,data,sizeof(what));
        if(!what || (what&FREAD)) bt_tty.rx_head=bt_tty.rx_len=0;
        break;
    }
    case BT_TIOCOUTQ: {int n=(int)bt_tty.tx_len;memcpy(data,&n,sizeof(n));break;}
    case BT_FIONREAD: {int n=(int)bt_tty.rx_len;memcpy(data,&n,sizeof(n));break;}
    case BT_TIOCMGET: {int lines=0x002|0x004|0x020|0x040|0x100;memcpy(data,&lines,sizeof(lines));break;}  /* DTR RTS CTS CAR DSR */
    case BT_TIOCDRAIN: {
        struct timespec timeout={1,0};
        while(bt_tty.tx_len && !error && !bt_tty_io_status())error=msleep(&bt_tty.tx_len,&bt_tty_lock,PZERO|PCATCH,"btdrain",&timeout);
        if(error==EWOULDBLOCK) error=0;
        break;
    }
    case BT_TIOCEXCL: case BT_TIOCNXCL: case BT_TIOCMSET: case BT_TIOCMBIS: case BT_TIOCMBIC: case BT_TIOCSDTR: case BT_TIOCCDTR:
    case BT_FIONBIO: case BT_FIOASYNC:
        break;
    default:
        error=ENOTTY;
    }
    lck_mtx_unlock(&bt_tty_lock);
    return error;
}
static const struct cdevsw bt_tty_cdevsw={
    .d_open=bt_tty_open,.d_close=bt_tty_close,.d_read=bt_tty_read,.d_write=bt_tty_write,.d_ioctl=bt_tty_ioctl,
    .d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=bt_tty_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,
    .d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0,
};
/* devfs takes nodes once it is ready; the board's worker can start before that. */
static bool bt_tty_make_node(void)
{
    lck_mtx_lock(&bt_tty_lock);
    if(bt_tty.major<0) {
        bt_tty.major=cdevsw_add(-1,&bt_tty_cdevsw);
        if(bt_tty.major>=0)(void)cdevsw_setkqueueok(bt_tty.major,&bt_tty_cdevsw,0);
    }
    if(bt_tty.major>=0 && !bt_tty.node)
        bt_tty.node=devfs_make_node(makedev(bt_tty.major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,BT_TTY_NAME);
    bool made=bt_tty.node!=NULL;
    lck_mtx_unlock(&bt_tty_lock);
    return made;
}
static void bt_tty_node_thread(void *context __unused,wait_result_t result __unused)
{
    unsigned tries=0;
    while(!bt_tty_make_node() && ++tries<600) {
        struct timespec pause={0,100*1000*1000};
        (void)msleep(&bt_tty.node_thread,NULL,PZERO,"btnode",&pause);
    }
    printf("[bluetooth tty] /dev/" BT_TTY_NAME " %s after %u waits\n",bt_tty.node?"made":"not made",tries);
    thread_terminate(current_thread());
}

static int bt_tty_boot_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int error=0,value;
    bt_tty_count(1);
    lck_mtx_lock(&bt_tty_lock);
    struct timespec timeout={30,0};
    while(bt_tty.boot<0 && !error) error=msleep(&bt_tty.boot,&bt_tty_lock,PZERO|PCATCH,"btboot",&timeout);
    value=bt_tty.boot;
    lck_mtx_unlock(&bt_tty_lock);
    return error?error:SYSCTL_OUT(req,&value,sizeof(value));
}
SYSCTL_PROC(_kern,OID_AUTO,qbt_boot,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,bt_tty_boot_sysctl,"I","WCN7850 kernel bring-up result");
SYSCTL_STRING(_kern,OID_AUTO,qbt_firmware,CTLFLAG_RD|CTLFLAG_LOCKED,bt_tty.firmware,sizeof(bt_tty.firmware),"WCN7850 firmware build label");

/* A sandboxed client cannot write sysctls, so reading this value requests startup and waits for the worker's result. */
static int bt_tty_power_on_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp,arg1,arg2)
    int result=0;
    if(req->newptr)return EPERM;
    if(!req->oldptr)return SYSCTL_OUT(req,&result,sizeof(result));
    bt_tty_count(0);
    result=bt_tty_power_on();
    return SYSCTL_OUT(req,&result,sizeof(result));
}
SYSCTL_PROC(_kern,OID_AUTO,qbt_power_on,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED,NULL,0,bt_tty_power_on_sysctl,"I","WCN7850 startup request");

int qbt_transport_init(const struct qbt_transport_ops *ops)
{
    if(!ops || !ops->wake || !ops->power)return EINVAL;
    lck_mtx_lock(&bt_tty_lock);
    if(bt_ops.wake){lck_mtx_unlock(&bt_tty_lock);return EALREADY;}
    bt_ops=*ops;
    bool start=!bt_tty.node_thread;bt_tty.node_thread=true;
    lck_mtx_unlock(&bt_tty_lock);
    if(start && !bt_tty_make_node()) {
        thread_t thread=THREAD_NULL;
        if(kernel_thread_start(bt_tty_node_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    }
    return 0;
}

/* Called by the UART owner after each power transition: 0 after a complete bring-up (with the UART speed it configured),
 * else errno. */
void qbt_transport_power_result(bool enabled,int error,const char *firmware,uint32_t speed)
{
    (void)bt_tty_make_node();
    lck_mtx_lock(&bt_tty_lock);
    if(firmware) strlcpy(bt_tty.firmware,firmware,sizeof(bt_tty.firmware));
    if(enabled && !error){bt_tty.speed=speed;if(!bt_tty.open)bt_tty_termios_default();}
    bt_tty.boot=error?error:enabled?0:ENETDOWN;
    if(!enabled || error)bt_tty.rx_head=bt_tty.rx_len=bt_tty.tx_head=bt_tty.tx_len=0;
    bt_tty.power_pending=false;bt_tty.power_result=error;
    printf("[bluetooth tty] power=%u error=%d firmware=%s speed=%u\n",enabled,error,firmware?firmware:"",speed);
    wakeup(&bt_tty.boot);
    wakeup(&bt_tty.power_pending);
    /* Blocked clients observe the new state on their next pass. */
    bt_tty_wake_readers();bt_tty_wake_writers();
    lck_mtx_unlock(&bt_tty_lock);
    bt_kick();
}
void qbt_transport_boot_result(int error,const char *firmware,uint32_t speed)
{
    qbt_transport_power_result(true,error,firmware,speed);
}
