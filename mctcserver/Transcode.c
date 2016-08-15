//gcc -o transcode transcode.c -lavformat -lavcodec -lswscale

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

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

//////////////////////////////////////////////////////////////

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
init_transcode(Transcode * tc, int argc, char ** argv)
{
	bzero(tc, sizeof(Transcode));
	strcpy(tc->in_filename, argv[1]);
	strcpy(tc->out_filename, argv[2]);
	tc->width = atoi(argv[3]);
	tc->height = atoi(argv[4]);
	tc->in_vsti = -1;
	tc->in_asti = -1;
	tc->out_vsti = -1;
	tc->out_asti = -1;
	sample_queue_init(&tc->sq);
	frame_queue_init(&tc->fq);
}

void
init_decode(Transcode * tc)
{
	int i = 0;
	AVCodec * codec = NULL;

	if(av_open_input_file(&tc->in_fc, tc->in_filename, NULL, 0, NULL))
	{
		printf("av_open_input_file error\n");
		exit(1);
	}
	if(av_find_stream_info(tc->in_fc) < 0)
	{
		printf("av_find_stream_info error\n");
		exit(1);
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
		printf("can not find audio and video stream\n");
		exit(1);
	}
	if(tc->in_vsti == -1)
		printf("can not find video stream\n");
	if(tc->in_asti == -1)
		printf("can not find audio stream\n");

	if(tc->in_asti != -1)
	{
		codec = avcodec_find_decoder(tc->in_fc->streams[tc->in_asti]->codec->codec_id);
		if(!codec)
		{
			printf("can not find audio decoder\n");
			exit(1);
		}
		if(avcodec_open(tc->in_fc->streams[tc->in_asti]->codec, codec))
		{
			printf("open audio decoder error\n");
			exit(1);
		}
	}

	if(tc->in_vsti != -1)
	{
		codec = avcodec_find_decoder(tc->in_fc->streams[tc->in_vsti]->codec->codec_id);
		if(!codec)
		{
			printf("can not find video decoder\n");
			exit(1);
		}
		if(avcodec_open(tc->in_fc->streams[tc->in_vsti]->codec, codec))
		{
			printf("open video decoder error\n");
			exit(1);
		}
	}
}

void
init_encode(Transcode * tc)
{
	AVOutputFormat * ofmt = NULL;

	ofmt = av_guess_format(NULL, tc->out_filename, NULL);
	if(!ofmt)
	{
		printf("can not deduce output format from file extension\n");
		exit(1);
	}
	tc->out_fc = avformat_alloc_context();
	if(!tc->out_fc)
	{
		printf("avformat_alloc_context error\n");
		exit(1);
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
			printf("av_new_stream error\n");
			exit(1);
		}
		tc->out_vsti = st->index;

		tc->out_fc->streams[tc->out_vsti]->codec->codec_id = tc->out_fc->oformat->video_codec;
		tc->out_fc->streams[tc->out_vsti]->codec->codec_type = CODEC_TYPE_VIDEO;
		
		tc->out_fc->streams[tc->out_vsti]->codec->bit_rate = 
			tc->in_fc->streams[tc->in_vsti]->codec->bit_rate ? tc->in_fc->streams[tc->in_vsti]->codec->bit_rate : 5000000;
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
			printf("av_new_stream error\n");
			exit(1);
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
		printf("no output stream\n");
		exit(1);
	}

	if(av_set_parameters(tc->out_fc, NULL) < 0)
	{
		printf("av_set_parameters error\n");
		exit(1);
	}
	dump_format(tc->out_fc, 0, tc->out_filename, 1);   //debug

	if(tc->out_vsti != -1)
	{
		AVCodec * codec;

		tc->data.video_outbuf_size = 400000;
		tc->data.video_outbuf = av_malloc(tc->data.video_outbuf_size);
		if(!tc->data.video_outbuf)
		{
			printf("av_malloc error\n");
			exit(1);
		}
		tc->data.picture = NULL;

		codec = avcodec_find_encoder(tc->out_fc->streams[tc->out_vsti]->codec->codec_id);
		if(!codec)
		{
			printf("can not find video encoder\n");
			exit(1);
		}
		if(avcodec_open(tc->out_fc->streams[tc->out_vsti]->codec, codec) < 0)
		{
			printf("open video encoder error\n");
			exit(1);
		}
	}

	if(tc->out_asti != -1)
	{
		AVCodec * codec;

		tc->data.audio_outbuf_size = 100000;
		tc->data.audio_outbuf = av_malloc(tc->data.audio_outbuf_size);
		if(!tc->data.audio_outbuf)
		{
			printf("av_malloc error\n");
			exit(1);
		}

		codec = avcodec_find_encoder(tc->out_fc->streams[tc->out_asti]->codec->codec_id);
		if(!codec)
		{
			printf("can not find audio encoder\n");
			exit(1);
		}
		if(avcodec_open(tc->out_fc->streams[tc->out_asti]->codec, codec) < 0)
		{
			printf("open audio encoder error\n");
			exit(1);
		}

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
}

void
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
			printf("sws_getContext error\n");
			exit(1);
		}
	}
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

void
transcoding(Transcode * tc)
{
	int decodefinish;
	double audio_pts, video_pts;

	if(!(tc->out_fc->oformat->flags & AVFMT_NOFILE))
		if(url_fopen(&tc->out_fc->pb, tc->out_filename, URL_WRONLY) < 0)
		{
			printf("can not open output file\n");
			exit(1);
		}

	if(av_write_header(tc->out_fc))
	{
		printf("av_write_header error\n");
		exit(1);
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
						printf("avcodec_decode_video2 error\n");
						exit(1);
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
							printf("frame_queue_put error\n");
							exit(1);
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
							printf("avcodec_decode_audio2 error\n");
							exit(1);
						}
						if(link_sample_size > 0)
							if(sample_queue_put(&tc->sq, link_sample, link_sample_size))
							{
								printf("sample_queue_put error\n");
								exit(1);
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
				printf("sample_queue_get error\n");
				exit(1);
			}
			pkt.size = avcodec_encode_audio(tc->out_fc->streams[tc->out_asti]->codec,
												tc->data.audio_outbuf, tc->data.audio_outbuf_size, tc->data.samples);
			if(pkt.size < 0)
			{
				printf("avcodec_encode_audio error\n");
				exit(1);
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
					printf("writing audio frame error\n");
					exit(1);
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
				printf("frame_queue_get error\n");
				exit(1);
			}
			pkt.size = avcodec_encode_video(tc->out_fc->streams[tc->out_vsti]->codec,
												tc->data.video_outbuf, tc->data.video_outbuf_size, temp_frame);
			if(pkt.size < 0)
			{
				printf("avcodec_encode_video error\n");
				exit(1);
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
					printf("writing video frame error\n");
					exit(1);
				}
			}
			av_free(temp_frame->data[0]);
			av_free(temp_frame);
		}
	}

///////////////////////////////////////////////////////////////////////
	if(av_write_trailer(tc->out_fc))
	{
		printf("av_write_trailer error\n");
		exit(1);
	}

	av_free(tc->data.picture);
}

void
freeall(Transcode * tc)
{
	int i;
	AVFrame * temp_picture;
	int16_t * temp_sample;

	for(i = 0; i < tc->fq.nb_frames; i++)
	{
		if(frame_queue_get(&tc->fq, &temp_picture))
		{
			printf("frame_queue_get error\n");
			exit(1);
		}
		av_free(temp_picture->data[0]);
		av_free(temp_picture);
	}

	if(tc->sq.whole_size > 0)
	{
		if(sample_queue_get(&tc->sq, &temp_sample, tc->sq.whole_size))
		{
			printf("sample_queue_get error\n");
			exit(1);
		}
		av_free(temp_sample);
	}

	if(tc->swscale_ctx)
		sws_freeContext(tc->swscale_ctx);

	if(tc->in_asti != -1)
	{
		if(tc->in_fc->streams[tc->in_asti]->codec->codec)
			avcodec_close(tc->in_fc->streams[tc->in_asti]->codec);
		if(tc->out_asti != -1)
		{
			if(tc->out_fc->streams[tc->out_asti]->codec->codec)
			{
				av_free(tc->data.audio_outbuf);
				avcodec_close(tc->out_fc->streams[tc->out_asti]->codec);
			}
			av_free(tc->out_fc->streams[tc->out_asti]->codec);
			av_free(tc->out_fc->streams[tc->out_asti]);
		}
	}
	
	if(tc->in_vsti != -1)
	{
		if(tc->in_fc->streams[tc->in_vsti]->codec->codec)
			avcodec_close(tc->in_fc->streams[tc->in_vsti]->codec);
		if(tc->out_vsti != -1)
		{
			if(tc->out_fc->streams[tc->out_vsti]->codec->codec)
			{
				av_free(tc->data.video_outbuf);
				avcodec_close(tc->out_fc->streams[tc->out_vsti]->codec);
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
}




int
main(int argc, char ** argv)
{
	Transcode transcode;
	
	if(argc != 5)
	{
		printf("Usage: ./a.out input_file output_file width height\n");
		return -1;
	}
	init_transcode(&transcode, argc, argv);

	av_register_all();

	init_decode(&transcode);

	init_encode(&transcode);

	init_swscale(&transcode);

	transcoding(&transcode);

	freeall(&transcode);

	return 0;
}



