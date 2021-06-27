#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <sys/resource.h>
#define LISTENQ 10
#define KEY 0x1 /* key for first message queue */
#define SERV_PORT 9877
#define MAXLINE 20
#define NTHREAD 5
#define MAXN 16384
#define handle_error(msg) \
          do { perror(msg); exit(EXIT_FAILURE); } while (0)
#define handle_error_en(en, msg) \
      do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct {
    int  rl_cnt; /* initialize to 0 */
    char *rl_bufptr; /* initialize to rl_buf */
    char rl_buf[MAXLINE];
} Rline;
typedef struct {
    long mtype;
    char mtext[MAXLINE];
} Mymsg;

void web_child(int sockfd);
void str_echo(int sockfd);
ssize_t writen(int fd, const void *vptr, size_t n);
ssize_t readline(int fd, void *vptr, size_t maxlen);
static ssize_t my_read(Rline *tsd,int fd, char *ptr);
void * doit(void *arg);
static void readline_once(void);
static void readline_destructor(void *ptr);
void pr_cpu_time(void);
pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t rl_key;
static pthread_once_t rl_once = PTHREAD_ONCE_INIT;
char flag;
int listenfd;

int main(int argc, char **argv)
{
    int ident,error,on=1;
    int threadnum;
    in_port_t port;
    pthread_t tid;
    long * ptr;
    Mymsg msg;
    int i=0;
    struct sockaddr_in servaddr;
    if (argc != 4)
        errx(1,"tcp_prethread_ser <addr> <port> <threadnum>\n");

    threadnum = atoi(argv[3]);
    if((ptr = calloc(threadnum, sizeof(long))) == NULL)
        handle_error("calloc");
    port = atoi(argv[2]);
    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        handle_error("socket");
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) == -1)
        handle_error("inet_pton");
    if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
        handle_error("setsockopt");
    if(bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) == -1)
        handle_error("bind");
    if(listen(listenfd, LISTENQ) == -1)
        handle_error("listen");


    for(i=0;i<threadnum;i++){
        if((error=pthread_create(&tid, NULL,doit, ptr+i)) != 0)
            handle_error_en(error,"pthread_create");
        printf("Thread fo client:%d\n",tid);
    }

    for ( ; ; ){
      if(!flag){
          if((ident=msgget(KEY,0660)) == -1 )
              continue;
          flag=1;
      }
      if(flag)
          //Every time it is judged whether the client sends a message to the server
          if (msgrcv(ident,&msg,MAXLINE,1,IPC_NOWAIT) ==  -1){
              if(errno != ENOMSG)
                  handle_error("msgrcv");
          }
          else{


          pr_cpu_time();
              for(i =0;i<threadnum;i++)
                  printf("thread %d connected number:%d\n",i,ptr[i]);
              msg.mtype=2;
              memcpy(msg.mtext,"done",5);
              if (msgsnd(ident,&msg,MAXLINE,0) == -1 )
                  handle_error("msgrcv");
              return 0;
          }
    }
}


void pr_cpu_time(void)
{
    double user, sys;
    struct rusage myusage, childusage;
    if (getrusage(RUSAGE_SELF, &myusage) < 0)
        handle_error("getrusage error");
    if (getrusage(RUSAGE_CHILDREN, &childusage) < 0)
        handle_error("getrusage error");
    user = (double) myusage.ru_utime.tv_sec +myusage.ru_utime.tv_usec / 1000000.0;
    user += (double) childusage.ru_utime.tv_sec +childusage.ru_utime.tv_usec / 1000000.0;
    sys = (double) myusage.ru_stime.tv_sec + myusage.ru_stime.tv_usec / 1000000.0;
    sys += (double) childusage.ru_stime.tv_sec + childusage.ru_stime.tv_usec / 1000000.0;
    printf("\nuser time = %g, sys time = %g\n", user, sys);

}

void * doit(void *arg){
    int error,connfd;
    socklen_t clilen;
    long * ptr=arg;
    if((error = pthread_detach(pthread_self())) != 0){
        handle_error_en(error,"pthread_detach");
    }
    for(;;){
        if((error = pthread_mutex_lock(&mlock)) != 0)
            handle_error_en(error,"pthread_mutex_lock");

        if((connfd = accept(listenfd,NULL,NULL)) == -1 )
            handle_error("accept");
        if((error = pthread_mutex_unlock(&mlock)) != 0)
            handle_error_en(error,"pthread_mutex_lock");

        (*ptr)++;
        web_child(connfd); /* same function as before */
        if(close(connfd) == -1)
            handle_error("close"); /* done with connected socket */
   }
   return NULL;
}

void web_child(int sockfd)
{
    int ntowrite;
    ssize_t nread;
    char line[MAXLINE], result[MAXN];
    for ( ; ; ) {

        if((nread=readline(sockfd, line, MAXLINE)) == -1)
            handle_error("readline");
        else if(nread == 0)
            return ;
        ntowrite = atol(line);
        if ((ntowrite <= 0) || (ntowrite > MAXN))
            errx(1,"client request for %d bytes,max size is %d\n", ntowrite,MAXN);
        if(writen(sockfd, result, ntowrite) == -1)
            handle_error("writen");
    }
}


ssize_t writen(int fd, const void *vptr, size_t n){
    size_t nleft;
    ssize_t nwritten;
    const char *ptr;
    ptr = vptr;
    nleft = n;
    while (nleft > 0){
        if ( (nwritten = write(fd, ptr, nleft)) <= 0){
            if (nwritten < 0 && errno == EINTR)
                nwritten = 0; /* and call write() again */
            else
                return (-1); /* error */
        }
        nleft -= nwritten;
        ptr += nwritten;
    }
    return (n);
}

static ssize_t my_read(Rline *tsd,int fd, char *ptr){
    if(tsd->rl_cnt <= 0) {
        again:
            if ((tsd->rl_cnt = read(fd, tsd->rl_buf, sizeof(tsd->rl_buf))) < 0) {
                if (errno == EINTR)
                    goto again;
                return (-1);
            } else if (tsd->rl_cnt == 0)
                return (0);
            tsd->rl_bufptr = tsd->rl_buf;
    }
    tsd->rl_cnt--;
    *ptr = *tsd->rl_bufptr++;
    return (1);
}

ssize_t readline(int fd, void *vptr, size_t maxlen){
    ssize_t n, rc;
    char c, *ptr;
    ptr = vptr;
    Rline *tsd;
    int error;

    if((error = pthread_once(&rl_once, readline_once)) != 0)
        handle_error_en(error,"pthread_once");
    if ( (tsd = pthread_getspecific(rl_key)) == NULL) {
        if((tsd = calloc(1, sizeof(Rline))) == NULL)
            handle_error("calloc");
        if((error = pthread_setspecific(rl_key, tsd)) != 0)
            handle_error_en(error,"pthread_setspecific");
    }
    for (n = 1; n < maxlen; n++) {
        if ( (rc = my_read(tsd,fd, &c)) == 1) {
            *ptr++ = c;
            if (c == '\n')
                break; /* newline is stored, like fgets() */
        } else if (rc == 0) {
            *ptr = 0;
            return (n - 1); /* EOF, n - 1 bytes were read */
        } else
            return (-1); /* error, errno set by read() */
    }
    *ptr = 0; /* null terminate like fgets() */
    return (n);
}

static void readline_destructor(void *ptr){
    free(ptr);
}

static void readline_once(void){
    int error;
    if((error = pthread_key_create(&rl_key, readline_destructor)) != 0)
         handle_error_en(error,"pthread_key_create");
}
