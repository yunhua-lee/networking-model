#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/mman.h>
#define KEY 0x1 /* key for first message queue */
#define LISTENQ 10
#define MAXLINE 20
#define MAXN 16384
#define handle_error(msg) \
          do { perror(msg); exit(EXIT_FAILURE); } while (0)

typedef struct {
    long mtype;
    char mtext[MAXLINE];
} Mymsg;

static int read_cnt;
static char *read_ptr;
static char read_buf[MAXLINE];
char flag;



void web_child(int sockfd);
ssize_t readline(int fd, void *vptr, size_t maxlen);
static ssize_t my_read(int fd, char *ptr) ;
ssize_t writen(int fd, const void *vptr, size_t n);
void pr_cpu_time(void);
pid_t child_make(int, long *, int listenfd);
void child_main(int,long *, int listenfd);

int main(int argc, char **argv){
    int listenfd, connfd,ident,flags;
    socklen_t clilen;
    int nchildren;
    in_port_t port;
    pid_t *pids;
    long * ptr;
    Mymsg msg;
    int i=0;
    struct sockaddr_in cliaddr, servaddr;
    if (argc != 4)
        errx(1,"tcp_fork_server <addr> <port> <childnum>\n");

    nchildren = atoi(argv[3]);
    if((pids = calloc(nchildren, sizeof(pid_t))) == NULL)
        handle_error("calloc");

    port = atoi(argv[2]);
    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        handle_error("socket");
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) == -1)
       handle_error("inet_pton");
    //This anonymously mapped shared memory can only be used for in-process communication with a certain relationship
    if((ptr = mmap(0, nchildren * sizeof(long), PROT_READ | PROT_WRITE,MAP_ANON | MAP_SHARED, -1, 0)) == MAP_FAILED)
       handle_error("mmap");
    if((flags = fcntl(listenfd, F_GETFL, 0)) == -1)
         handle_error("fcntl");
    else
        if(fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) == -1)
                handle_error("fcntl");

    if(bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) == -1)
        handle_error("bind");
    if(listen(listenfd, LISTENQ) == -1)
        handle_error("listen");

    for (i = 0; i < nchildren; i++)
        pids[i] = child_make(i,ptr,listenfd); /* parent returns */

    for(;;){
        //Compared with the logic of the previous article, here we need to put the judgment of message IPC here
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
                for (i = 0; i < nchildren; i++)
                    kill(pids[i], SIGTERM);

                while (wait(NULL) > 0);
                    if (errno != ECHILD)
                        errx(1,"wait error");
                pr_cpu_time();
                for(i =0;i<nchildren;i++)
                    printf("child %d connected number:%d\n",i,ptr[i]);
                msg.mtype=2;
                memcpy(msg.mtext,"done",5);
                if (msgsnd(ident,&msg,MAXLINE,0) == -1 )
                    handle_error("msgrcv");
                return 0;
            }
    }
}


void pr_cpu_time(void){
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



pid_t child_make(int i, long * ptr, int listenfd){
    pid_t pid;
    if ((pid = fork()) <0)
        handle_error("fork");
    else if(pid > 0)
        return (pid); /* parent */
    else
        child_main(i, ptr,listenfd); /* never returns */
}


void child_main(int i, long * ptr, int listenfd){
    int connfd;
    socklen_t clilen;
    struct sockaddr *cliaddr;
    if((cliaddr = malloc(sizeof(struct sockaddr_in))) == NULL)
        handle_error("malloc");
    printf("child %ld starting\n", (long) getpid());
    for ( ; ; ) {

        clilen = sizeof(struct sockaddr_in);
        if((connfd = accept(listenfd, (struct sockaddr *) &cliaddr, &clilen)) == -1 )
            if( errno == EAGAIN)
                continue;
            else
                handle_error("accept");
        //In the shared storage area, each client corresponds to a long storage area
        ptr[i]++;
        web_child(connfd); /* process the request */
        if(close(connfd) == -1)
            handle_error("close"); /* parent closes connected socket */
    }
}

void web_child(int sockfd){
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


static ssize_t my_read(int fd, char *ptr){
    if (read_cnt <= 0) {
        again:
            if ( (read_cnt = read(fd, read_buf, sizeof(read_buf))) < 0) {
                if (errno == EINTR)
                    goto again;
                return (-1);
            } else if (read_cnt == 0)
                return (0);
            read_ptr = read_buf;
    }
    read_cnt--;
    *ptr = *read_ptr++;
    return (1);
}

ssize_t readline(int fd, void *vptr, size_t maxlen){
    ssize_t n, rc;
    char c, *ptr;
    ptr = vptr;
    for (n = 1; n < maxlen; n++) {
        if ( (rc = my_read(fd, &c)) == 1) {
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

