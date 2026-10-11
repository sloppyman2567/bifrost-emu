/* OpenAL OSS negotiation, independent queues and poll-driven playback. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d: %s (errno=%d)\n",__LINE__,#x,errno); return 1; } } while(0)
static int set(int fd,unsigned long cmd,int value) {
 if(ioctl(fd,cmd,&value)) return -1;
 return value;
}
int main(void) {
 int first=open("/dev/dsp",O_WRONLY|O_NONBLOCK), second=open("/dev/dsp",O_WRONLY|O_NONBLOCK);
 CHECK(first>=0 && second>=0);
 int fmts=0; CHECK(ioctl(first,SNDCTL_DSP_GETFMTS,&fmts)==0);
 CHECK((fmts&(AFMT_S16_LE|AFMT_U8|AFMT_S8))==(AFMT_S16_LE|AFMT_U8|AFMT_S8));
 CHECK(set(first,SNDCTL_DSP_SETFMT,AFMT_S16_LE)==AFMT_S16_LE);
 CHECK(set(first,SNDCTL_DSP_CHANNELS,2)==2);
 CHECK(set(first,SNDCTL_DSP_SPEED,48000)==48000);
 CHECK(set(first,SNDCTL_DSP_SETTRIGGER,0)==0);
 CHECK(set(second,SNDCTL_DSP_SETFMT,AFMT_U8)==AFMT_U8);
 CHECK(set(second,SNDCTL_DSP_CHANNELS,1)==1);
 CHECK(set(second,SNDCTL_DSP_SPEED,24000)==24000);
 CHECK(set(second,SNDCTL_DSP_SETTRIGGER,0)==0);
 int fragments=(4<<16)|10;
 CHECK(ioctl(first,SNDCTL_DSP_SETFRAGMENT,&fragments)==0);
 CHECK(ioctl(second,SNDCTL_DSP_SETFRAGMENT,&fragments)==0);
 unsigned char *page=mmap((void*)0x6500000000ull,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 CHECK(page!=MAP_FAILED); CHECK(mprotect(page+4096,4096,PROT_NONE)==0);
 audio_buf_info *info=(audio_buf_info*)(page+4096-sizeof(*info));
 CHECK(ioctl(first,SNDCTL_DSP_GETOSPACE,info)==0);
 CHECK(info->fragsize==1024 && info->fragstotal==4 && info->bytes==4096 && info->fragments==4);
 errno=0; CHECK(ioctl(first,SNDCTL_DSP_GETOSPACE,NULL)==-1 && errno==EFAULT);
 unsigned char silence[8192]={0};
 CHECK(write(first,silence,sizeof(silence))==4096); // partial acceptance, no false full write
 errno=0; CHECK(write(first,silence,4)==-1 && errno==EAGAIN);
 CHECK(ioctl(first,SNDCTL_DSP_GETOSPACE,info)==0 && info->bytes==0 && info->fragments==0);
 struct pollfd p={first,POLLOUT,0}; CHECK(poll(&p,1,20)==0);
 CHECK(write(second,silence,4096)==4096);
 CHECK(ioctl(first,SNDCTL_DSP_RESET,0)==0);
 p.revents=0; CHECK(poll(&p,1,0)==1 && (p.revents&POLLOUT));
 CHECK(ioctl(second,SNDCTL_DSP_GETOSPACE,info)==0 && info->bytes==0);
 CHECK(write(first,silence,4096)==4096);
 CHECK(set(first,SNDCTL_DSP_SETTRIGGER,PCM_ENABLE_OUTPUT)==PCM_ENABLE_OUTPUT);
 p.revents=0; CHECK(poll(&p,1,1000)==1 && (p.revents&POLLOUT)); // SDL dummy consumes real mixer queue
 CHECK(close(first)==0);
 CHECK(ioctl(second,SNDCTL_DSP_GETOSPACE,info)==0 && info->bytes==0); // close is per descriptor
 CHECK(ioctl(second,SNDCTL_DSP_RESET,0)==0);
 CHECK(set(second,SNDCTL_DSP_SETFMT,0x40000000)==AFMT_S16_LE);
 CHECK(set(second,SNDCTL_DSP_SETFMT,AFMT_S8)==AFMT_S8);
 CHECK(set(second,SNDCTL_DSP_SPEED,1)==8000);
 CHECK(set(second,SNDCTL_DSP_SPEED,999999)==192000);
 CHECK(set(second,SNDCTL_DSP_CHANNELS,8)==2);
 CHECK(close(second)==0); CHECK(munmap(page,8192)==0);
 puts("OSS negotiation, independent queues and playback polling passed"); return 0;
}
