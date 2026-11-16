from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/debug-auth';out.mkdir(parents=True,exist_ok=True)
source=repo/'package/debug27/MarbleDebug.c'
text='''#define MARBLE_DEBUG_AUTH "fixture-key"
#define main marble_service_main
#include "'''+str(source)+'''"
#undef main
#include <assert.h>
#include <pthread.h>
int marble_copy_unified_log(void **bytes,size_t *size){*bytes=strdup("fixture log\\n");*size=12;return 0;}
static int registrations;
int marble_register_webkit_extensions(void){registrations++;return 0;}
struct Case{int fd,local;};
static void *run(void *p){struct Case *c=p;serve(c->fd,c->local);close(c->fd);return NULL;}
static void check(const char *request,int local,int status){int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));struct Case c={fds[1],local};pthread_t t;assert(!pthread_create(&t,NULL,run,&c));assert(send_all(fds[0],request,strlen(request))==0);char reply[4096];size_t used=0;ssize_t n;while((n=read(fds[0],reply+used,sizeof(reply)-used-1))>0)used+=n;reply[used]=0;char wanted[32];snprintf(wanted,sizeof wanted,"HTTP/1.0 %d",status);assert(!strncmp(reply,wanted,strlen(wanted)));close(fds[0]);pthread_join(t,NULL);}
int main(){
check("GET /unified.log HTTP/1.0\\r\\n\\r\\n",0,403);
check("GET /unified.log HTTP/1.0\\r\\n\\r\\n",1,403);
check("GET /unified.log HTTP/1.0\\r\\nAuthorization: Bearer wrong\\r\\n\\r\\n",0,403);
check("GET /unified.log HTTP/1.0\\r\\nAuthorization: Bearer fixture-key\\r\\n\\r\\n",0,200);
check("GET /status HTTP/1.0\\r\\n\\r\\n",0,403);
check("GET /status HTTP/1.0\\r\\n\\r\\n",1,200);
check("GET /webkit-status.json HTTP/1.0\\r\\n\\r\\n",0,403);
check("GET /webkit-status.json HTTP/1.0\\r\\n\\r\\n",1,403);
check("POST /register-webkit HTTP/1.0\\r\\nAuthorization: Bearer fixture-key\\r\\n\\r\\n",0,404);
assert(registrations==0);const char *arguments[]={"marble-debug","--register-webkit"};assert(marble_service_main(2,arguments)==0&&registrations==1);
puts("PASS: authenticated read-only log/status routes; registration requires explicit local CLI mode; bounded server route fixtures");}
'''
(out/'auth.c').write_text(text);subprocess.run(['clang','-std=c11','-pthread','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(out/'auth.c'),'-o',str(out/'auth')],check=True);subprocess.run([str(out/'auth')],check=True)
