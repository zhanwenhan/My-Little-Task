#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <signal.h>
#include <aio.h>

#define SERVPORT 9999
#define SIG_AIO_RECEIVE SIGRTMIN+1
#define SIG_AIO_SEND SIGRTMIN+2

void
aio_signal_receive_handler(int signo, siginfo_t * info, void * context)
{
	int ret;
	struct aiocb * req;

	if(info->si_signo == SIG_AIO_RECEIVE)
	{
		req = (struct aiocb *)info->si_value.sival_ptr;
		if(aio_error(req) == 0)
		{
			ret = aio_return(req);
			if(ret == 0)
			{
				close(req->aio_fildes);
				free(req->aio_buf);
				free(req);
				return;
			}
			req->aio_nbytes = ret;
			req->aio_offset = 0;
			req->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
			req->aio_sigevent.sigev_signo = SIG_AIO_SEND;
			req->aio_sigevent.sigev_value.sival_ptr = req;
			aio_write(req);
		}
	}
}

void
aio_signal_send_handler(int signo, siginfo_t * info, void * context)
{
	int ret;
	struct aiocb * req;

	if(info->si_signo == SIG_AIO_SEND)
	{
		req = (struct aiocb *)info->si_value.sival_ptr;
		if(aio_error(req) == 0)
		{
			ret = aio_return(req);
			if(ret == 0)
			{
				close(req->aio_fildes);
				free(req->aio_buf);
				free(req);
				return;
			}
//			((char *)req->aio_buf)[ret] = 0;
//			printf("%s\n", req->aio_buf);
			req->aio_nbytes = 1024;
			req->aio_offset = 0;
			req->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
			req->aio_sigevent.sigev_signo = SIG_AIO_RECEIVE;
			req->aio_sigevent.sigev_value.sival_ptr = req;
			aio_read(req);
		}
	}
}

int
main(int argc, char ** argv)
{
	struct sockaddr_in servaddr, cliaddr;
	int listenfd, connfd;
	int clilen;
	struct sigaction receiveact, sendact;
	struct aiocb * paiocb;

	bzero(&receiveact, sizeof(receiveact));
	sigemptyset(&receiveact.sa_mask);
	receiveact.sa_flags = SA_SIGINFO | SA_RESTART;
	receiveact.sa_sigaction = aio_signal_receive_handler;
	sigaction(SIG_AIO_RECEIVE, &receiveact, NULL);

	bzero(&sendact, sizeof(sendact));
	sigemptyset(&sendact.sa_mask);
	sendact.sa_flags = SA_SIGINFO | SA_RESTART;
	sendact.sa_sigaction = aio_signal_send_handler;
	sigaction(SIG_AIO_SEND, &sendact, NULL);

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(SERVPORT);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	//setnonblocking(listenfd);
	bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr));
	listen(listenfd, 100);

	while(1)
	{
		clilen = sizeof(cliaddr);
		if((connfd=accept(listenfd, (struct sockaddr *)&cliaddr, &clilen))==-1)
		{
			if(errno==EINTR)
				continue;
			else
			{
				fprintf(stderr,"accept:\n");
				exit(1);
			}
		}
		paiocb = (struct aiocb *)malloc(sizeof(struct aiocb));
		bzero(paiocb, sizeof(struct aiocb));
		paiocb->aio_fildes = connfd;
		paiocb->aio_buf = malloc(1024);
		paiocb->aio_nbytes = 1024;
		paiocb->aio_offset = 0;
		paiocb->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
		paiocb->aio_sigevent.sigev_signo = SIG_AIO_RECEIVE;
		paiocb->aio_sigevent.sigev_value.sival_ptr = paiocb;
		aio_read(paiocb);
	}
	return 0;
}


