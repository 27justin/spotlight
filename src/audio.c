#include "audio.h"
#include <libavutil/avassert.h>

// Returns a pointer through reference and the number of devices through return value
AudioDevice** init_pulse(size_t* numDevices) {
	int nrDevices = cfg_size(C_AUDIO_ROOT, "device");
	AudioDevice** devices = malloc(sizeof(AudioDevice*) * nrDevices);
	*numDevices = nrDevices;

	int i;
	for(i = 0; i < nrDevices; ++i) {
		cfg_t *deviceSection = cfg_getnsec(C_AUDIO_ROOT, "device", i);
		AudioDevice* device = malloc(sizeof(AudioDevice));

		// Initialize the device with set parameters
		//
		pa_sample_spec deviceSpecification = {
			.format = PA_SAMPLE_S16LE,
			.rate = 44100,
			.channels = 2
		};
		char* desiredChannelLayout = cfg_getstr(deviceSection, "channels");
		if(strcmp(desiredChannelLayout, "mono") == 0) {
			deviceSpecification.channels = 1;
		} else if(strcmp(desiredChannelLayout, "stereo") == 0) {
			deviceSpecification.channels = 2;
		} else {
			printf("Invalid channel layout %s\n", desiredChannelLayout);
			return NULL;
		}


		device->name = (char*) cfg_title(deviceSection);
		device->pulseName = cfg_getstr(deviceSection, "name");
		device->num = i;
		device->sampleRate = deviceSpecification.rate;
		device->channels = deviceSpecification.channels;
		device->sampleSize = pa_sample_size_of_format(deviceSpecification.format);
		device->sampleSpec = deviceSpecification;

		
		// Initialize PulseAudio simple on this device
		int error;
		device->handle = pa_simple_new(NULL, "Spotlight", PA_STREAM_RECORD, device->pulseName, "Spotlight Record", &deviceSpecification, NULL, NULL, &error);
		if(device->handle == NULL) {
			printf("Error initializing PulseAudio on device %s: %s\n", device->name, pa_strerror(error));
			return NULL;
		}
		devices[i] = device;
	}
	
	for(int i = 0; i < nrDevices; ++i) {
		AudioDevice* device = devices[i];
		printf("Registered audio device %s: %s\n", device->name, device->pulseName);
	}
	return devices;
}

AudioStream* alloc_audio_stream(Capture* cap, AudioDevice* source) {
	AudioStream* audioStream = malloc(sizeof(AudioStream));
	memset(audioStream, 0, sizeof(AudioStream));
	audioStream->root = cap;
	audioStream->device = source;

	open_audio_stream(cap, audioStream);
	AVCodecContext* codecContext = audioStream->codecContext;


	int *numSamples = &audioStream->numSamples;
	if (codecContext->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
		*numSamples = 10000;
	else
		*numSamples = codecContext->frame_size;

	// Calculate the number of frames we have to buffer to hold `windowSize` seconds of audio
	int numFrames = codecContext->sample_rate / *numSamples * cap->windowSize;
	audioStream->bufferSize = numFrames;
	audioStream->frameBuffer = (AVFrame**)malloc(sizeof(AVFrame*) * numFrames);
	if(!audioStream->frameBuffer) {
		printf("Error allocating frame buffer\n");
		exit(1);
	}
	
	// Allocate resampling frame with original sample rate and sample format
	audioStream->resampleFrame = av_frame_alloc();
	if(!audioStream->resampleFrame) {
		printf("Error allocating resample frame\n");
		exit(1);
	}
	audioStream->resampleFrame->nb_samples = *numSamples;
	audioStream->resampleFrame->format = AV_SAMPLE_FMT_S16; // TODO: Make this configurable
	// Configure the resampleFrame's channel layout based on the source->channels
	switch(source->channels) {
		case 1: // MONO
			av_channel_layout_copy(&audioStream->resampleFrame->ch_layout, &(AVChannelLayout) AV_CHANNEL_LAYOUT_MONO);
			break;
		case 2: // STEREO
			av_channel_layout_copy(&audioStream->resampleFrame->ch_layout, &(AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO);
			break;
		default:
			printf("Unsupported number of channels: %i on device %s\n", source->channels, source->name);
			return NULL;
	}
	audioStream->resampleFrame->sample_rate = source->sampleRate;

	if(av_frame_get_buffer(audioStream->resampleFrame, 0) < 0) {
		printf("Error allocating resample frame buffer\n");
		exit(1);
	}
	for (int i = 0; i < numFrames; i++) {
		AVFrame* frame = audioStream->frameBuffer[i] = av_frame_alloc();
		if (!frame) {
			fprintf(stderr, "Failed to allocate frame %i of audio frame buffer\n", i);
			exit(1);
		}
		frame->nb_samples = *numSamples;
		frame->format = audioStream->codecContext->sample_fmt;
		frame->sample_rate = audioStream->codecContext->sample_rate;
		av_channel_layout_copy(&frame->ch_layout, &audioStream->codecContext->ch_layout);

		// Allocate the data buffers
		if (av_frame_get_buffer(frame, 0) < 0) {
			fprintf(stderr, "Failed to allocate data buffers for frame %i of audio frame buffer\n", i);
			exit(1);
		}
	}
	// Allocate the packet
	audioStream->packet = av_packet_alloc();
	if(!audioStream->packet) {
		printf("Error allocating packet\n");
		exit(1);
	}
	audioStream->packet->stream_index = audioStream->stream->index;

	// Allocate resampler
	if(audioStream->resampler == NULL) {
		audioStream->resampler = swr_alloc();
		if (!audioStream->resampler) {
			fprintf(stderr, "Could not allocate resampler context\n");
			exit(1);
		}

		av_opt_set_chlayout  (audioStream->resampler, "in_chlayout",       &audioStream->resampleFrame->ch_layout,      0);
		av_opt_set_int       (audioStream->resampler, "in_sample_rate",     source->sampleRate,    0);
		av_opt_set_sample_fmt(audioStream->resampler, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
		av_opt_set_chlayout  (audioStream->resampler, "out_chlayout",      &audioStream->codecContext->ch_layout,      0);
		av_opt_set_int       (audioStream->resampler, "out_sample_rate",    audioStream->codecContext->sample_rate,    0);
		av_opt_set_sample_fmt(audioStream->resampler, "out_sample_fmt",     audioStream->codecContext->sample_fmt,     0);

		int ret;
		if ((ret = swr_init(audioStream->resampler)) < 0) {
			fprintf(stderr, "Failed to initialize the resampling context\n");
			exit(1);
		}
	}
	return audioStream;
}



int resample(AudioStream *stream, AVFrame* inFrame, AVFrame* outFrame) {
	// Convert audio data
	const uint8_t** input = (const uint8_t**) inFrame->data;
	uint8_t** output = outFrame->data;
	int inSamples = inFrame->nb_samples;
	int outsamples = outFrame->nb_samples;
	int dstSamples = av_rescale_rnd(swr_get_delay(stream->resampler, outFrame->sample_rate) + inSamples, outFrame->sample_rate, outFrame->sample_rate, AV_ROUND_UP);
	int result = swr_convert(stream->resampler, output, dstSamples, input, inSamples);

	if (result < 0) {
		return -1;
	}
	return 0;
}


// Takes stream->codecContext->frame_size samples from the device and puts them into the writeIndex
void audio_encode(AudioStream* stream) {
	AVFrame* frame = stream->frameBuffer[stream->writeIndex];
	AVFrame* resampleFrame = stream->resampleFrame;

	// Read the samples from the device
	int error;
	int byteNum;
	// Calculating the byte num is fairly easy,
	//    sample-size * channels
	// We have to get the sample size from the pulse audio device, thus; it's specification
	
	byteNum = stream->device->sampleSize * stream->device->channels * stream->numSamples;
	if(pa_simple_read(stream->device->handle, *resampleFrame->data, byteNum, &error) < 0) {
		printf("Error reading from device %s: %s\n", stream->device->name, pa_strerror(error));
		exit(1);
	}

	resample(stream, resampleFrame, frame);

	stream->writeIndex = (stream->writeIndex + 1) % stream->bufferSize;
	stream->frameCount++;
}

void flush_audio_stream(AudioStream *audio) {
	int start_index;
	int encoding = 1;

	if (audio->frameCount > audio->bufferSize) {
		start_index = audio->writeIndex;
	} else {
		start_index = 0;
	}

	int n = 0;
	int ret;

	do {
		printf("\r[%s] Frame #%i (%i)/%i (PTS:%i)", audio->device->name, start_index, n, audio->frameCount, audio->pts);
		AVFrame *frame = audio->frameBuffer[start_index];
		frame->pts = frame->pkt_dts = audio->pts;
		audio->pts += frame->nb_samples;

		ret = avcodec_send_frame(audio->codecContext, frame);
		if (ret < 0) {
			printf("Error sending frame for encoding\n");
			return;
		}
		while (ret >= 0) {
			ret = avcodec_receive_packet(audio->codecContext, audio->packet);
		
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				goto outer;
			} else if (ret < 0) {
				printf("Error during encoding\n");
				return;
			}

			// Calculate packet duration
			av_packet_rescale_ts(audio->packet, audio->codecContext->time_base, audio->stream->time_base);
			audio->packet->stream_index = audio->stream->index;
			// Write packet to file
			av_interleaved_write_frame(audio->root->formatContext, audio->packet);
			av_packet_unref(audio->packet);
		}

outer:
		++n;
		if (n == audio->bufferSize) {
			encoding = 0;
		}
		start_index = (start_index + 1) % audio->bufferSize;
	}while (encoding);
}


void free_audio_stream(AudioStream *audio) {
	if(audio->resampler != NULL)
		swr_free(&audio->resampler);
	avcodec_free_context(&audio->codecContext);
	av_packet_free(&audio->packet);
	for(int i = 0; i < audio->bufferSize; i++) {
		av_frame_free(&audio->frameBuffer[i]);
	}
	free(audio->frameBuffer);
	free(audio);
}

// This function resets given stream by reallocating the
// AVCodecContext and AVCodec
int open_audio_stream(Capture *cap, AudioStream* audioStream) {
	if(audioStream->codecContext != NULL)
		avcodec_free_context(&audioStream->codecContext);

	// Allocate the codec context
	audioStream->codecContext = avcodec_alloc_context3(audioStream->codec);
	if(!audioStream->codecContext) {
		fprintf(stderr, "Could not allocate audio codec context\n");
		return 1;
	}

	// TODO: Don't pull the codec name from the config, save in AudioStream
	audioStream->codec = avcodec_find_encoder_by_name(cfg_getstr(C_AUDIO_ROOT, "codec"));
	if(!audioStream->codec) {
		fprintf(stderr, "Could not find audio codec\n");
		return 1;
	}

	audioStream->stream = avformat_new_stream(cap->formatContext, NULL);
	if(!audioStream->stream) {
		fprintf(stderr, "Error allocating audio stream\n");
		return 1;
	}

	audioStream->stream->index = cap->formatContext->nb_streams - 1;
	audioStream->stream->time_base = (AVRational) { 1, audioStream->device->sampleRate };
	if(audioStream->packet){
		audioStream->packet->stream_index = audioStream->stream->index;
		audioStream->packet->pts = 0;
		audioStream->packet->dts = 0;
		audioStream->packet->duration = 0;
	}
	audioStream->pts = 0;

	// Allocate codecContext
	audioStream->codecContext = avcodec_alloc_context3(audioStream->codec);
	if(!audioStream->codecContext) {
		fprintf(stderr, "Could not allocate audio codec context\n");
		return 1;
	}

	// Set codec parameters
	
	
	audioStream->codecContext->sample_fmt = audioStream->codec->sample_fmts ? audioStream->codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
	audioStream->codecContext->bit_rate = 64000;
	audioStream->codecContext->sample_rate = audioStream->device->sampleRate;



	switch(audioStream->device->channels) {
		case 1: // MONO
			av_channel_layout_copy(&audioStream->codecContext->ch_layout, &(AVChannelLayout) AV_CHANNEL_LAYOUT_MONO);
			break;
		case 2: // STEREO
			av_channel_layout_copy(&audioStream->codecContext->ch_layout, &(AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO);
			break;
		default:
			printf("Unsupported number of channels: %i on device %s\n", audioStream->device->channels, audioStream->device->name);
			return 1;
	}


	AVCodecParameters *codecParams = audioStream->stream->codecpar;
	codecParams->codec_type = AVMEDIA_TYPE_AUDIO;
	codecParams->codec_id = audioStream->codec->id;
	codecParams->bit_rate = audioStream->codecContext->bit_rate;
	codecParams->sample_rate = audioStream->codecContext->sample_rate;
	av_channel_layout_copy(&codecParams->ch_layout, &audioStream->codecContext->ch_layout);


	// Open the codec
	if(avcodec_open2(audioStream->codecContext, audioStream->codec, NULL) < 0) {
		fprintf(stderr, "Could not open audio codec\n");
		return 1;
	}

	int *numSamples = &audioStream->numSamples;
	if (audioStream->codecContext->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
		*numSamples = 10000;
	else
		*numSamples = audioStream->codecContext->frame_size;


	if(avcodec_parameters_from_context(codecParams, audioStream->codecContext) < 0) {
		fprintf(stderr, "Could not copy the stream parameters\n");
		return 1;
	}

	return 0;
}
