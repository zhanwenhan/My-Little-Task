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
#include <time.h>

#define DEBUGC

#define MAXCONN 4096

struct cli
{
	int fd;
};

struct cli client[MAXCONN];

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

int main(int argc, char * argv[])
{
	int i;
	int num;
	int port;
	int epollfd;
	int nready, nwriten;
	int nread;
	char buf[2048];
	struct sockaddr_in servaddr;
	struct epoll_event pev[MAXCONN], ev;

#ifdef DEBUGC

	int x = 0;
	double count = 0.0;
	struct timeval tv, tv1;

#endif

	if(argc != 4)
	{
		fprintf(stderr, "Usage:scli IPaddr Port Number\n");
		return -1;
	}

	if((num = atoi(argv[3])) < 0)
	{
		fprintf(stderr, "Usage:scli IPaddr Port Number\n");
		return -1;
	}

	if((port = atoi(argv[2]))<0)
	{
		fprintf(stderr,"Usage:scli IPaddr Port Number\n");
		return -1;
	}

	for(i=0; i < num; i++)
		if((client[i].fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		{
			fprintf(stderr, "Create socket failed\n");
			return -1;
		}

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons((short)port);
	if(inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0)
	{
		fprintf(stderr, "IP Address error\n");
		return -1;
	}

	for(i=0; i < num; i++)
	{
		if(connect(client[i].fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
		{
			fprintf(stderr, "Connect failed:%s\n", strerror(errno));
			return -1;
		}

		if(setnonblocking(client[i].fd) != 0)
		{
			fprintf(stderr, "set nonblocking error\n");
			return -1;
		}

		fprintf(stdout, "%d\n",i);
	}

	epollfd = epoll_create(num);
	if(epollfd == -1)
	{
		fprintf(stderr,"epoll create error:%s\n",strerror(errno));
		return -1;
	}

	for(i=0; i < num; i++)
	{
		ev.data.fd = client[i].fd;
		ev.events = EPOLLOUT;
		if(epoll_ctl(epollfd, EPOLL_CTL_ADD, client[i].fd, &ev) < 0)
		{
			fprintf(stderr, "epoll ctl error:%s\n", strerror(errno));
			return -1;
		}
	}

#ifdef DEBUGC

	gettimeofday(&tv, NULL);

#endif

	while(1)
	{
		nready = epoll_wait(epollfd, pev, MAXCONN, -1);
		for(i=0; i<nready; i++)
		{
			if(pev[i].events & EPOLLIN)
			{
				nread = read(pev[i].data.fd, buf, sizeof(buf));
				buf[nread] = '\0';
				if(nread < 0)
				{
					if(errno != EWOULDBLOCK)
					{
						fprintf(stderr, "Read failed:%s\n", strerror(errno));
						return -1;
					}
				}
				if(strcmp(buf, "asdfashflkjasdhfljka") != 0)
					fprintf(stderr, "WRONG!!!!!%s\n", strerror(errno));
				ev.data.fd = pev[i].data.fd;
				ev.events = EPOLLOUT;
				if(epoll_ctl(epollfd, EPOLL_CTL_MOD, pev[i].data.fd, &ev)==-1)
				{
					fprintf(stderr, "epoll control error:%s\n", strerror(errno));
					return -1;
				}

#ifdef DEBUGC

				count = count + 1.0;

#endif

			}
			else if(pev[i].events & EPOLLOUT)
			{
				nwriten = write(pev[i].data.fd, "asdfashflkjasdhfljka", strlen("asdfashflkjasdhfljka") + 1);
				if(nwriten < 0)
				{
					if(errno != EWOULDBLOCK)
					{
						fprintf(stderr, "Write failed:%s\n", strerror(errno));
						return -1;
					}
				}
				if(nwriten > 0)
				{
					ev.data.fd = pev[i].data.fd;
					ev.events = EPOLLIN;
					if(epoll_ctl(epollfd, EPOLL_CTL_MOD, pev[i].data.fd, &ev)==-1)
					{
						fprintf(stderr, "epoll control error:%s\n", strerror(errno));
						return -1;
					}
				}
			}
		}

#ifdef DEBUGC

        if(++x >= 100)
		{
			gettimeofday(&tv1, NULL);
			printf("%lf\n", count * 1000000 / ((tv1.tv_sec * 1000000 + tv1.tv_usec) - (tv.tv_sec * 1000000 + tv.tv_usec)));
			x = 0;
		}

#endif	

	}

	return 0;
}

















