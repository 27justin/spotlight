#include <stdio.h>
#include <confuse.h>
#include <stdlib.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <pthread.h>
#include <semaphore.h>

#include "audio.h"
#include "video.h"

#define BILLION 1000000000L

struct Capture *G_CAPTURE = NULL;



void save() {
	// Dump the correct window to the output directory
	G_CAPTURE->pause = 1;
	char* file = generate_output_filename();
	flush_capture(G_CAPTURE, file);
	free(file);
	G_CAPTURE->pause = 0;
}

void audio_thread(AudioStream* stream) {
	while(1) {
		while(G_CAPTURE->pause == 1);
		audio_encode(stream);
	}
}
void cleanup();

int main(int argc, char** argv) {
	signal(SIGUSR1, save);
	
	// Wait for video threads to spin up.
	int ready = 1;
	printf("Waiting for threads to spin up...\n");
	do {
		for(int i = 0; i < G_CAPTURE->nb_video_streams; ++i) {
			// Video streams take some time until they're ready
			VideoStream *stream = G_CAPTURE->video_streams[i];
			for(int j = 0; j < stream->orchestrator->nb_threads; ++j) {
				ready &= stream->orchestrator->contexts[j]->ready;
			}
		}
		if(ready == 1) {
			G_CAPTURE->pause = 0;
			break;
		}
		usleep(10000); // 10ms
	}while(!ready);

	printf("Ready, send SIGUSR1 to save.\n");

	while(1);
}

__attribute__((constructor))
void setup() {
	init_config();
	load_config();
	
	// Initialize capture
	G_CAPTURE = alloc_capture();
	G_CAPTURE->pause = 1;
	// This flag is automatically unset by the video threads when they get initialized.



	VideoStream *defaultStream = default_video(G_CAPTURE);
	if(!defaultStream) {
		printf("Couldn't initialize X11 video stream.");
		exit(1);
	}
	add_video_stream(G_CAPTURE, defaultStream);

	AudioDevice** devices = NULL;
	size_t numDevices = 0;
	if(C_AUDIO_ROOT) {
		devices = init_pulse(&numDevices);
		if(devices == NULL) {
			printf("Error initializing PulseAudio\n");
			exit(1);
		}
		for(int i = 0; i < numDevices; i++) {
			AudioStream* stream = alloc_audio_stream(G_CAPTURE, devices[i]);
			add_audio_stream(G_CAPTURE, stream);
		}
	}
	
	// Create a new thread for each audio stream
	for(int i = 0; i < G_CAPTURE->nb_audio_streams; i++) {
		AudioStream* stream = G_CAPTURE->audio_streams[i];
		pthread_t thread;
		pthread_create(&thread, NULL, audio_thread, stream);
	}

}
