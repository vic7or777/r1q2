#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <linux/soundcard.h>
#include <stdio.h>

#include "../client/client.h"
#include "../client/snd_loc.h"

int audio_fd;
int snd_inited;

cvar_t *sndbits;
cvar_t *sndspeed;
cvar_t *sndchannels;
cvar_t *snddevice;

static int tryrates[] = { 11025, 22051, 44100, 8000 };

qboolean SNDDMA_Init(qboolean firsttime)
{

	int rc;
    int fmt;
	int tmp;
    int i;

	struct audio_buf_info info;
	int caps;

	if (snd_inited)
		return false;

	if (!snddevice) {
		sndbits = Cvar_Get("sndbits", "16", CVAR_ARCHIVE);
		sndspeed = Cvar_Get("sndspeed", "0", CVAR_ARCHIVE);
		sndchannels = Cvar_Get("sndchannels", "2", CVAR_ARCHIVE);
		snddevice = Cvar_Get("snddevice", "/dev/dsp", CVAR_ARCHIVE);
	}

// open /dev/dsp, confirm capability to mmap, and get size of dma buffer

	if (!audio_fd)
	{

		audio_fd = open(snddevice->string, O_RDWR);

		if (audio_fd < 0)
		{
			perror(snddevice->string);
			Com_Printf("Could not open %s\n", LOG_CLIENT, snddevice->string);
			return false;
		}
	}

    rc = ioctl(audio_fd, SNDCTL_DSP_RESET, 0);
    if (rc < 0)
	{
		perror(snddevice->string);
		Com_Printf("Could not reset %s\n", LOG_CLIENT, snddevice->string);
		close(audio_fd);
		return false;
	}

	if (ioctl(audio_fd, SNDCTL_DSP_GETCAPS, &caps)==-1)
	{
		perror(snddevice->string);
	        Com_Printf("Sound driver too old\n", LOG_CLIENT);
		close(audio_fd);
		return false;
	}

	if (!(caps & DSP_CAP_TRIGGER) || !(caps & DSP_CAP_MMAP))
	{
		Com_Printf("Sorry but your soundcard can't do this\n", LOG_CLIENT);
		close(audio_fd);
		return false;
	}

    if (ioctl(audio_fd, SNDCTL_DSP_GETOSPACE, &info)==-1)
    {   
        perror("GETOSPACE");
		Com_Printf("Um, can't do GETOSPACE?\n", LOG_CLIENT);
		close(audio_fd);
		return 0;
    }
    
// set sample bits & speed

    dma.samplebits = (int)sndbits->value;
	if (dma.samplebits != 16 && dma.samplebits != 8)
    {
        ioctl(audio_fd, SNDCTL_DSP_GETFMTS, &fmt);

        if (fmt & AFMT_S16_LE)
			dma.samplebits = 16;
        else if (fmt & AFMT_U8)
			dma.samplebits = 8;
    }

	dma.speed = (int)sndspeed->value;
	if (!dma.speed) {
        for (i=0 ; i<sizeof(tryrates)/4 ; i++)
            if (!ioctl(audio_fd, SNDCTL_DSP_SPEED, &tryrates[i])) break;
        dma.speed = tryrates[i];
    }

	dma.channels = (int)sndchannels->value;
	if (dma.channels < 1 || dma.channels > 2)
		dma.channels = 2;
	
	dma.samples = info.fragstotal * info.fragsize / (dma.samplebits/8);
	dma.submission_chunk = 1;

// memory map the dma buffer

	if (!dma.buffer)
		dma.buffer = (unsigned char *) mmap(NULL, info.fragstotal
			* info.fragsize, PROT_WRITE, MAP_FILE|MAP_SHARED, audio_fd, 0);
	if (!dma.buffer)
	{
		perror(snddevice->string);
		Com_Printf("Could not mmap %s\n", LOG_CLIENT, snddevice->string);
		close(audio_fd);
		return false;
	}

	tmp = 0;
	if (dma.channels == 2)
		tmp = 1;
    rc = ioctl(audio_fd, SNDCTL_DSP_STEREO, &tmp);
    if (rc < 0)
    {
		perror(snddevice->string);
	        Com_Printf("Could not set %s to stereo=%d", LOG_CLIENT, snddevice->string, dma.channels);
		close(audio_fd);
        return false;
    }
	if (tmp)
		dma.channels = 2;
	else
		dma.channels = 1;

    rc = ioctl(audio_fd, SNDCTL_DSP_SPEED, &dma.speed);
    if (rc < 0)
    {
		perror(snddevice->string);
	        Com_Printf("Could not set %s speed to %d", LOG_CLIENT, snddevice->string, dma.speed);
		close(audio_fd);
        return false;
    }

    if (dma.samplebits == 16)
    {
        rc = AFMT_S16_LE;
        rc = ioctl(audio_fd, SNDCTL_DSP_SETFMT, &rc);
        if (rc < 0)
		{
			perror(snddevice->string);
			Com_Printf("Could not support 16-bit data.  Try 8-bit.\n", LOG_CLIENT);
			close(audio_fd);
			return false;
		}
    }
    else if (dma.samplebits == 8)
    {
        rc = AFMT_U8;
        rc = ioctl(audio_fd, SNDCTL_DSP_SETFMT, &rc);
        if (rc < 0)
		{
			perror(snddevice->string);
			Com_Printf("Could not support 8-bit data.\n", LOG_CLIENT);
			close(audio_fd);
			return false;
		}
    }
	else
	{
		perror(snddevice->string);
		Com_Printf("%d-bit sound not supported.", LOG_CLIENT, dma.samplebits);
		close(audio_fd);
		return false;
	}

// toggle the trigger & start her up

    tmp = 0;
    rc  = ioctl(audio_fd, SNDCTL_DSP_SETTRIGGER, &tmp);
	if (rc < 0)
	{
		perror(snddevice->string);
		Com_Printf("Could not toggle.\n", LOG_CLIENT);
		close(audio_fd);
		return false;
	}
    tmp = PCM_ENABLE_OUTPUT;
    rc = ioctl(audio_fd, SNDCTL_DSP_SETTRIGGER, &tmp);
	if (rc < 0)
	{
		perror(snddevice->string);
		Com_Printf("Could not toggle.\n", LOG_CLIENT);
		close(audio_fd);
		return false;
	}

	dma.samplepos = 0;

	snd_inited = 1;
	return true;
}

int SNDDMA_GetDMAPos(void)
{

	struct count_info count;

	if (!snd_inited) return 0;

	if (ioctl(audio_fd, SNDCTL_DSP_GETOPTR, &count)==-1)
	{
		perror(snddevice->string);
		Com_Printf("Uh, sound dead.\n", LOG_CLIENT);
		close(audio_fd);
		snd_inited = 0;
		return 0;
	}
//	dma.samplepos = (count.bytes / (dma.samplebits / 8)) & (dma.samples-1);
//	fprintf(stderr, "%d    \r", count.ptr);
	dma.samplepos = count.ptr / (dma.samplebits / 8);

	return dma.samplepos;

}

void SNDDMA_Shutdown(void)
{
#if 0
	if (snd_inited)
	{
		close(audio_fd);
		snd_inited = 0;
	}
#endif
}

/*
==============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
}

void SNDDMA_BeginPainting (void)
{
}

