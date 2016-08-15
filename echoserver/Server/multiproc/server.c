#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

typedef void Sigfunc(int);

Sigfunc *
signal(int signo , Sigfunc * func)
{
    struct sigaction act, oact;

    act.sa_handler=func;
    sigemptyset(&act.sa_mask);
    act.sa_flags=0;
    if(signo==SIGALRM)
    {
#ifdef	SA_INTERRUPT
        act.sa_flags|=SA_INTERRUPT;
#endif
    }
    else
    {
#ifdef  SA_RESTART
        act.sa_flags|=SA_RESTART;
#endif
    }
    if(sigaction(signo,&act,&oact)<0)
        return(SIG_ERR);
    return(oact.sa_handler);
}

void
sig_chld(int signo)
{
    pid_t	pid;
    int		stat;
    while((pid=waitpid(-1,&stat,WNOHANG))>0);
    return;
}

void
Doit(int sockfd)
{
    ssize_t n;
    char	buf[1024];
again:
    while((n=read(sockfd,buf,sizeof(buf)))>0)
        write(sockfd,buf,n);
    if(n<0&&errno==EINTR)
        goto again;
    else if(n<0)
    {
        fprintf(stderr,"read:%s\n",strerror(errno));
        exit(0);
    }
}

int
main(int argc , char ** argv)
{
    int 				listenfd,connfd;
    pid_t 				childpid;
    socklen_t			clilen;
    struct sockaddr_in 	cliaddr,servaddr;

    if((listenfd=socket(AF_INET,SOCK_STREAM,0))==-1)
    {
        fprintf(stderr,"socket:\n");
        exit(1);
    }

    bzero(&servaddr,sizeof(struct sockaddr_in));
    servaddr.sin_family=AF_INET;
    servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
    servaddr.sin_port=htons(9999);

    if(bind(listenfd,(struct sockaddr *)&servaddr,sizeof(servaddr))==-1)
    {
        fprintf(stderr,"bind:\n");
        exit(1);
    }

    if(listen(listenfd,5)==-1)
    {
        fprintf(stderr,"listen:\n");
        exit(1);
    }

    signal(SIGCHLD,sig_chld);

    while(1)
    {
        clilen=sizeof(cliaddr);
        if((connfd=accept(listenfd,(struct sockaddr *)&cliaddr,&clilen))==-1)
        {
            if(errno==EINTR)
                continue;
            else
            {
                fprintf(stderr,"accept:\n");
                exit(1);
            }
        }
        childpid=fork();
        if(childpid==0)
        {
            close(listenfd);
            Doit(connfd);
            exit(0);
        }
        close(connfd);
    }

    return 0;
}



