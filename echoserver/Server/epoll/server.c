#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define MAXCONN 4096

int
setnonblocking(int fd)
{
    int opt;

    opt = fcntl(fd, F_GETFL, 0);
    if(opt < 0)
    {
        fprintf(stderr,"get fd error:%s\n",strerror(errno));
        return -1;
    }
    opt |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, opt) < 0)
    {
        fprintf(stderr,"set fd error:%s\n",strerror(errno));
        return -1;
    }
    return 0;
}

int
do_use_fd(int fd)
{
    int n;
    char buff[1024];

    bzero(buff, sizeof(buff));
    n = read(fd, buff, 1024);
    if(n == -1)
    {
        if(errno != EAGAIN)
        {
            fprintf(stderr,"read error:%s\n",strerror(errno));
            close(fd);
            return 1;
        }
    }
    else if(n == 0)
        close(fd);
    else
        write(fd, buff, n);
    return 0;
}

int
main()
{
    struct sockaddr_in addr;
    int listenfd;
    int epollfd;
    int nfds;
    int i;
    struct epoll_event pev[MAXCONN], ev;

    bzero(pev, sizeof(pev));
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0)
    {
        fprintf(stderr,"create socket error:%s\n",strerror(errno));
        return -1;
    }
    if(setnonblocking(listenfd)!=0)
    {
        fprintf(stderr,"set nonblocking error\n");
        return -1;
    }
    if(bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        fprintf(stderr,"bind error:%s\n",strerror(errno));
        return -1;
    }
    if(listen(listenfd, 100) == -1)
    {
        fprintf(stderr,"listen error:%s\n",strerror(errno));
        return -1;
    }

    epollfd = epoll_create(MAXCONN);
    if(epollfd == -1)
    {
        fprintf(stderr,"epoll create error:%s\n",strerror(errno));
        return -1;
    }
    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) < 0)
    {
        fprintf(stderr,"epoll ctl error:%s\n",strerror(errno));
        return -1;
    }

    while(1)
    {
        nfds = epoll_wait(epollfd, pev, MAXCONN, -1);
        for(i=0; i < nfds; i++)
        {
            if(pev[i].data.fd == listenfd)
            {
                int connd;
                struct sockaddr_in remoteAddr;
                socklen_t remoteAddrLen;

                remoteAddrLen = sizeof(remoteAddr);
                connd = accept(listenfd, (struct sockaddr*)&remoteAddr, &remoteAddrLen);
                if(connd == -1)
                {
                    if(errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
                        continue;
                    fprintf(stderr,"accept error:%s\n",strerror(errno));
                    return -1;
                }
                if(setnonblocking(connd)!=0)
                {
                    fprintf(stderr,"set nonblocking error\n");
                    return -1;
                }
                ev.data.fd = connd;
                ev.events = EPOLLIN | EPOLLET;
                if(epoll_ctl(epollfd, EPOLL_CTL_ADD, connd, &ev)==-1)
                {
                    fprintf(stderr,"epoll control error:%s\n",strerror(errno));
                    return -1;
                }
            }
            else if(pev[i].events & (EPOLLHUP | EPOLLERR))
                close(pev[i].data.fd);
            else if(pev[i].events & EPOLLIN)
                do_use_fd(pev[i].data.fd);
            else
                fprintf(stdout,"%d  %d\n",pev[i].data.fd,pev[i].events);
        }
    }

    return 0;
}


