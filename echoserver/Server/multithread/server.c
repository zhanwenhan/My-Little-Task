#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <pthread.h>

#define SERV_PORT 9999
#define MAXLINE 1024

void
str_echo(int sockfd)
{
    ssize_t n, w;
    char buf[MAXLINE];

again:
    while((n = read(sockfd, buf, MAXLINE)) > 0)
        if((w = write(sockfd, buf, n)) != n)
            return ;
    if(n < 0 && errno == EINTR)
        goto again;
    else if(n < 0)
    {
        perror("server read error");
        return ;
    }
}

void *
disposer(void * arg)
{
    int temp;

    if(pthread_detach(pthread_self()) != 0)
    {
        perror("pthread_detach error");
        exit(1);
    }

    temp = *(int *)arg;
    free(arg);

    str_echo(temp);
    close(temp);

    return NULL;
}

int
main(int argc, char ** argv)
{
    int connfd, listenfd;
    int * temp = NULL;
    pthread_t tid;
    socklen_t clilen;
    struct sockaddr_in cliaddr, servaddr;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0)
    {
        perror("socket error");
        exit(1);
    }

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);
    if(bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        perror("bind error");
        exit(1);
    }
    if(listen(listenfd, 100) < 0)
    {
        perror("listen error");
        exit(1);
    }

    for(;;)
    {
        clilen = sizeof(cliaddr);
        connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen);
        if(connfd < 0 && errno == EINTR)
            continue;
        else if(connfd < 0)
        {
            perror("accept error");
            exit(1);
        }

        if((temp = (int *)malloc(sizeof(int))) != NULL)
            *temp = connfd;

        if(pthread_create(&tid, NULL,  disposer, temp) != 0)
        {
            perror("can't create thread");
            exit(1);
        }
    }
    close(listenfd);
    exit(0);
}



