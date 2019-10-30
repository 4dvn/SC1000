/*
 * Copyright (C) 2018 Mark Hills <mark@xwax.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "player.h"
#include "track.h"
#include "sc_queue.h"
#include "biquad.h"

/* Bend playback speed to compensate for the difference between our
 * current position and that given by the timecode */

#define SYNC_TIME (1.0 / 2) /* time taken to reach sync */
#define SYNC_PITCH 0.05		/* don't sync at low pitches */
#define SYNC_RC 0.05		/* filter to 1.0 when no timecodes available */

/* If the difference between our current position and that given by
 * the timecode is greater than this value, recover by jumping
 * straight to the position given by the timecode. */

#define SKIP_THRESHOLD (1.0 / 8) /* before dropping audio */

/* The base volume level. A value of 1.0 leaves no headroom to play
 * louder when the record is going faster than 1.0. */

#define VOLUME (7.0 / 8)

// Time in seconds fader takes to decay
#define FADERDECAY 0.020
#define DECAYSAMPLES FADERDECAY * 48000

#define SQ(x) ((x) * (x))
#define TARGET_UNKNOWN INFINITY

/*
 * Return: the cubic interpolation of the sample at position 2 + mu
 */

static inline double cubic_interpolate(signed short y[4], double mu)
{
	signed long a0, a1, a2, a3;
	double mu2;

	mu2 = SQ(mu);
	a0 = y[3] - y[2] - y[0] + y[1];
	a1 = y[0] - y[1] - a0;
	a2 = y[2] - y[0];
	a3 = y[1];

	return (mu * mu2 * a0) + (mu2 * a1) + (mu * a2) + a3;
}

/*
 * Return: Random dither, between -0.5 and 0.5
 */

static double dither(void)
{
	unsigned int bit, v;
	static unsigned int x = 0xbeefface;

	/* Maximum length LFSR sequence with 32-bit state */

	bit = (x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
	x = x << 1 | bit;

	/* We can adjust the balance between randomness and performance
	 * by our chosen bit permutation; here we use a 12 bit subset
	 * of the state */

	v = (x & 0x0000000f) | ((x & 0x000f0000) >> 12) | ((x & 0x0f000000) >> 16);

	return (double)v / 4096 - 0.5; /* not quite whole range */
}

/*
 * Equivalent to build_pcm, but for use when the track is
 * not available
 *
 * Return: number of seconds advanced in the audio track
 * Post: buffer at pcm is filled with silence
 */

static double build_silence(signed short *pcm, unsigned samples,
							double sample_dt, double pitch)
{
	memset(pcm, '\0', sizeof(*pcm) * PLAYER_CHANNELS * samples);
	return sample_dt * pitch * samples;
}

/*
 * Change the timecoder used by this playback
 */

void player_set_timecoder(struct player *pl, struct timecoder *tc)
{
	assert(tc != NULL);
	pl->timecoder = tc;
	pl->recalibrate = true;
	pl->timecode_control = true;
}

/*
 * Post: player is initialised
 */
#define status_t int
status_t lowpass(biquad_t *filter, double center_freq, double Q);

void player_init(struct player *pl, unsigned int sample_rate,
				 struct track *track)
{
	assert(track != NULL);
	assert(sample_rate != 0);

	spin_init(&pl->lock);

	pl->sample_dt = 1.0 / sample_rate;
	pl->track = track;
	//player_set_timecoder(pl, tc);

	pl->position = 0.0;
	pl->offset = 0.0;
	pl->target_position = 0.0;;
	pl->last_difference = 0.0;

	pl->pitch = 0.0;
	pl->sync_pitch = 1.0;
	pl->volume = 0.0;
	pl->GoodToGo = 0;
	pl->samplesSoFar = 0;
	pl->nominal_pitch = 1.0;
	pl->timestamp = 0.0;
	double lastval = 0;

	if (!pl->justPlay)
	{
		pl->debugout = fopen("pitchvals.csv", "w");
	}

	/*lowpass(&pl->filter, 5000, 0.7071);
	lowpass(&pl->filter2, 5000, 0.7071);*/

	BQ_init(&pl->filter);
	BQ_setNum(&pl->filter, 0.0000008870817623267249, 0.0000017741635246534498, 0.0000008870817623267249);
	BQ_setDen(&pl->filter, -1.997334246315892, 0.9973377946429413);

	BQ_init(&pl->filter2);
	BQ_setNum(&pl->filter2, 0.0000008870817623267249, 0.0000017741635246534498, 0.0000008870817623267249);
	BQ_setDen(&pl->filter2, -1.997334246315892, 0.9973377946429413);

	/*BQ_init(&pl->filter);
	BQ_setNum(&pl->filter, 0.00000009865221032426577, 0.00000019730442064853154, 0.00000009865221032426577);
	BQ_setDen(&pl->filter, -1.9991114149567926,  0.9991118095656341);

	BQ_init(&pl->filter2);
	BQ_setNum(&pl->filter2, 0.00000009865221032426577, 0.00000019730442064853154, 0.00000009865221032426577);
	BQ_setDen(&pl->filter2, -1.9991114149567926,  0.9991118095656341);*/


}

/*
 * Pre: player is initialised
 * Post: no resources are allocated by the player
 */

void player_clear(struct player *pl)
{
	spin_clear(&pl->lock);
	track_release(pl->track);
}

/*
 * Enable or disable timecode control
 */

void player_set_timecode_control(struct player *pl, bool on)
{
	if (on && !pl->timecode_control)
		pl->recalibrate = true;
	pl->timecode_control = on;
}

/*
 * Toggle timecode control
 *
 * Return: the new state of timecode control
 */

bool player_toggle_timecode_control(struct player *pl)
{
	pl->timecode_control = !pl->timecode_control;
	if (pl->timecode_control)
		pl->recalibrate = true;
	return pl->timecode_control;
}

void player_set_internal_playback(struct player *pl)
{
	pl->timecode_control = false;
	pl->pitch = 1.0;
}

double player_get_position(struct player *pl)
{
	return pl->position;
}

double player_get_elapsed(struct player *pl)
{
	return pl->position - pl->offset;
}

double player_get_remain(struct player *pl)
{
	return (double)pl->track->length / pl->track->rate + pl->offset - pl->position;
}

bool player_is_active(const struct player *pl)
{
	return (fabs(pl->pitch) > 0.01);
}

/*
 * Cue to the zero position of the track
 */

void player_recue(struct player *pl)
{
	pl->offset = pl->position;
}

/*
 * Set the track used for the playback
 *
 * Pre: caller holds reference on track
 * Post: caller does not hold reference on track
 */

void player_set_track(struct player *pl, struct track *track)
{
	struct track *x;

	assert(track != NULL);
	assert(track->refcount > 0);

	spin_lock(&pl->lock); /* Synchronise with the playback thread */
	x = pl->track;
	pl->track = track;
	spin_unlock(&pl->lock);

	track_release(x); /* discard the old track */
}

/*
 * Set the playback of one player to match another, used
 * for "instant doubles" and beat juggling
 */

void player_clone(struct player *pl, const struct player *from)
{
	double elapsed;
	struct track *x, *t;

	elapsed = from->position - from->offset;
	pl->offset = pl->position - elapsed;

	t = from->track;
	track_acquire(t);

	spin_lock(&pl->lock);
	x = pl->track;
	pl->track = t;
	spin_unlock(&pl->lock);

	track_release(x);
}

/*
 * Synchronise to the position and speed given by the timecoder
 *
 * Return: 0 on success or -1 if the timecoder is not currently valid
 */

/*
 * Synchronise to the position given by the timecoder without
 * affecting the audio playback position
 */

static void calibrate_to_timecode_position(struct player *pl)
{
	assert(pl->target_position != TARGET_UNKNOWN);
	pl->offset += pl->target_position - pl->position;
	pl->position = pl->target_position;
}

void retarget(struct player *pl)
{
	double diff;

	if (pl->recalibrate)
	{
		calibrate_to_timecode_position(pl);
		pl->recalibrate = false;
	}

	/* Calculate the pitch compensation required to get us back on
	 * track with the absolute timecode position */

	diff = pl->position - pl->target_position;
	pl->last_difference = diff; /* to print in user interface */

	if (fabs(diff) > SKIP_THRESHOLD)
	{

		/* Jump the track to the time */

		pl->position = pl->target_position;
		fprintf(stderr, "Seek to new position %.2lfs.\n", pl->position);
	}
	else if (fabs(pl->pitch) > SYNC_PITCH)
	{

		/* Re-calculate the drift between the timecoder pitch from
		 * the sine wave and the timecode values */

		pl->sync_pitch = pl->pitch / (diff / SYNC_TIME + pl->pitch);
	}
}

/*
 * Seek to the given position
 */

void player_seek_to(struct player *pl, double seconds)
{
	pl->offset = pl->position - seconds;
}

unsigned long samplesSoFar = 0;

/*
 * Get a block of PCM audio data to send to the soundcard
 *
 * This is the main function which retrieves audio for playback.  The
 * clock of playback is decoupled from the clock of the timecode
 * signal.
 *
 * Post: buffer at pcm is filled with the given number of samples
 */

bool NearlyEqual(double val1, double val2, double tolerance)
{
	if (fabs(val1 - val2) < tolerance)
		return true;
	else
		return false;
}



static double build_pcm(struct player *pl, signed short *pcm, unsigned samples, bool looping)
{
	int s;
	double sample, step, vol, gradient, pitchGradient;
	inputstate is;
	double av = 0;

	vol = 0.5;

	for (s = 0; s < samples; s++)
	{

		int c, sa, q;
		double f;
		signed short i[PLAYER_CHANNELS][4];

		// Interpolate between input events
		if (InterpolateQueue(pl->scqueue, &pl->timestamp, &is.target_position))
		{
			//is.target_position = pl->position;
			/*fifoWrite(pl->filterqueue, &is, 1);

			av = 0;
			for (int i=0; i < pl->filterqueue->size; i++){
				av += pl->filterqueue->buffer[i].target_position;
			}

			pl->position = av / pl->filterqueue->size;*/
			//pl->position = is.target_position;

			//pl->position = BQ_process(&pl->filter, is.target_position);
			//pl->position = BQ_process(&pl->filter2, pl->position);
			
			pl->position = is.target_position;
			//pl->position = is.target_position;

			//if (!pl->justPlay) printf("%f\n", pl->position);
			/*if (!pl->justPlay)
			{
				fprintf(pl->debugout, "%.10f,%.10f,%.10f\n", pl->position, (pl->position - pl->last_position) * 48000, (is.target_position - pl->last_unf_position)* 48000);
				pl->last_position = pl->position;
				pl->last_unf_position = is.target_position;
			}*/
			pl->pitch = pl->position - pl->last_position;
			pl->last_position = pl->position;
			pl->timestamp += pl->sample_dt;
		}

		else {
			//printf("Underflowing\n");
			// Keep playing if we haven't got data
			pl->position += pl->pitch;
		}

		pl->samplesSoFar++;
		//sample = pl->samplesSoFar;
		sample = pl->position * 48000;
		// 4-sample window for audio interpolation
		sa = (int)(sample);
		if (sample < 0.0)
			sa--;
		f = sample - sa;
		sa--;

		for (q = 0; q < 4; q++, sa++)
		{
			if (sa < 0 || sa >= pl->track->length)
			{
				for (c = 0; c < PLAYER_CHANNELS; c++)
					i[c][q] = 0;
			}
			else
			{
				signed short *ts;
				int c;

				ts = track_get_sample(pl->track, sa);
				for (c = 0; c < PLAYER_CHANNELS; c++)
					i[c][q] = ts[c];
			}
		}

		for (c = 0; c < PLAYER_CHANNELS; c++)
		{
			double v;

			v = vol * cubic_interpolate(i[c], f) + dither();

			signed short *sp;
			sp = pcm + (2 * s) + c;

			if (v > SHRT_MAX)
			{
				*sp = SHRT_MAX;
			}
			else if (v < SHRT_MIN)
			{
				*sp = SHRT_MIN;
			}
			else
			{
				*sp = (signed short)v;
			}
		}
		

		//printf("%f\n", sample);
		//printf("%f\n", sample);
	}
}

void handleInput(struct player *pl, inputstate *state)
{
	pl->timestamp = state->timestamp;
	pl->target_position = state->target_position;
	if (!pl->justPlay)
		printf("Got Event - pos:%f, target_pos:%f - \n", pl->position, pl->target_position);
}
extern unsigned int numblocks;
void player_collect(struct player *pl, signed short *pcm, unsigned totalSamples)
{
	double r;
	if (!spin_try_lock(&pl->lock))
	{
		//r = build_silence(pcm, totalSamples, pl->sample_dt, 1.0); THis won't work anymore, but could still run interpolation algorithm
	}
	else
	{
		r = build_pcm(pl, pcm, totalSamples, 0);
		spin_unlock(&pl->lock);
	}
}

/*void player_collect(struct player *pl, signed short *pcm, unsigned samples)
{
	double r, pitch, target_volume, amountToDecay, target_pitch;
	double diff;
	inputstate hehe;
	unsigned startSample = 0;

	pitch = pl->pitch; // Original pitch for smoothing

	//pl->target_position = (sin(((double)pl->samplesSoFar) / 80000) + 1); // Sine wave to simulate scratching, used for debugging

	target_pitch = pl->pitch;

	amountToDecay = (DECAYSAMPLES) / (double)samples;

	if (NearlyEqual(pl->faderTarget, pl->faderVolume, amountToDecay)) // Make sure to set directly when we're nearly there to avoid oscilation
		pl->faderVolume = pl->faderTarget;
	else if (pl->faderTarget > pl->faderVolume)
		pl->faderVolume += amountToDecay;
	else
		pl->faderVolume -= amountToDecay;

	target_volume = fabs(pl->pitch) * VOLUME * pl->faderVolume;

	if (target_volume > 1.0)
		target_volume = 1.0;

	if (!pl->justPlay)
		printf("--------BLEH------\n");

	if (!spin_try_lock(&pl->lock))
	{
		r = build_silence(pcm, samples, pl->sample_dt, pitch);
		pl->position += r;
	}
	else
	{
		//r = build_pcm(pl, pcm, 50, pl->track, pl->volume, target_volume, pl->looping, 0);
		//r = build_pcm(pl, pcm, 50, pl->track, pl->volume, target_volume, pl->looping, 50);
		//r = build_pcm(pl, pcm, 50, pl->track, pl->volume, target_volume, pl->looping, 100);
		//r = build_pcm(pl, pcm, 50, pl->track, pl->volume, target_volume, pl->looping, 150);
		//r = build_pcm(pl, pcm, 41, pl->track, pl->volume, target_volume, pl->looping, 200);
		while (samples > 0)
		{
			// No input events in the fifo, just render the entire remaining block of samples and break
			if (!fifoRead(pl->scqueue, &hehe))
			{
				if (!pl->justPlay)
					printf("None %u - ss:%u sc:%u\n", hehe.timestamp, startSample, samples);
				r = build_pcm(pl, pcm, samples, pl->track, pl->volume, target_volume, pl->looping, startSample);
				samples = 0;
				break;
			}
			else
			{
				// Next fifo event is too far in the future for this block,
				unsigned long long samplesToNextMidiMessage = usecToSamples(hehe.timestamp - pl->timestamp);
				if (samplesToNextMidiMessage >= samples)
				{
					if (!pl->justPlay)
						printf("Next one too far in future - %u %u\n", samplesToNextMidiMessage, samples);
					r = build_pcm(pl, pcm, samples, pl->track, pl->volume, target_volume, pl->looping, startSample);
					handleInput(pl, &hehe);
					break;
				}
				// I think this is only needed if there is a minimum sample block size
				//if (samplesToNextMidiMessage < MINBLOCKSIZE)
				//{
				//	handleInput(pl, &hehe);
				//	continue;
				//}

				if (!pl->justPlay)
					printf("Yay %u -  ss:%u sc:%u next:%u\n", hehe.timestamp, startSample, samples, samplesToNextMidiMessage);
				r = build_pcm(pl, pcm, samplesToNextMidiMessage, pl->track, pl->volume, target_volume, pl->looping, startSample);

				handleInput(pl, &hehe);
				startSample += samplesToNextMidiMessage;
				samples -= samplesToNextMidiMessage;
			}
		}
		while (fifoRead(pl->scqueue, &hehe))
		{
			handleInput(pl, &hehe);
			if (!pl->justPlay)
				printf("Wipin \n");
		}

		spin_unlock(&pl->lock);
	}

	//

	pl->volume = target_volume;*/
