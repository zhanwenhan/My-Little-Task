#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define OPEN_MAX 4096
/*
int setnonblocking(int fd)
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

int do_use_fd(int fd)
{
	int n;
	char buff[1024];
	bzero(buff,sizeof(buff));
	n = read(fd, buff, 1024);
	if(n == -1)
	{
		if(errno == EAGAIN)
		{
			fprintf(stderr,"read error:%s\n",strerror(errno));
			return 1;
		}
	}
	else if(n == 0)
		close(fd);
	else
		write(fd, buff, n);
	return 0;
}
*/

int
main()
{
	struct sockaddr_in servaddr;
	struct sockaddr_in cliaddr;
	socklen_t clilen;
	ssize_t n;
	int listenfd;
	int nready;
	int i;
	int maxi;
	int connfd, sockfd;
	struct pollfd client[OPEN_MAX];
	char buf[2048];

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(9999);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if(listenfd < 0)
	{
		fprintf(stderr,"create socket error:%s\n",strerror(errno));
		return -1;
	}
	if(bind(listenfd, (struct sockaddr * )&servaddr, sizeof(servaddr)) == -1)
	{
		fprintf(stderr,"bind error:%s\n",strerror(errno));
		return -1;
	}
	if(listen(listenfd, 100) == -1)
	{
		fprintf(stderr,"listen error:%s\n",strerror(errno));
		return -1;
	}

	client[0].fd = listenfd;
	client[0].events = POLLIN ;

	for(i=1;i<OPEN_MAX;i++)
		client[i].fd = -1;
	maxi = 0;

	for(;;)
	{
		if((nready = poll(client, maxi+1, -1)) <= 0)
		{
			printf("\n  Poll error \n");
			exit(1);
		}
		if(client[0].revents & POLLIN)
		{
			clilen = sizeof(cliaddr);
			connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
			if(connfd <= 0)
			{
				printf("\n Aceept error \n");
				exit(1);
			}
			for(i=1; i < OPEN_MAX; i++)
			{
				if(client[i].fd < 0)
				{
					client[i].fd = connfd;
					break;
				}
			}
			if(i == OPEN_MAX)
			{
				fprintf(stderr, "too many client:\n");
				return -1;
			}
			client[i].events = POLLIN;
			if(i > maxi)
			  maxi = i;
			if(--nready <= 0)
			  continue;
		}
		for(i=1; i <= maxi; i++)
		{
			if((sockfd = client[i].fd) < 0)
			  continue;
			if(client[i].revents & (POLLIN | POLLERR))
			{
				if((n = read(sockfd, buf, 2048)) < 0)
				{
					if(errno == ECONNRESET)
					{
						close(sockfd);
						client[i].fd = -1;
					}
					else
					{
						fprintf(stderr,"read error:\n");
						return -1; 
					}
				}
				else if(n == 0)
				{
					close(sockfd);
					client[i].fd=-1;
				}
				else
				  write(sockfd, buf, n);
				if(--nready <= 0)
				  break;;
			}
		}
	}

	return 0;
}
















