#ifndef AUDIO_H_
#define AUDIO_H_

#include <pulse/simple.h>
#include <pulse/error.h>
#include <libswresample/swresample.h>

#include "spotlight.h"

typedef struct AudioDevice {
	char* name;
	char* pulseName;
	int num;
	unsigned int sampleRate;
	unsigned int channels;
	unsigned int sampleSize;
	pa_simple *handle;
	pa_sample_spec sampleSpec;
	pa_buffer_attr *bufferAttr;
} AudioDevice;

extern pa_sample_spec G_SAMPLE_SPEC;


extern AudioDevice** init_pulse(size_t*);
extern AudioStream* alloc_audio_stream(Capture*, AudioDevice*);
extern void flush_audio_stream(AudioStream*);
extern void free_audio_stream(AudioStream*);
// TODO: Same as video.c, this function name is misleading
extern int open_audio_stream(Capture*, AudioStream*);
extern void free_pulse();
extern void audio_encode(AudioStream*);

#endif
