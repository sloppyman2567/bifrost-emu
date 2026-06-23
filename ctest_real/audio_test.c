/* audio_test.c — generate a simple sine wave and write it to /dev/dsp.
 * Tests the bifrost-emu audio backend: open /dev/dsp, write PCM data,
 * close. The emulator's --audio-dump flag captures the output to WAV.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>

int main(void) {
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        perror("open /dev/dsp");
        return 1;
    }
    printf("audio_test: opened /dev/dsp (fd=%d)\n", fd);
    printf("audio_test: writing 1 second of 440Hz sine wave...\n");

    /* Generate 1 second of 440Hz sine wave at 44100 Hz, stereo 16-bit. */
    int sample_rate = 44100;
    int duration = 1;  /* seconds */
    int num_samples = sample_rate * duration;
    int channels = 2;
    int16_t* buf = malloc(num_samples * channels * sizeof(int16_t));
    if (!buf) {
        perror("malloc");
        close(fd);
        return 1;
    }

    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / sample_rate;
        double val = sin(2.0 * M_PI * 440.0 * t) * 0.3;  /* 440Hz, 30% volume */
        int16_t sample = (int16_t)(val * 32767);
        buf[i * channels + 0] = sample;  /* left */
        buf[i * channels + 1] = sample;  /* right */
    }

    size_t total = num_samples * channels * sizeof(int16_t);
    size_t written = 0;
    while (written < total) {
        ssize_t w = write(fd, (uint8_t*)buf + written, total - written);
        if (w < 0) {
            perror("write");
            free(buf);
            close(fd);
            return 1;
        }
        written += w;
    }

    printf("audio_test: wrote %zu bytes of PCM data\n", written);
    free(buf);
    close(fd);
    printf("audio_test: done\n");
    return 0;
}
