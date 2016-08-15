//gcc -o multicore_transcode_server MCTC_Server.c zwhlib/zwhwraplib/filelib/zwh_file.o zwhlib/zwhwraplib/netlib/zwh_ipv4.o zwhlib/zwhwraplib/normallib/zwh_normal.o zwhlib/zwhwraplib/processlib/zwh_process.o zwhlib/zwhwraplib/threadlib/zwh_thread.o -lpthread -lavformat -lavcodec -lswscale

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/epoll.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "zwhlib/zwhwraplib/filelib/zwh_file.h"
#include "zwhlib/zwhwraplib/netlib/zwh_ipv4.h"
#include "zwhlib/zwhwraplib/normallib/zwh_normal.h"
#include "zwhlib/zwhwraplib/threadlib/zwh_thread.h"
#include "zwhlib/zwhwraplib/processlib/zwh_process.h"

#define MAXCONN 8192
#define SERVPORT 21
#define THREADNUM 3

#define LOCALUSER "mv"
#define LOCALPASS "mv"

#define STORSERVADDR "192.168.1.47"
#define STORSERVUSER "mv"
#define STORSERVPASS "mv"

/////////////////////////////////////////////////
//our ftp server support 8 commands below   本服务器需要实现的FTP 的8个命令

#define  USER 1   //用户名
#define  PASS 2   //密码
#define  SYST 3   //系统类型
#define  TYPE 4   //传输类型,ASCII形式或binary形式
#define  PORT 5  //服务器主动连接时客户端通报自己IP和PORT的命令
#define  STOR 6  //存储命令
#define  PASV 7  //服务器被动连接的命令,收到此命令后,服务器发送自己的ip port给客户端
#define  QUIT 8  //退出

/////////////////////////////////////////////////
//客户端状态机

#define  INIT       0
#define  USERSEND   1   //用户名
#define  PASSSEND   2   //密码
#define  SYSTSEND   3   //系统类型
#define  TYPESEND   4   //传输类型,ASCII形式或binary形式
#define  PORTSEND   5  //服务器主动连接时客户端通报自己IP和PORT的命令
#define  STORSEND   6  //存储命令
#define  QUITSEND   7  //退出
#define  QUITOK     8


/////////////////////////////////////////////////
//conv type   用于确定会话是正在接收文件准备转码，还是转码完毕正在发送

#define GETTYPE 0
#define PUTTYPE 1


/////////////////////////////////////////////////
//sock type   套接口种类

#define CTRLTYPE 1
#define DATATYPE 2
#define LISTENTYPE 3


/////////////////////////////////////////////////
//log state   登陆状态

#define  UNLOG 1        //未登录
#define  LOGGING 2      //正在登陆,即输入了用户名
#define  LOGIN 3        //登陆成功

//passive or active   数据连接,主动连接还是被动连接
#define  NOPAM 0   //未设置
#define  UPASM 1   //主动连接,即服务器连接客户端
#define  PASVM 2   //被动连接,客户端连接服务器


/////////////////////////////////////////////////
//listen accept and store cmd state  这是一个状态记录,用于保证被动连接时监听套接口已接收连接且收到了stor命令

#define  ALLDONOT 0  //两者都没有
#define  LISTENACCEPT 1  //已经接收连接

#define  STORCMD 2  //已经得到stor 命令
#define  SENDCMD 4

/////////////////////////////////////////////////
//已经接收完毕的音视频文件，由该结构组成任务队列，转码线程取该队列进行转码操作。
typedef struct tagwork
{
	char filename[64];
	struct tagwork * next;
}work;


/////////////////////////////////////////////////
//主体数据结构，用于进行FTP会话管理
typedef struct tagconv conv;

typedef struct tagconvsock
{
	int type;
	conv * pconv;
}convsock;

struct tagconv
{
	int type;  //用于区别该结构是用于收还是用于发
	
	int ctrlfd;
	int listenfd;   //the data connect listen fd
	int datafd;   //the data connect fd
	convsock * pctrlcs;
	convsock * pdatacs;
	convsock * plistencs;

	int filefd;
	char filename[64];

	int convoptstate;

	char databuf[10240000];

/////////////////////////////////////////////////

	int putstate; 

	char * dataptr;
	ssize_t nlefttosend;
	
	char respond[1024];
	char resreadbuf[1024];
	char * resreadnptr;
	char * ressearchptr;   //get the 0d0a pointer
	char * respondptr;   //the pointer point to the readbuf the first respond
	ssize_t resreadn;   //read n char

/////////////////////////////////////////////////

	int logstate;//the login state
	int connstate;//the connect state active or passive
	int cmd;//the command to do
	
	char user[64];//username
	char pass[64];//password
	char peeraddr[64];//client ip addr
	int  peerport;//client port
	
	char readbuf[1024];//read buffer
	char * readnptr;
	char * searchptr;//get the 0d0a pointer
	char * cmdptr;//the pointer point to the readbuf the first cmd
	char cmdbuf[1024];//command buffer
	ssize_t readn;//read n char

/////////////////////////////////////////////////

	struct tagconv * pre;
	struct tagconv * next;
};

/////////////////////////////////////////////////

work * getqueueptr = NULL;
pthread_mutex_t getqueueptr_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t getqueueptr_cond = PTHREAD_COND_INITIALIZER;

work * putqueueptr = NULL;
pthread_mutex_t putqueueptr_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t avlib_mutex = PTHREAD_MUTEX_INITIALIZER;

int epollfd = 0;
conv * handleconv = NULL;

/////////////////////////////////////////////////

int
writen(int fd, const char * str, int size)   //一次保证写入N和字节
{
	int nleft;
	int nwriten;
	const char * ptr;
	
	ptr = str;
	nleft = size;
	while(nleft > 0)
	{
		if((nwriten = write(fd, ptr, nleft)) <= 0)
		{
			if(nwriten < 0 && errno == EINTR)
				nwriten = 0;
			else
				return (-1);
		}
		nleft -= nwriten;
		ptr += nwriten;
	}
	
	return (size);
}

/////////////////////////////////////////////////

typedef struct tag_SampleList
{
	int16_t * ptr;
	int16_t * sample;
	int lenth;
	struct tag_SampleList * next;
} SampleList;

typedef struct tag_SampleQueue
{
	SampleList * first_sample;
	SampleList * last_sample;
	int whole_size;
} SampleQueue;

typedef struct tag_FrameList
{
	AVFrame * frame;
	struct tag_FrameList * next;
} FrameList;

typedef struct tag_FrameQueue
{
	FrameList * first_frame;
	FrameList * last_frame;
	int nb_frames;
} FrameQueue;

void
sample_queue_init(SampleQueue * q)
{
	q->first_sample = NULL;
	q->last_sample = NULL;
	q->whole_size = 0;
}

int
sample_queue_put(SampleQueue * q, int16_t * sample, int lenth)  //lenth in bytes;
{
	SampleList * put_sample;
	
	put_sample = av_malloc(sizeof(SampleList));
	if(!put_sample)
		return -1;
	put_sample->ptr = sample;
	put_sample->sample = sample;
	put_sample->lenth = lenth;
	put_sample->next = NULL;
  
	if (!q->last_sample)
		q->first_sample = put_sample;
	else
		q->last_sample->next = put_sample;
	q->last_sample = put_sample;
	put_sample = NULL;
	q->whole_size += lenth;

	return 0;
}

void
sample_put(SampleQueue * q, int16_t * sample, int lenth)
{
	if(lenth <= q->first_sample->lenth)
	{
		memcpy((char *)sample, (char *)q->first_sample->sample, lenth);
		q->first_sample->sample += lenth / 2;
		q->first_sample->lenth -= lenth;
		q->whole_size -= lenth;
		if(!q->first_sample->lenth)
		{
			SampleList * free_sample;

			free_sample = q->first_sample;
			q->first_sample = free_sample->next;
			if(!q->first_sample)
				q->last_sample = q->first_sample;
			free_sample->next = NULL;
			av_free(free_sample->ptr);
			av_free(free_sample);
		}
	}
	else
	{
		SampleList * free_sample;
		
		memcpy((char *)sample, (char *)q->first_sample->sample, q->first_sample->lenth);
		sample += q->first_sample->lenth / 2;
		lenth -= q->first_sample->lenth;
		
		q->whole_size -= q->first_sample->lenth;
		free_sample = q->first_sample;
		q->first_sample = free_sample->next;
		if(!q->first_sample)
			q->last_sample = q->first_sample;
		free_sample->next = NULL;
		av_free(free_sample->ptr);
		av_free(free_sample);
		sample_put(q, sample, lenth);
	}
}

int
sample_queue_get(SampleQueue * q, int16_t ** psample, int lenth)   //lenth in bytes;
{	
	if(q->whole_size < lenth)
		return -1;
	*psample = av_malloc(lenth);
	if(!*psample)
		return -1;
	sample_put(q, *psample, lenth);
	
	return 0;
}


void
frame_queue_init(FrameQueue * q)
{
	q->first_frame = NULL;
	q->last_frame = NULL;
	q->nb_frames = 0;
}

int
frame_queue_put(FrameQueue * q, AVFrame * frame)
{
	FrameList * put_frame;
	
	put_frame = av_malloc(sizeof(FrameList));
	if(!put_frame)
		return -1;
	put_frame->frame = frame;
	put_frame->next = NULL;
  
	if (!q->last_frame)
		q->first_frame = put_frame;
	else
		q->last_frame->next = put_frame;
	q->last_frame = put_frame;
	put_frame = NULL;
	q->nb_frames++;

	return 0;
}

int
frame_queue_get(FrameQueue * q, AVFrame ** pframe)
{
	FrameList * get_frame;

	get_frame = q->first_frame;
	if(!get_frame)
		return -1;
	q->first_frame = get_frame->next;
	if(!q->first_frame)
		q->last_frame = q->first_frame;
	q->nb_frames--;
	
	get_frame->next = NULL;
	*pframe = get_frame->frame;
	get_frame->frame = NULL;
	av_free(get_frame);

	return 0;
}

/////////////////////////

typedef struct tag_Encodedata
{
	AVFrame * picture;
	uint8_t * video_outbuf;
	int video_outbuf_size;

	int16_t * samples;
	int audio_input_frame_size;
	uint8_t * audio_outbuf;
	int audio_outbuf_size;
} Encodedata;

typedef struct tag_Transcode
{
	char in_filename[256], out_filename[256];
	int width, height;

	SampleQueue sq;
	FrameQueue fq;
	
	struct SwsContext * swscale_ctx;
	
	AVFormatContext * in_fc;
	int in_asti, in_vsti;

	AVFormatContext * out_fc;
	int out_asti, out_vsti;

	Encodedata data;
} Transcode;

void
pstrcpy(char * buf, int buf_size, const char * str)
{
    char c;
    char * q = buf;

    if(buf_size <= 0)
        return;
    for(;;)
    {   
        c = *str++;
        if (c == '\0' || q >= buf + buf_size - 1)
            break;
        *q++ = c;
    }   
    *q = '\0';
}

void
init_transcode(Transcode * tc, char * in_filename, char * out_filename, int width, int height)
{
	bzero(tc, sizeof(Transcode));
	strcpy(tc->in_filename, in_filename);
	strcpy(tc->out_filename, out_filename);
	tc->width = width;
	tc->height = height;
	tc->in_vsti = -1;
	tc->in_asti = -1;
	tc->out_vsti = -1;
	tc->out_asti = -1;
	sample_queue_init(&tc->sq);
	frame_queue_init(&tc->fq);
}

int
init_decode(Transcode * tc)
{
	int i = 0;
	AVCodec * codec = NULL;

	if(av_open_input_file(&tc->in_fc, tc->in_filename, NULL, 0, NULL))
	{
		writen(STDERR_FILENO, "av_open_input_file error.\n",
				strlen("av_open_input_file error.\n"));
		return -1;
	}
	if(av_find_stream_info(tc->in_fc) < 0)
	{
		writen(STDERR_FILENO, "av_find_stream_info error.\n",
				strlen("av_find_stream_info error.\n"));
		return -1;
	}
	dump_format(tc->in_fc, 0, tc->in_filename, 0);    //debug

	for(i = 0; i < tc->in_fc->nb_streams; i++)
	{
		if(tc->in_fc->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO && tc->in_vsti < 0)
			tc->in_vsti = i;
		if(tc->in_fc->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO && tc->in_asti < 0)
			tc->in_asti = i;
	}
	if(tc->in_vsti == -1 && tc->in_asti == -1)
	{
		writen(STDERR_FILENO, "can not find audio and video stream.\n",
				strlen("can not find audio and video stream.\n"));
		return -1;
	}
	if(tc->in_vsti == -1)
		writen(STDERR_FILENO, "can not find video stream.\n",
				strlen("can not find video stream.\n"));
	if(tc->in_asti == -1)
		writen(STDERR_FILENO, "can not find audio stream.\n",
				strlen("can not find audio stream.\n"));

	if(tc->in_asti != -1)
	{
		codec = avcodec_find_decoder(tc->in_fc->streams[tc->in_asti]->codec->codec_id);
		if(!codec)
		{
			writen(STDERR_FILENO, "can not find audio decoder.\n",
					strlen("can not find audio decoder.\n"));
			return -1;
		}
		Pthread_mutex_lock(&avlib_mutex);
		if(avcodec_open(tc->in_fc->streams[tc->in_asti]->codec, codec))
		{
			Pthread_mutex_unlock(&avlib_mutex);
			writen(STDERR_FILENO, "open audio decoder error.\n",
					strlen("open audio decoder error.\n"));
			return -1;
		}
		Pthread_mutex_unlock(&avlib_mutex);
	}

	if(tc->in_vsti != -1)
	{
		codec = avcodec_find_decoder(tc->in_fc->streams[tc->in_vsti]->codec->codec_id);
		if(!codec)
		{
			writen(STDERR_FILENO, "can not find video decoder.\n",
					strlen("can not find video decoder.\n"));
			return -1;
		}
		Pthread_mutex_lock(&avlib_mutex);
		if(avcodec_open(tc->in_fc->streams[tc->in_vsti]->codec, codec))
		{
			Pthread_mutex_unlock(&avlib_mutex);
			writen(STDERR_FILENO, "open video decoder error.\n",
					strlen("open video decoder error.\n"));
			return -1;
		}
		Pthread_mutex_unlock(&avlib_mutex);
	}

	return 0;
}

int
init_encode(Transcode * tc)
{
	AVOutputFormat * ofmt = NULL;

	ofmt = av_guess_format(NULL, tc->out_filename, NULL);
	if(!ofmt)
	{
		writen(STDERR_FILENO, "can not deduce output format from file extension.\n",
				strlen("can not deduce output format from file extension.\n"));
		return -1;
	}
	tc->out_fc = avformat_alloc_context();
	if(!tc->out_fc)
	{
		writen(STDERR_FILENO, "avformat_alloc_context error.\n",
				strlen("avformat_alloc_context error.\n"));
		return -1;
	}
	tc->out_fc->oformat = ofmt;

	pstrcpy(tc->out_fc->filename, sizeof(tc->out_fc->filename), tc->out_filename);
	
#if LIBAVFORMAT_VERSION_INT < (53<<16)

	pstrcpy(tc->out_fc->title, sizeof(tc->out_fc->title), tc->in_fc->title);
	pstrcpy(tc->out_fc->author, sizeof(tc->out_fc->author), tc->in_fc->author);
	pstrcpy(tc->out_fc->copyright, sizeof(tc->out_fc->copyright), tc->in_fc->copyright);
	pstrcpy(tc->out_fc->comment, sizeof(tc->out_fc->comment), tc->in_fc->comment);
	pstrcpy(tc->out_fc->album, sizeof(tc->out_fc->album), tc->in_fc->album);
	pstrcpy(tc->out_fc->genre, sizeof(tc->out_fc->genre), tc->in_fc->genre);
	tc->out_fc->year = tc->in_fc->year;
	tc->out_fc->track = tc->in_fc->track;
	
#endif

	if(tc->out_fc->oformat->video_codec != CODEC_ID_NONE && tc->in_vsti != -1)
	{
		AVStream * st;

		st = av_new_stream(tc->out_fc, 0);
		if(!st)
		{
			writen(STDERR_FILENO, "av_new_stream error.\n",
					strlen("av_new_stream error.\n"));
			return -1;
		}
		tc->out_vsti = st->index;

		tc->out_fc->streams[tc->out_vsti]->codec->codec_id = tc->out_fc->oformat->video_codec;
		tc->out_fc->streams[tc->out_vsti]->codec->codec_type = CODEC_TYPE_VIDEO;
		
		tc->out_fc->streams[tc->out_vsti]->codec->bit_rate = 
			tc->in_fc->streams[tc->in_vsti]->codec->bit_rate ? tc->in_fc->streams[tc->in_vsti]->codec->bit_rate : 1000000;
		tc->out_fc->streams[tc->out_vsti]->codec->width = tc->width ? tc->width : tc->in_fc->streams[tc->in_vsti]->codec->width;
		tc->out_fc->streams[tc->out_vsti]->codec->height = tc->height ? tc->height : tc->in_fc->streams[tc->in_vsti]->codec->height;
		tc->out_fc->streams[tc->out_vsti]->codec->time_base.den = tc->in_fc->streams[tc->in_vsti]->time_base.den;
		tc->out_fc->streams[tc->out_vsti]->codec->time_base.num = tc->in_fc->streams[tc->in_vsti]->time_base.num;
		tc->out_fc->streams[tc->out_vsti]->time_base.den = tc->in_fc->streams[tc->in_vsti]->time_base.den;
		tc->out_fc->streams[tc->out_vsti]->time_base.num = tc->in_fc->streams[tc->in_vsti]->time_base.num;
		tc->out_fc->streams[tc->out_vsti]->codec->gop_size = tc->in_fc->streams[tc->in_vsti]->codec->gop_size;
		tc->out_fc->streams[tc->out_vsti]->codec->pix_fmt = PIX_FMT_YUV420P;
		if(tc->out_fc->streams[tc->out_vsti]->codec->codec_id == CODEC_ID_MPEG2VIDEO)
			tc->out_fc->streams[tc->out_vsti]->codec->max_b_frames = 2;
		if(tc->out_fc->streams[tc->out_vsti]->codec->codec_id == CODEC_ID_MPEG1VIDEO)
			tc->out_fc->streams[tc->out_vsti]->codec->mb_decision=2;
		if(tc->out_fc->oformat->flags & AVFMT_GLOBALHEADER)
			tc->out_fc->streams[tc->out_vsti]->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	if(tc->out_fc->oformat->audio_codec != CODEC_ID_NONE && tc->in_asti != -1)
	{
		AVStream * st;

		st = av_new_stream(tc->out_fc, 1);
		if(!st)
		{
			writen(STDERR_FILENO, "av_new_stream error.\n",
					strlen("av_new_stream error.\n"));
			return -1;
		}
		tc->out_asti = st->index;

		tc->out_fc->streams[tc->out_asti]->codec->codec_id = tc->out_fc->oformat->audio_codec;
		tc->out_fc->streams[tc->out_asti]->codec->codec_type = CODEC_TYPE_AUDIO;

		tc->out_fc->streams[tc->out_asti]->codec->bit_rate = 
			tc->in_fc->streams[tc->in_asti]->codec->bit_rate ? tc->in_fc->streams[tc->in_asti]->codec->bit_rate : 64000;
		tc->out_fc->streams[tc->out_asti]->codec->sample_rate = tc->in_fc->streams[tc->in_asti]->codec->sample_rate;
		tc->out_fc->streams[tc->out_asti]->codec->channels = tc->in_fc->streams[tc->in_asti]->codec->channels;
		if(tc->out_fc->oformat->flags & AVFMT_GLOBALHEADER)
			tc->out_fc->streams[tc->out_asti]->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	}

	if(tc->out_fc->nb_streams <= 0)
	{
		writen(STDERR_FILENO, "no output stream.\n",
				strlen("no output stream.\n"));
		return -1;
	}

	if(av_set_parameters(tc->out_fc, NULL) < 0)
	{
		writen(STDERR_FILENO, "av_set_parameters error.\n",
				strlen("av_set_parameters error.\n"));
		return -1;
	}
	dump_format(tc->out_fc, 0, tc->out_filename, 1);   //debug

	if(tc->out_vsti != -1)
	{
		AVCodec * codec;

		tc->data.video_outbuf_size = 400000;
		tc->data.video_outbuf = av_malloc(tc->data.video_outbuf_size);
		if(!tc->data.video_outbuf)
		{
			writen(STDERR_FILENO, "av_malloc error.\n",
					strlen("av_malloc error.\n"));
			return -1;
		}
		tc->data.picture = NULL;

		codec = avcodec_find_encoder(tc->out_fc->streams[tc->out_vsti]->codec->codec_id);
		if(!codec)
		{
			writen(STDERR_FILENO, "can not find video encoder.\n",
					strlen("can not find video encoder.\n"));
			return -1;
		}
		Pthread_mutex_lock(&avlib_mutex);
		if(avcodec_open(tc->out_fc->streams[tc->out_vsti]->codec, codec) < 0)
		{
			Pthread_mutex_unlock(&avlib_mutex);
			writen(STDERR_FILENO, "open video encoder error.\n",
					strlen("open video encoder error.\n"));
			return -1;
		}
		Pthread_mutex_unlock(&avlib_mutex);
	}

	if(tc->out_asti != -1)
	{
		AVCodec * codec;

		tc->data.audio_outbuf_size = 100000;
		tc->data.audio_outbuf = av_malloc(tc->data.audio_outbuf_size);
		if(!tc->data.audio_outbuf)
		{
			writen(STDERR_FILENO, "av_malloc error.\n",
					strlen("av_malloc error.\n"));
			return -1;
		}

		codec = avcodec_find_encoder(tc->out_fc->streams[tc->out_asti]->codec->codec_id);
		if(!codec)
		{
			writen(STDERR_FILENO, "can not find audio encoder.\n",
					strlen("can not find audio encoder.\n"));
			return -1;
		}
		Pthread_mutex_lock(&avlib_mutex);
		if(avcodec_open(tc->out_fc->streams[tc->out_asti]->codec, codec) < 0)
		{
			Pthread_mutex_unlock(&avlib_mutex);
			writen(STDERR_FILENO, "open audio encoder error.\n",
					strlen("open audio encoder error.\n"));
			return -1;
		}
		Pthread_mutex_unlock(&avlib_mutex);

		tc->data.samples = NULL;
		if(tc->out_fc->streams[tc->out_asti]->codec->frame_size <= 1)
		{
			tc->data.audio_input_frame_size = tc->data.audio_outbuf_size / tc->out_fc->streams[tc->out_asti]->codec->channels;
			switch(tc->out_fc->streams[tc->out_asti]->codec->codec_id)
			{
			case CODEC_ID_PCM_S16LE:
			case CODEC_ID_PCM_S16BE:
			case CODEC_ID_PCM_U16LE:
			case CODEC_ID_PCM_U16BE:
				tc->data.audio_input_frame_size >>= 1;
				break;
			default:
				break;
			}
		}
		else
			tc->data.audio_input_frame_size = tc->out_fc->streams[tc->out_asti]->codec->frame_size;
	}

	return 0;
}

int
init_swscale(Transcode * tc)
{
	if(tc->in_vsti != -1 && tc->out_vsti != -1)
	{
		tc->swscale_ctx = sws_getContext(
											tc->in_fc->streams[tc->in_vsti]->codec->width,
											tc->in_fc->streams[tc->in_vsti]->codec->height,
											tc->in_fc->streams[tc->in_vsti]->codec->pix_fmt,
											tc->out_fc->streams[tc->out_vsti]->codec->width,
											tc->out_fc->streams[tc->out_vsti]->codec->height,
											tc->out_fc->streams[tc->out_vsti]->codec->pix_fmt,
											SWS_BICUBIC, NULL, NULL, NULL
										);
		if(!tc->swscale_ctx)
		{
			writen(STDERR_FILENO, "sws_getContext error.\n",
					strlen("sws_getContext error.\n"));
			return -1;
		}
	}

	return 0;
}

AVFrame *
alloc_picture(enum PixelFormat pix_fmt, int width, int height)
{
	AVFrame * picture;
	uint8_t * picture_buf;
	int size;

	picture = avcodec_alloc_frame();
	if(!picture)
		return NULL;
	size = avpicture_get_size(pix_fmt, width, height);
	picture_buf = av_malloc(size);
	if(!picture_buf)
	{
		av_free(picture);
		return NULL;
	}
	avpicture_fill((AVPicture *)picture, picture_buf, pix_fmt, width, height);

	return picture;
}

int
transcoding(Transcode * tc)
{
	int decodefinish;
	double audio_pts, video_pts;

	if(!(tc->out_fc->oformat->flags & AVFMT_NOFILE))
		if(url_fopen(&tc->out_fc->pb, tc->out_filename, URL_WRONLY) < 0)
		{
			writen(STDERR_FILENO, "can not open output file.\n",
					strlen("can not open output file.\n"));
			return -1;
		}

	if(av_write_header(tc->out_fc))
	{
		writen(STDERR_FILENO, "av_write_header error.\n",
				strlen("av_write_header error.\n"));
		return -1;
	}
///////////////////////////////////////////////////////////////////////

	decodefinish = 0;
	tc->data.picture = avcodec_alloc_frame();

	while(1)
	{
		if(!decodefinish)
		{
			AVPacket pkt;
			
			if(av_read_frame(tc->in_fc, &pkt) >= 0)
			{				
				if(pkt.stream_index == tc->in_vsti && tc->out_vsti != -1)
				{
					int len;
					int frameFinished = 0;
					AVFrame * link_picture;

					len = avcodec_decode_video2(tc->in_fc->streams[tc->in_vsti]->codec, 
													tc->data.picture, &frameFinished, &pkt);
					if(len < 0)
					{
						writen(STDERR_FILENO, "avcodec_decode_video2 error.\n",
								strlen("avcodec_decode_video2 error.\n"));
						return -1;
					}
					if(frameFinished)
					{
						link_picture = alloc_picture(tc->out_fc->streams[tc->out_vsti]->codec->pix_fmt,
														tc->out_fc->streams[tc->out_vsti]->codec->width,
														tc->out_fc->streams[tc->out_vsti]->codec->height);
						sws_scale(tc->swscale_ctx, tc->data.picture->data, tc->data.picture->linesize, 0,
									tc->in_fc->streams[tc->in_vsti]->codec->height,
									link_picture->data, link_picture->linesize);
						link_picture->pts = pkt.pts;   //////////////////////////////// bug
						if(frame_queue_put(&tc->fq, link_picture))
						{
							writen(STDERR_FILENO, "frame_queue_put error.\n",
									strlen("frame_queue_put error.\n"));
							return -1;
						}
					}
				}
				if(pkt.stream_index == tc->in_asti && tc->out_asti != -1)
				{
					int len;
					int16_t * link_sample;
					int link_sample_size;
					
					int temp_size = pkt.size;
					uint8_t * temp_data = pkt.data;
					
					while(temp_size > 0)
					{
						link_sample_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
						link_sample = av_malloc(link_sample_size);
						len = avcodec_decode_audio2(tc->in_fc->streams[tc->in_asti]->codec, link_sample,
														&link_sample_size, temp_data, temp_size);
						if(len < 0)
						{
							writen(STDERR_FILENO, "avcodec_decode_audio2 error.\n",
									strlen("avcodec_decode_audio2 error.\n"));
							return -1;
						}
						if(link_sample_size > 0)
							if(sample_queue_put(&tc->sq, link_sample, link_sample_size))
							{
								writen(STDERR_FILENO, "sample_queue_put error.\n",
										strlen("sample_queue_put error.\n"));
								return -1;
							}
						temp_size -= len;
						temp_data += len;
					}
				}
				av_free_packet(&pkt); 
			}
			else
				decodefinish = 1;
		}

		if(decodefinish && 
			((tc->out_vsti != -1 && tc->fq.nb_frames < 1) || 
			(tc->out_asti != -1 && tc->sq.whole_size < tc->data.audio_input_frame_size * 2 * tc->out_fc->streams[tc->out_asti]->codec->channels)))
			break;
		if((tc->out_vsti != -1 && tc->fq.nb_frames < 1) || 
			(tc->out_asti != -1 && tc->sq.whole_size < tc->data.audio_input_frame_size * 2 * tc->out_fc->streams[tc->out_asti]->codec->channels))
			continue;

		if(tc->out_asti != -1)
			audio_pts = (double)tc->out_fc->streams[tc->out_asti]->pts.val * 
						tc->out_fc->streams[tc->out_asti]->time_base.num / tc->out_fc->streams[tc->out_asti]->time_base.den;
		else
			audio_pts = 0.0;

		if(tc->out_vsti != -1)
			video_pts = (double)tc->out_fc->streams[tc->out_vsti]->pts.val * 
						tc->out_fc->streams[tc->out_vsti]->time_base.num / tc->out_fc->streams[tc->out_vsti]->time_base.den;
		else
			video_pts = 0.0;

		if(tc->out_vsti == -1 || (tc->out_vsti != -1 && tc->out_asti != -1 && audio_pts < video_pts))
		{
			AVPacket pkt;

			av_init_packet(&pkt);
			if(sample_queue_get(&tc->sq, &tc->data.samples, 
								tc->data.audio_input_frame_size * 2 * tc->out_fc->streams[tc->out_asti]->codec->channels))
			{
				writen(STDERR_FILENO, "sample_queue_get error.\n",
						strlen("sample_queue_get error.\n"));
				return -1;
			}
			pkt.size = avcodec_encode_audio(tc->out_fc->streams[tc->out_asti]->codec,
												tc->data.audio_outbuf, tc->data.audio_outbuf_size, tc->data.samples);
			if(pkt.size < 0)
			{
				writen(STDERR_FILENO, "avcodec_encode_audio error.\n",
						strlen("avcodec_encode_audio error.\n"));
				return -1;
			}
			if(pkt.size > 0)
			{
				if(tc->out_fc->streams[tc->out_asti]->codec->coded_frame && 
					tc->out_fc->streams[tc->out_asti]->codec->coded_frame->pts != AV_NOPTS_VALUE)
					pkt.pts = av_rescale_q(tc->out_fc->streams[tc->out_asti]->codec->coded_frame->pts,
											tc->out_fc->streams[tc->out_asti]->codec->time_base, 
											tc->out_fc->streams[tc->out_asti]->time_base);
				pkt.flags |= AV_PKT_FLAG_KEY;
				pkt.stream_index = tc->out_asti;
				pkt.data = tc->data.audio_outbuf;
				if(av_interleaved_write_frame(tc->out_fc, &pkt))
				{
					writen(STDERR_FILENO, "writing audio frame error.\n",
							strlen("writing audio frame error.\n"));
					return -1;
				}
			}
			av_free(tc->data.samples);
		}
		else
		{
			AVPacket pkt;
			AVFrame * temp_frame;

			av_init_packet(&pkt);
			if(frame_queue_get(&tc->fq, &temp_frame))
			{
				writen(STDERR_FILENO, "frame_queue_get error.\n",
						strlen("frame_queue_get error.\n"));
				return -1;
			}
			pkt.size = avcodec_encode_video(tc->out_fc->streams[tc->out_vsti]->codec,
												tc->data.video_outbuf, tc->data.video_outbuf_size, temp_frame);
			if(pkt.size < 0)
			{
				writen(STDERR_FILENO, "avcodec_encode_video error.\n",
						strlen("avcodec_encode_video error.\n"));
				return -1;
			}
			if(pkt.size > 0)
			{
				if(tc->out_fc->streams[tc->out_vsti]->codec->coded_frame->pts != AV_NOPTS_VALUE)
					pkt.pts = av_rescale_q(tc->out_fc->streams[tc->out_vsti]->codec->coded_frame->pts,
											tc->out_fc->streams[tc->out_vsti]->codec->time_base,
											tc->out_fc->streams[tc->out_vsti]->time_base);
				if(tc->out_fc->streams[tc->out_vsti]->codec->coded_frame->key_frame)
					pkt.flags |= AV_PKT_FLAG_KEY;
				pkt.stream_index = tc->out_vsti;
				pkt.data = tc->data.video_outbuf;
				if(av_interleaved_write_frame(tc->out_fc, &pkt))
				{
					writen(STDERR_FILENO, "writing video frame error.\n",
							strlen("writing video frame error.\n"));
					return -1;
				}
			}
			av_free(temp_frame->data[0]);
			av_free(temp_frame);
		}
	}

///////////////////////////////////////////////////////////////////////
	if(av_write_trailer(tc->out_fc))
	{
		writen(STDERR_FILENO, "av_write_trailer error.\n",
				strlen("av_write_trailer error.\n"));
		return -1;
	}
	av_free(tc->data.picture);

	return 0;
}

int
freeall(Transcode * tc)
{
	int i;
	AVFrame * temp_picture;
	int16_t * temp_sample;

	for(i = 0; i < tc->fq.nb_frames; i++)
	{
		if(frame_queue_get(&tc->fq, &temp_picture))
		{
			writen(STDERR_FILENO, "frame_queue_get error.\n",
					strlen("frame_queue_get error.\n"));
			return -1;
		}
		av_free(temp_picture->data[0]);
		av_free(temp_picture);
	}

	if(tc->sq.whole_size > 0)
	{
		if(sample_queue_get(&tc->sq, &temp_sample, tc->sq.whole_size))
		{
			writen(STDERR_FILENO, "sample_queue_get error.\n",
					strlen("sample_queue_get error.\n"));
			return -1;
		}
		av_free(temp_sample);
	}

	if(tc->swscale_ctx)
		sws_freeContext(tc->swscale_ctx);

	if(tc->in_asti != -1)
	{
		if(tc->in_fc->streams[tc->in_asti]->codec->codec)
		{
			Pthread_mutex_lock(&avlib_mutex);
			avcodec_close(tc->in_fc->streams[tc->in_asti]->codec);
			Pthread_mutex_unlock(&avlib_mutex);
		}
		if(tc->out_asti != -1)
		{
			if(tc->out_fc->streams[tc->out_asti]->codec->codec)
			{
				av_free(tc->data.audio_outbuf);
				Pthread_mutex_lock(&avlib_mutex);
				avcodec_close(tc->out_fc->streams[tc->out_asti]->codec);
				Pthread_mutex_unlock(&avlib_mutex);
			}
			av_free(tc->out_fc->streams[tc->out_asti]->codec);
			av_free(tc->out_fc->streams[tc->out_asti]);
		}
	}
	
	if(tc->in_vsti != -1)
	{
		if(tc->in_fc->streams[tc->in_vsti]->codec->codec)
		{
			Pthread_mutex_lock(&avlib_mutex);
			avcodec_close(tc->in_fc->streams[tc->in_vsti]->codec);
			Pthread_mutex_unlock(&avlib_mutex);
		}
		if(tc->out_vsti != -1)
		{
			if(tc->out_fc->streams[tc->out_vsti]->codec->codec)
			{
				av_free(tc->data.video_outbuf);
				Pthread_mutex_lock(&avlib_mutex);
				avcodec_close(tc->out_fc->streams[tc->out_vsti]->codec);
				Pthread_mutex_unlock(&avlib_mutex);
			}
			av_free(tc->out_fc->streams[tc->out_vsti]->codec);
			av_free(tc->out_fc->streams[tc->out_vsti]);
		}
	}

	if(tc->in_fc)
		av_close_input_file(tc->in_fc); 

	if(tc->out_fc && tc->out_fc->oformat)
		if(!(tc->out_fc->oformat->flags & AVFMT_NOFILE))
			url_fclose(tc->out_fc->pb);

	if(tc->out_fc)
		av_free(tc->out_fc);

	return 0;
}


int
transcode(char * fromfilename, char * tofilename, int width, int height)
{
	Transcode transcode;
	
	init_transcode(&transcode, fromfilename, tofilename, width, height);

	if(init_decode(&transcode))
	{
		freeall(&transcode);
		return -1;
	}

	if(init_encode(&transcode))
	{
		freeall(&transcode);
		return -1;
	}

	if(init_swscale(&transcode))
	{
		freeall(&transcode);
		return -1;
	}

	if(transcoding(&transcode))
	{
		freeall(&transcode);
		return -1;
	}

	if(freeall(&transcode))
		return -1;

	return 0;
}

////////////////

void
changename(char * filename)
{
	int i, len;

	len = strlen(filename);
	for(i = len - 1; i >= 0; i--)
		if(filename[i] == '.')
		{
			filename[i] = '\0';
			break;
		}
	strcat(filename, ".flv");
}

void *
transcodethread(void * arg)     //负责进行转码的线程
{
	work * pgetwork;
	work * pputwork;
	int success;

	Pthread_detach(pthread_self());

	while(1)
	{
		Pthread_mutex_lock(&getqueueptr_mutex);
		if(getqueueptr == NULL)
		{
			while(getqueueptr == NULL)
				Pthread_cond_wait(&getqueueptr_cond, &getqueueptr_mutex);
			Pthread_mutex_unlock(&getqueueptr_mutex);
			continue;
		}
		else
		{
			pgetwork = getqueueptr;
			getqueueptr = pgetwork->next;
			pgetwork->next = NULL;
			Pthread_mutex_unlock(&getqueueptr_mutex);
		}

		pputwork = (work *)Malloc(sizeof(work));
		pputwork->next = NULL;
		strcpy(pputwork->filename, "transcode");
		strcat(pputwork->filename, pgetwork->filename);
		changename(pputwork->filename);
		success = transcode(pgetwork->filename, pputwork->filename, 0, 0);
		Unlink(pgetwork->filename);
		free(pgetwork);
		if(success == 0)
		{
			Pthread_mutex_lock(&putqueueptr_mutex);
				pputwork->next = putqueueptr;
				putqueueptr = pputwork;
				pputwork = NULL;
			Pthread_mutex_unlock(&putqueueptr_mutex);
		}
		else
		{
			writen(STDERR_FILENO, "Transcode error.\n",
					strlen("Transcode error.\n"));
			free(pputwork);
		}
	}

	return (NULL);
}


/////////////////////////////////////////////////

void
setnonblockandcloexec(int fd)    //设置套接口非阻塞模式
{
	int opt;
	
	opt = Fcntl(fd, F_GETFL, 0);	
	opt |= O_NONBLOCK;
	Fcntl(fd, F_SETFL, opt);
	opt = Fcntl(fd, F_GETFD, 0);
	opt |= FD_CLOEXEC;
	Fcntl(fd, F_SETFD, opt);
}

void
addtolink(conv * pnode)    //在主干链表上增加一个节点
{
	if(handleconv)
	{
		handleconv->pre = pnode;
		pnode->next = handleconv;
		handleconv = pnode;
		handleconv->pre = NULL;
		pnode = NULL;
	}
	else
	{
		handleconv = pnode;
		handleconv->pre = NULL;
		handleconv->next = NULL;
		pnode == NULL;
	}
}

void
delfromlink(conv * pnode)   //删除指针所指向的主干链表中的一个节点
{
	if(handleconv == NULL)
	{
		writen(STDERR_FILENO, "Link list error.\n",
					strlen("Link list error.\n"));
		exit(1);
	}
	if(pnode->pre == NULL && pnode->next == NULL)
	{
		free(pnode);
		handleconv = NULL;
		pnode = NULL;
	}
	else if(pnode->pre == NULL && pnode->next)
	{
		handleconv = pnode->next;
		handleconv->pre = NULL;
		pnode->next = NULL;
		free(pnode);
		pnode = NULL;
	}
	else if(pnode->pre && pnode->next == NULL)
	{
		pnode->pre->next = NULL;
		pnode->pre = NULL;
		free(pnode);
		pnode = NULL;
	}
	else
	{
		pnode->next->pre = pnode->pre;
		pnode->pre->next = pnode->next;
		pnode->next = NULL;
		pnode->pre = NULL;
		free(pnode);
		pnode = NULL;
	}
}

void
closeconv(conv * pconv)     //关闭一个会话
{
	if(pconv->plistencs)
	{
		Close(pconv->listenfd);
		free(pconv->plistencs);
		pconv->plistencs = NULL;
	}
	if(pconv->pdatacs)
	{
		Close(pconv->datafd);
		free(pconv->pdatacs);
		pconv->pdatacs = NULL;
	}
	if(pconv->pctrlcs)
	{
		Close(pconv->ctrlfd);
		free(pconv->pctrlcs);
		pconv->pctrlcs = NULL;
	}
	delfromlink(pconv);
	pconv = NULL;
}

///////////////////////////////////////////////

int
cmdanaly(int ctrlfd, char * cmd, int cmdlen)     //analyse the cmd
{
	int cmdtype = 0;
	
	if(cmdlen > 4)
	{
		if(cmd[0]=='U' && cmd[1]=='S' && cmd[2]=='E' && cmd[3]=='R' && cmd[4]==' ')
			cmdtype = USER;
		else if(cmd[0]=='P' && cmd[1]=='A' && cmd[2]=='S' && cmd[3]=='S' && cmd[4]==' ')
		  	cmdtype = PASS;
		else if(cmd[0]=='S' && cmd[1]=='Y' && cmd[2]=='S' && cmd[3]=='T')
		  	cmdtype = SYST;
		else if(cmd[0]=='T' && cmd[1]=='Y' && cmd[2]=='P' && cmd[3]=='E' && cmd[4]==' ')
		  	cmdtype = TYPE;
		else if(cmd[0]=='P' && cmd[1]=='O' && cmd[2]=='R' && cmd[3]== 'T' && cmd[4]==' ')
		  	cmdtype = PORT;
		else if(cmd[0]=='S' && cmd[1]=='T' && cmd[2]=='O' && cmd[3]=='R' && cmd[4]==' ')
		  	cmdtype = STOR;
		else if(cmd[0]=='Q' && cmd[1]=='U' && cmd[2]=='I' && cmd[3]=='T')
		  	cmdtype = QUIT;
		else if(cmd[0]=='P' && cmd[1]=='A' && cmd[2]=='S' && cmd[3]=='V')
			cmdtype = PASV;
		else
		  	cmdtype = 0;
	}
	if(cmdtype == 0)
		writen(ctrlfd, "500 Unknown command.\r\n", strlen("500 Unknown command.\r\n"));
	return (cmdtype);
}

int
checkstat(int ctrlfd, int logstate, int cmd)     //analyse the cmd under the certain state
{
	int result = 0;
	
	if(logstate == UNLOG)     //if we don't login ,we must login first or quit
	{
	  	if(cmd == USER || cmd == QUIT)
			result = 1;
		else
		  	writen(ctrlfd, "530 Please login with USER and PASS.\r\n",
						strlen("530 Please login with USER and PASS.\r\n"));
	}
	else if(logstate == LOGGING)    //if we input our user name , we must wait for the password or quit
	{
		if(cmd == PASS || cmd == QUIT)
		  	result = 1;
		else
		  	writen(ctrlfd, "530 Please login with USER and PASS.\r\n",
						strlen("530 Please login with USER and PASS.\r\n"));
	}
	else            //in other case we can do other cmd
	{
		if(cmd == USER)
			writen(ctrlfd, "530 Can't change to another user.\r\n",
						strlen("530 Can't change to another user.\r\n"));
		else if(cmd == PASS)
			writen(ctrlfd, "230 Already logged in.\r\n",
						strlen("230 Already logged in.\r\n"));
		else
			result = 1;
	}
	
	return result;
}


int
getcmd(conv * pconv)    //获得命令连接发来的命令
{
	int cmd;

	pconv->readn = read(pconv->ctrlfd, pconv->readnptr, pconv->readbuf + sizeof(pconv->readbuf) - pconv->readnptr);
	if(pconv->readn < 0 && errno == EWOULDBLOCK)
		return (0);
	else if(pconv->readn < 0)
	{
		writen(STDERR_FILENO, "Read cmd error.\n",
						strlen("Read cmd error.\n"));
		return (-1);
	}
	else if(pconv->readn == 0)
	{
		writen(STDERR_FILENO, "Connect closed by peer.\n",
					strlen("Connect closed by peer.\n"));
		return (-1);
	}
	pconv->readnptr += pconv->readn;
	if((pconv->searchptr = strstr(pconv->cmdptr, "\r\n")) )   //find the 0d0a first appear
	{
		pconv->searchptr += 2;
		//read the cmd between the cmdptr and searchptr
		strncpy(pconv->cmdbuf, pconv->cmdptr, pconv->searchptr - pconv->cmdptr);
		pconv->cmdbuf[pconv->searchptr - pconv->cmdptr]='\0';
		//change the point
		pconv->cmdptr = pconv->searchptr;
		if(pconv->readnptr == pconv->searchptr)
			pconv->cmdptr = pconv->readnptr = pconv->searchptr = pconv->readbuf;
	}
	else
		return (0);
	
	if((cmd = cmdanaly(pconv->ctrlfd, pconv->cmdbuf, strlen(pconv->cmdbuf))) == 0)
		return (0);
	if(checkstat(pconv->ctrlfd, pconv->logstate, cmd) == 0)
		return (0);      //if the command is not available or the command is not support goto redo

	return (cmd);
}

////////////////////

void
getcmdval(char * buf, char * cmdbuf)  //to get the varables in the cmd
{
	char * ptr;
	
	ptr = strchr(cmdbuf, ' ');
	ptr++;
	strcpy(buf, ptr);
	buf[strlen(buf) - 2] = '\0';
}

int
addrportanaly(char * peername, char * peeraddr, int * peerport)   //to transfer the ip addr and port
{
	int num[6];
	int count=0;
	
	count = sscanf(peername, "%d,%d,%d,%d,%d,%d", &num[0], &num[1], &num[2], &num[3], &num[4], &num[5]);
	if(count != 6)
		return (1);
	sprintf(peeraddr, "%d.%d.%d.%d", num[0], num[1], num[2], num[3]);
	*peerport = num[4]*256 + num[5];
	
	return (0);
}

////////////////////////////////////////////////

void
GetDoctrl(conv * pconv)
{
	pconv->cmd = getcmd(pconv);
	if(pconv->cmd == -1)
	{
		closeconv(pconv);
		pconv = NULL;
		return ;
	}
	else if(pconv->cmd == 0)
		return ;

	if(pconv->cmd == USER)
	{
		getcmdval(pconv->user, pconv->cmdbuf);
		pconv->logstate = LOGGING;
		writen(pconv->ctrlfd, "331 Please specify the password.\r\n",
					strlen("331 Please specify the password.\r\n"));
		return ;
	}	
	if(pconv->cmd == PASS)
	{
		getcmdval(pconv->pass, pconv->cmdbuf);
		if(strcmp(pconv->user, LOCALUSER) == 0 && strcmp(pconv->pass, LOCALPASS) == 0)
		{
			pconv->logstate = LOGIN;
			writen(pconv->ctrlfd, "230 Login successful.\r\n",
						strlen("230 Login successful.\r\n"));
		}
		else
		{
			pconv->logstate = UNLOG;
			writen(pconv->ctrlfd, "530 Login incorrect.\r\n",
						strlen("530 Login incorrect.\r\n"));
		}
		return ;
	}	
	if(pconv->cmd == SYST)
	{
		writen(pconv->ctrlfd, "215 UNIX Type: L8\r\n",
					strlen("215 UNIX Type: L8\r\n"));
		return ;
	}
	if(pconv->cmd == TYPE)
	{
		char type[64];
		
		getcmdval(type, pconv->cmdbuf);
		if(type[0] == 'A')
		  	writen(pconv->ctrlfd, "200 Switching to ASCII mode.\r\n",
						strlen("200 Switching to ASCII mode.\r\n"));
		else if(type[0] == 'I')
			writen(pconv->ctrlfd, "200 Switching to Binary mode.\r\n",
						strlen("200 Switching to Binary mode.\r\n"));
		else
			writen(pconv->ctrlfd, "500 Unrecognised TYPE command.\r\n",
						strlen("500 Unrecognised TYPE command.\r\n"));
		return ;
	}
	if(pconv->cmd == QUIT)
	{
		writen(pconv->ctrlfd, "221 Goodbye.\r\n",
					strlen("221 Goodbye.\r\n"));
		closeconv(pconv);
		pconv = NULL;
		return ;
	}
	
	if(pconv->cmd == PORT)
	{
		char peername[64];
		
		getcmdval(peername, pconv->cmdbuf);   //get the varable in the cmd
		if(addrportanaly(peername, pconv->peeraddr, &pconv->peerport) == 0)   //tranlate them to ip addr and port
		{
			writen(pconv->ctrlfd, "200 PORT command successful. Consider using PASV.\r\n",
						strlen("200 PORT command successful. Consider using PASV.\r\n"));
			pconv->connstate = UPASM;
		}
		else
		{
			writen(pconv->ctrlfd, "500 Illegal PORT command.\r\n",
						strlen("500 Illegal PORT command.\r\n"));
			pconv->connstate = NOPAM;
		}
		return ;
	}

	if(pconv->cmd == PASV)
	{
		struct epoll_event ev;
		struct sockaddr_in listenaddr, ctrladdr;
		socklen_t listenaddrlen, ctrladdrlen;
		int tempport; //the listen sock port
		uint32_t trans; //to store the ip addr in 32 bit mode
		char buff[64];  //to store the present case of the ip addr
		int taddr[4];  //the 4 val of the ip addr
		char tempname[64];  //the temp val to store the ip and port
		char tellcmd[64]; // the cmd respond to send to client
		
		pconv->listenfd = Socket(AF_INET, SOCK_STREAM, 0);
		setnonblockandcloexec(pconv->listenfd);
		Listen(pconv->listenfd, 1);

		ev.events = EPOLLIN;
		ev.data.ptr = Malloc(sizeof(convsock));
		((convsock *)ev.data.ptr)->type = LISTENTYPE;
		((convsock *)ev.data.ptr)->pconv = pconv;
		pconv->plistencs = (convsock *)ev.data.ptr;
		Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->listenfd, &ev);
		
		listenaddrlen = sizeof(listenaddr);
		ctrladdrlen = sizeof(ctrladdr);			
		Getsockname(pconv->listenfd, (struct sockaddr *)&listenaddr, &listenaddrlen);
		Getsockname(pconv->ctrlfd, (struct sockaddr *)&ctrladdr, &ctrladdrlen);
		tempport = ntohs(listenaddr.sin_port);
		trans = ctrladdr.sin_addr.s_addr;
		Inet_ntop(AF_INET, &trans, buff, sizeof(buff));
		sscanf(buff, "%d.%d.%d.%d", &taddr[0], &taddr[1], &taddr[2], &taddr[3]);
		sprintf(tempname,"%d,%d,%d,%d,%d,%d",
					taddr[0], taddr[1], taddr[2], taddr[3], tempport/256, tempport%256);
		sprintf(tellcmd, "227 Entering Passive Mode.(%s)\r\n", tempname);
		writen(pconv->ctrlfd, tellcmd, strlen(tellcmd));
		pconv->connstate = PASVM;  //change state

		return ;
	}

	if(pconv->cmd == STOR) //   +char filename[64];      getcmdval(filename, pconv->cmdbuf);   //get the file name
	{
		if(pconv->connstate == UPASM)  //in active case
		{
			struct epoll_event ev;
			struct sockaddr_in cliaddr;

			getcmdval(pconv->filename, pconv->cmdbuf);
			pconv->filefd = Open(pconv->filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
			setnonblockandcloexec(pconv->filefd);
			pconv->datafd = Socket(AF_INET, SOCK_STREAM, 0);
			setnonblockandcloexec(pconv->datafd);
			bzero(&cliaddr, sizeof(cliaddr));
			cliaddr.sin_family = AF_INET;
			cliaddr.sin_port = htons((short)pconv->peerport);
			Inet_pton(AF_INET, pconv->peeraddr, &cliaddr.sin_addr);
			if(connect(pconv->datafd, (struct sockaddr *)&cliaddr, sizeof(cliaddr)) < 0)
			{
				if(errno!=EINPROGRESS)
				{
					closeconv(pconv);
					pconv = NULL;
					return ;
				}
			}
			ev.events = EPOLLIN;
			ev.data.ptr = Malloc(sizeof(convsock));
			((convsock *)ev.data.ptr)->type = DATATYPE;
			((convsock *)ev.data.ptr)->pconv = pconv;
			pconv->pdatacs = (convsock *)ev.data.ptr;
			Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->datafd, &ev);

			writen(pconv->ctrlfd, "150 Ok to send data.\r\n",
						strlen("150 Ok to send data.\r\n"));
		}
		else if(pconv->connstate == PASVM)  //in passive case
		{
			getcmdval(pconv->filename, pconv->cmdbuf);
			pconv->filefd = Open(pconv->filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
			setnonblockandcloexec(pconv->filefd);
			if(pconv->convoptstate == LISTENACCEPT)   //如已收到连接,则发送150
			{
				writen(pconv->ctrlfd, "150 Ok to send data.\r\n",
							strlen("150 Ok to send data.\r\n"));	
				pconv->convoptstate = ALLDONOT;
			}
			else   //否则设置收到stor命令标志
				pconv->convoptstate = STORCMD;
			return ;		
		}

		else
			writen(pconv->ctrlfd, "425 Use PORT or PASV first.\r\n",
						strlen("425 Use PORT or PASV first.\r\n"));
		return ;
	}

}

void
GetDolisten(conv * pconv)
{
	struct epoll_event ev;
	struct sockaddr_in dataconnaddr;
	socklen_t dataconnaddrlen;

	dataconnaddrlen = sizeof(dataconnaddr);
	if((pconv->datafd = accept(pconv->listenfd, (struct sockaddr *)&dataconnaddr, &dataconnaddrlen)) == -1)
	{
		if(errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
			return ;
		writen(pconv->ctrlfd, "502 Data link accept error.\r\n",
					strlen("502 Data link accept error.\r\n"));
		if(pconv->convoptstate == STORCMD)
			Close(pconv->filefd);
		closeconv(pconv);
		pconv = NULL;
	}
	Close(pconv->listenfd);
	free(pconv->plistencs);
	pconv->plistencs = NULL;

	setnonblockandcloexec(pconv->datafd);
	ev.events = EPOLLIN;
	ev.data.ptr = Malloc(sizeof(convsock));
	((convsock *)ev.data.ptr)->type = DATATYPE;
	((convsock *)ev.data.ptr)->pconv = pconv;
	pconv->pdatacs = (convsock *)ev.data.ptr;
	Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->datafd, &ev);

	if(pconv->convoptstate == STORCMD)   //如已经收到stor命令,发送150
	{
		writen(pconv->ctrlfd, "150 Ok to send data.\r\n",
					strlen("150 Ok to send data.\r\n"));	
		pconv->convoptstate = ALLDONOT;
	}
	else  //否则设置收到连接标志
		pconv->convoptstate = LISTENACCEPT;
}

void
GetDodata(conv * pconv)
{
	int count = 0;
	int rnum = 0;
	
	rnum = read(pconv->datafd, pconv->databuf, sizeof(pconv->databuf));
	if(rnum < 0 && errno == EWOULDBLOCK)
		return ;
	else if(rnum < 0)
	{
		writen(pconv->ctrlfd, "552 Translate failed.\r\n",
					strlen("552 Translate failed.\r\n"));
		Close(pconv->filefd);
		closeconv(pconv);
		pconv = NULL;
		return ;
	}
	if(rnum > 0)
	{
		writen(pconv->filefd, pconv->databuf, rnum);
		return ;
	}
	if(rnum == 0)
	{
		work * pgetwork = NULL;
		work * tempwork = NULL;
	
		Close(pconv->filefd);
		Close(pconv->datafd);
		free(pconv->pdatacs);
		pconv->pdatacs = NULL;

		pgetwork = (work *)Malloc(sizeof(work));
		pgetwork->next = NULL;
		strcpy(pgetwork->filename, pconv->filename);

		Pthread_mutex_lock(&getqueueptr_mutex);
			if(getqueueptr == NULL)
			{
				getqueueptr = pgetwork;
				pgetwork = NULL;
				Pthread_cond_broadcast(&getqueueptr_cond);
			}
			else
			{
				tempwork = getqueueptr;
				while(tempwork->next != NULL)
					tempwork = tempwork->next;
				tempwork->next = pgetwork;
				tempwork = NULL;
				pgetwork = NULL;
			}
		Pthread_mutex_unlock(&getqueueptr_mutex);
		
		writen(pconv->ctrlfd, "226 File receive OK.\r\n",
					strlen("226 File receive OK.\r\n"));
		pconv->connstate = NOPAM;
		return ;
	}
}

////////////////////////////////////////////////////////////



int
getrespond(conv * pconv)
{
	pconv->resreadn = read(pconv->ctrlfd, pconv->resreadnptr, pconv->resreadbuf + sizeof(pconv->resreadbuf) - pconv->resreadnptr);
	if(pconv->resreadn < 0 && errno == EWOULDBLOCK)
		return (0);
	else if(pconv->resreadn < 0)
	{
		writen(STDERR_FILENO, "Read Respond error.\n",
						strlen("Read Respond error.\n"));
		return (-1);
	}
	else if(pconv->resreadn == 0)
		return (1);
	pconv->resreadnptr += pconv->resreadn;
	if((pconv->ressearchptr = strstr(pconv->respondptr, "\r\n")) )   //find the 0d0a first appear
	{
		pconv->ressearchptr += 2;
		//read the cmd between the cmdptr and searchptr
		strncpy(pconv->respond, pconv->respondptr, pconv->ressearchptr - pconv->respondptr);
		pconv->respond[pconv->ressearchptr - pconv->respondptr]='\0';
		//change the point
		pconv->respondptr = pconv->ressearchptr;
		if(pconv->resreadnptr == pconv->ressearchptr)
			pconv->respondptr = pconv->resreadnptr = pconv->ressearchptr = pconv->resreadbuf;

/*		writen(STDERR_FILENO, pconv->respond,
					strlen(pconv->respond));
*/
		
		return (2);
	}
	else
		return (0);
}


////////////////////////////////////////////////////////////

void
PutDoctrl(conv * pconv)
{
	int ret;
	
	if((ret = getrespond(pconv)) == 0)
		return ;
	if(ret < 0)
	{
		closeconv(pconv);
		pconv = NULL;
		writen(STDERR_FILENO, "Remote FTP Server error.1\n",
					strlen("Remote FTP Server error.1\n"));
		return ;
	}
	if(ret == 1 && pconv->putstate == QUITOK)
	{
		closeconv(pconv);
		pconv = NULL;
		return ;
	}
	if(ret == 1 && pconv->putstate != QUITOK)
	{
		closeconv(pconv);
		pconv = NULL;
		writen(STDERR_FILENO, "Remote FTP Server error.2\n",
					strlen("Remote FTP Server error.2\n"));
		return ;
	}

	switch(pconv->putstate)
	{
	case INIT:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];

			sprintf(sendcmd, "USER %s\r\n", STORSERVUSER);
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = USERSEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.3\n",
						strlen("Remote FTP Server error.3\n"));
			return ;
		}
		break;
	case USERSEND:
		if(pconv->respond[0] == '3')
		{
			char sendcmd[64];

			sprintf(sendcmd, "PASS %s\r\n", STORSERVPASS);
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = PASSSEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.4\n",
						strlen("Remote FTP Server error.4\n"));
			return ;
		}
		break;
	case PASSSEND:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];

			sprintf(sendcmd, "SYST\r\n");
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = SYSTSEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error, or password wrong.\n",
						strlen("Remote FTP Server error, or password wrong.\n"));
			return ;
		}
		break;
	case SYSTSEND:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];

			sprintf(sendcmd, "TYPE I\r\n");
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = TYPESEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.5\n",
						strlen("Remote FTP Server error.5\n"));
			return ;
		}
		break;
	case TYPESEND:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];
			struct epoll_event ev;
			struct sockaddr_in listenaddr, ctrladdr;
			socklen_t listenaddrlen, ctrladdrlen;
			int tempport; //the listen sock port
			uint32_t trans; //to store the ip addr in 32 bit mode
			char buff[64];  //to store the present case of the ip addr
			int taddr[4];  //the 4 val of the ip addr
			char tempname[64];  //the temp val to store the ip and port
			
			pconv->listenfd = Socket(AF_INET, SOCK_STREAM, 0);
			setnonblockandcloexec(pconv->listenfd);
			Listen(pconv->listenfd, 1);

			ev.events = EPOLLIN;
			ev.data.ptr = Malloc(sizeof(convsock));
			((convsock *)ev.data.ptr)->type = LISTENTYPE;
			((convsock *)ev.data.ptr)->pconv = pconv;
			pconv->plistencs = (convsock *)ev.data.ptr;
			Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->listenfd, &ev);
			
			listenaddrlen = sizeof(listenaddr);
			ctrladdrlen = sizeof(ctrladdr);			
			Getsockname(pconv->listenfd, (struct sockaddr *)&listenaddr, &listenaddrlen);
			Getsockname(pconv->ctrlfd, (struct sockaddr *)&ctrladdr, &ctrladdrlen);
			tempport = ntohs(listenaddr.sin_port);
			trans = ctrladdr.sin_addr.s_addr;
			Inet_ntop(AF_INET, &trans, buff, sizeof(buff));
			sscanf(buff, "%d.%d.%d.%d", &taddr[0], &taddr[1], &taddr[2], &taddr[3]);
			sprintf(tempname,"%d,%d,%d,%d,%d,%d",
						taddr[0], taddr[1], taddr[2], taddr[3], tempport/256, tempport%256);
			sprintf(sendcmd, "PORT %s\r\n", tempname);
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = PORTSEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.6\n",
						strlen("Remote FTP Server error.6\n"));
			return ;
		}
		break;
	case PORTSEND:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];

			pconv->filefd = open(pconv->filename, O_RDONLY);
			if(pconv->filefd < 0)
			{
				writen(STDERR_FILENO, "No send file.7\n",
							strlen("No send file.7\n"));
				exit(1);
			}
			setnonblockandcloexec(pconv->filefd);
			sprintf(sendcmd, "STOR %s\r\n", pconv->filename);
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = STORSEND;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.8\n",
						strlen("Remote FTP Server error.8\n"));
			return ;
		}
		break;
	case STORSEND:
		if(pconv->respond[0] == '1')
		{
			struct epoll_event ev;

			if(pconv->convoptstate == LISTENACCEPT)
			{
				ev.events = EPOLLOUT;
				ev.data.ptr = Malloc(sizeof(convsock));
				((convsock *)ev.data.ptr)->type = DATATYPE;
				((convsock *)ev.data.ptr)->pconv = pconv;
				pconv->pdatacs = (convsock *)ev.data.ptr;
				Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->datafd, &ev);
				pconv->convoptstate = ALLDONOT;
			}
			else
				pconv->convoptstate = SENDCMD;
			pconv->putstate = QUITSEND;
		}
		else
		{
			Close(pconv->filefd);
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.9\n",
						strlen("Remote FTP Server error.9\n"));
			return ;
		}
		break;
	case QUITSEND:
		if(pconv->respond[0] == '2')
		{
			char sendcmd[64];

			sprintf(sendcmd, "QUIT\r\n");
			writen(pconv->ctrlfd, sendcmd, strlen(sendcmd));
			pconv->putstate = QUITOK;
		}
		else
		{
			closeconv(pconv);
			pconv = NULL;
			writen(STDERR_FILENO, "Remote FTP Server error.0\n",
						strlen("Remote FTP Server error.0\n"));
			return ;
		}
		break;	
	case QUITOK:
		break;	
	}
}

void
PutDolisten(conv * pconv)
{
	struct epoll_event ev;
	struct sockaddr_in dataconnaddr;
	socklen_t dataconnaddrlen;

	dataconnaddrlen = sizeof(dataconnaddr);
	if((pconv->datafd = accept(pconv->listenfd, (struct sockaddr *)&dataconnaddr, &dataconnaddrlen)) == -1)
	{
		if(errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
			return ;
		Close(pconv->filefd);
		closeconv(pconv);
		pconv = NULL;
		writen(STDERR_FILENO, "Accept error.\n",
					strlen("Accept error.\n"));
		return ;
	}
	setnonblockandcloexec(pconv->datafd);
	Close(pconv->listenfd);
	free(pconv->plistencs);
	pconv->plistencs = NULL;

	if(pconv->convoptstate == SENDCMD)   //如已经收到stor命令,发送150
	{
		ev.events = EPOLLOUT;
		ev.data.ptr = Malloc(sizeof(convsock));
		((convsock *)ev.data.ptr)->type = DATATYPE;
		((convsock *)ev.data.ptr)->pconv = pconv;
		pconv->pdatacs = (convsock *)ev.data.ptr;
		Epoll_ctl(epollfd, EPOLL_CTL_ADD, pconv->datafd, &ev);
		pconv->convoptstate = ALLDONOT;
	}
	else  //否则设置收到连接标志
		pconv->convoptstate = LISTENACCEPT;
}

void
PutDodata(conv * pconv)
{
	int nwriteout;
	
	if(pconv->nlefttosend == 0)
	{
		pconv->nlefttosend = read(pconv->filefd, pconv->databuf, sizeof(pconv->databuf));
		if(pconv->nlefttosend < 0 && errno == EINTR)
		{
			pconv->nlefttosend = 0;
			return ;
		}
		if(pconv->nlefttosend < 0)
		{
			writen(STDERR_FILENO, "Read error.\n",
						strlen("Read error.\n"));
			exit(1);
		}
		pconv->dataptr = pconv->databuf;
		if(pconv->nlefttosend == 0)
		{
			Close(pconv->filefd);
			Close(pconv->datafd);
			free(pconv->pdatacs);
			pconv->pdatacs = NULL;
			Unlink(pconv->filename);
			return ;
		}
	}
	nwriteout = write(pconv->datafd, pconv->dataptr, pconv->nlefttosend);
	if(nwriteout < 0 && errno == EWOULDBLOCK)
		nwriteout = 0;
	if(nwriteout < 0)
	{
		char haha[1024];
		sprintf(haha, "Write sock error. %s\n", strerror(errno));
  		writen(STDERR_FILENO, haha, strlen(haha));
		exit(1);
	}
	pconv->nlefttosend -= nwriteout;
	pconv->dataptr += nwriteout;
}

/////////////////////////////////////////////////

void
Doctrl(conv * pconv)
{
	if(pconv->type == GETTYPE)
		GetDoctrl(pconv);
	else if(pconv->type == PUTTYPE)
		PutDoctrl(pconv);
}

void
Dolisten(conv * pconv)
{
	if(pconv->type == GETTYPE)
		GetDolisten(pconv);
	else if(pconv->type == PUTTYPE)
		PutDolisten(pconv);
}

void
Dodata(conv * pconv)
{
	if(pconv->type == GETTYPE)
		GetDodata(pconv);
	else if(pconv->type == PUTTYPE)
		PutDodata(pconv);
}


int
main()
{
/////////////////////////////////////////////////

	struct sockaddr_in servaddr;
	int listenfd;
	struct epoll_event pev[MAXCONN], ev;
	int i, j, k;

/////////////////////////////////////////////////

	pthread_t tid[THREADNUM];

	av_register_all();

	for(i = 0; i < THREADNUM; i++)
		Pthread_create(&tid[i], NULL, transcodethread, NULL);   //转码线程池，开启THREADNUM个转码线程

/////////////////////////////////////////////////

	bzero(pev, sizeof(pev));
	epollfd = Epoll_create(MAXCONN);

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(SERVPORT);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	listenfd = Socket(AF_INET, SOCK_STREAM, 0);
	setnonblockandcloexec(listenfd);
	Bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
	Listen(listenfd, 100);

	ev.events = EPOLLIN;
	ev.data.ptr = NULL;
	Epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);

/////////////////////////////////////////////////

	while(1)
	{
/////////////////////////////////////////////////

		int nfds = 0;

		nfds = Epoll_wait(epollfd, pev, MAXCONN, 10000);
		for(i=0; i < nfds; i++)
		{
			if(pev[i].data.ptr == NULL)    //返回为监听套接口
			{
				int connd;
				socklen_t clilen;
				struct sockaddr_in cliaddr;
				conv * temp = NULL;

				clilen = sizeof(cliaddr);
				if((connd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen)) == -1)
				{
					if(errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED)
						continue;
					writen(STDERR_FILENO, "Accept error.\n",
								strlen("Accept error.\n"));
					exit(1);
				}
				setnonblockandcloexec(connd);
				writen(connd, "220 (vsFTPd 2.0.7)\r\n", strlen("220 (vsFTPd 2.0.7)\r\n"));  //version

				bzero(&ev, sizeof(ev));
				ev.data.ptr = Malloc(sizeof(convsock));   //在主干链表中添加一项,并初始化
				((convsock *)ev.data.ptr)->type = CTRLTYPE;
				temp = (conv *)Malloc(sizeof(conv));
				bzero(temp, sizeof(conv));
				temp->type = GETTYPE;
				temp->ctrlfd = connd;	
				temp->pctrlcs = (convsock *)ev.data.ptr;
				((convsock *)ev.data.ptr)->pconv = temp;
				temp->logstate = UNLOG;
				temp->connstate = NOPAM;
				temp->cmdptr = temp->readnptr = temp->readbuf;
				addtolink(temp);
				temp = NULL;
				
				ev.events = EPOLLIN;
				Epoll_ctl(epollfd, EPOLL_CTL_ADD, connd, &ev);
				continue;
			}
			
			if(pev[i].events & (EPOLLIN | EPOLLOUT |EPOLLERR))
			{
				if( ((convsock *)pev[i].data.ptr)->type == CTRLTYPE && pev[i].events & EPOLLERR)
					closeconv( ((convsock *)pev[i].data.ptr)->pconv);
				else if( ((convsock *)pev[i].data.ptr)->type == CTRLTYPE && pev[i].events & EPOLLIN)
					Doctrl( ((convsock *)pev[i].data.ptr)->pconv);
				else if( ((convsock *)pev[i].data.ptr)->type == DATATYPE && pev[i].events & EPOLLERR)
					closeconv( ((convsock *)pev[i].data.ptr)->pconv);
				else if( ((convsock *)pev[i].data.ptr)->type == DATATYPE && pev[i].events & (EPOLLIN | EPOLLOUT))
					Dodata( ((convsock *)pev[i].data.ptr)->pconv);
				else if( ((convsock *)pev[i].data.ptr)->type == LISTENTYPE && pev[i].events & EPOLLERR)
					closeconv( ((convsock *)pev[i].data.ptr)->pconv);
				else if( ((convsock *)pev[i].data.ptr)->type == LISTENTYPE & EPOLLIN)
					Dolisten( ((convsock *)pev[i].data.ptr)->pconv);
			}
		}

/////////////////////////////////////////////////	

		if(putqueueptr)
		{
			conv * temp = NULL;
			struct sockaddr_in storservaddr; 
			work * pputwork;

			Pthread_mutex_lock(&putqueueptr_mutex);
				pputwork = putqueueptr;
				putqueueptr = pputwork->next;
				pputwork->next = NULL;
			Pthread_mutex_unlock(&putqueueptr_mutex);

			bzero(&storservaddr, sizeof(storservaddr));
			bzero(&ev, sizeof(ev));
			ev.data.ptr = Malloc(sizeof(convsock));   //在主干链表中添加一项,并初始化
			((convsock *)ev.data.ptr)->type = CTRLTYPE;
			temp = (conv *)Malloc(sizeof(conv));
			bzero(temp, sizeof(conv));
			temp->type = PUTTYPE;
			temp->putstate = INIT;
			temp->convoptstate = ALLDONOT;
			((convsock *)ev.data.ptr)->pconv = temp;
			temp->pctrlcs = (convsock *)ev.data.ptr;
			temp->dataptr = temp->databuf;
			temp->resreadnptr = temp->respondptr = temp->resreadbuf;
			strcpy(temp->filename, pputwork->filename);
			free(pputwork);
			temp->ctrlfd = Socket(AF_INET, SOCK_STREAM, 0);
			setnonblockandcloexec(temp->ctrlfd);
			storservaddr.sin_family = AF_INET;
			storservaddr.sin_port = htons((short)SERVPORT);
			Inet_pton(AF_INET, STORSERVADDR, &storservaddr.sin_addr);
			if(connect(temp->ctrlfd, (struct sockaddr *)&storservaddr, sizeof(storservaddr)) < 0)
			{
				if(errno != EINPROGRESS)
				{
					closeconv(temp);
					temp = NULL;
					continue;
				}
			}
			ev.events = EPOLLIN;
			Epoll_ctl(epollfd, EPOLL_CTL_ADD, temp->ctrlfd, &ev);
			
			addtolink(temp);
			temp = NULL;
		}			
	}

	return (0);
}





