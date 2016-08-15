#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

int
main(int argc , char * argv[])
{
    int sockfd,new_fd;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    int sin_size,portnumber;
    char msg[1024];

    if(argc!=2)
    {
		fprintf(stderr,"Usage:%s portnumber\n",argv[0]);
		exit(1);
    }
    if((portnumber=atoi(argv[1]))<0)
    {
		fprintf(stderr,"Usage:%s portnumber\n",argv[0]);
		exit(1);
    }
    if((sockfd=socket(AF_INET,SOCK_STREAM,0))==-1)
    {
		fprintf(stderr,"Socket error:%s\n",strerror(errno));
		exit(1);
    }
    bzero(&server_addr,sizeof(struct sockaddr_in));
    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=htonl(INADDR_ANY);
    server_addr.sin_port=htons(portnumber);

    if(bind(sockfd,(struct sockaddr *)(&server_addr),sizeof(struct sockaddr))==-1)
    {
		fprintf(stderr,"Bind error:%s\n",strerror(errno));
		exit(1);
    }
    if(listen(sockfd,5)==-1)
    {
		fprintf(stderr,"Listen error:%s\n",strerror(errno));
		exit(1);
    }

    while(1)
    {
		int nbytes=0;
		sin_size=sizeof(struct sockaddr_in);
		if((new_fd=accept(sockfd,(struct sockaddr *)(&client_addr),&sin_size))==-1)
		{
		    fprintf(stderr,"Accept error:%s\n",strerror(errno));
			exit(1);
		}
		bzero(&msg,sizeof(msg));
		if((nbytes=read(new_fd,msg,1024))==-1)
		{
		    fprintf(stderr,"Read Error:%s\n",strerror(errno));
		    exit(1);
		}
		msg[nbytes]='\0';
		if(write(new_fd,msg,strlen(msg))==-1)
		{
		    fprintf(stderr,"Write Error:%s\n",strerror(errno));
		    exit(1);
		}
		close(new_fd);
	}
    close(sockfd);

    return 0;
}














