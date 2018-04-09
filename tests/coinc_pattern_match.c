#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "cutil.h"
#include "api.h"

#define NLOOPS 10000000LU
#define VERBOSE_NLOOPS 50LU
#define NCHANNELS 8
#define TICK_EVERY 100
#define MAX_VAL 18 // noise = 17, unknown = 18
#define MAX_NUM 16
#define TOK_NOISE 17 // 0x11
#define TOK_UNKNOWN 18 // 0x12
#define TOK_NUM 0x14
#define TOK_ANY 0x18
#define V2
#ifdef V2
#  define matches matches_v2
#  define is_tick is_tick_v2
#else
#  define matches matches_v1
#  define is_tick is_tick_v1
#endif

typedef uint8_t (*coinc_pt)[NCHANNELS];

static coinc_pt
new_vec (void)
{
	coinc_pt vec_p = malloc (NCHANNELS);
	if (vec_p == NULL)
		return NULL;
	int r = rand ();
	if ((int)((double)r * TICK_EVERY / RAND_MAX) == 0)
	{
		memset (vec_p, 0, NCHANNELS);
		return vec_p;
	}
	for (int i = 0; i < NCHANNELS; i++)
	{
		r = (int)((double)r * (MAX_VAL+1) / RAND_MAX);
		if (r == MAX_VAL+1)
			r--;
		(*vec_p)[i] = r;
		r = rand ();
	}
	return vec_p;
}

bool matches_v1 (coinc_pt vec_p, coinc_pt patt_p)
{
#if NLOOPS <= VERBOSE_NLOOPS
	printf ("--------------------\n");
#endif
	for (int i = 0; i < NCHANNELS; i++)
	{
		uint8_t v = (*vec_p)[i];
		uint8_t p = (*patt_p)[i];
#if NLOOPS <= VERBOSE_NLOOPS
		printf ("val %2hhu vs patt %2hhu: ", v, p);
#endif
		if (p == TOK_ANY)
		{
#if NLOOPS <= VERBOSE_NLOOPS
			printf ("OK\n");
#endif
			continue;
		}
		else if (v == TOK_UNKNOWN)
		{
#if NLOOPS <= VERBOSE_NLOOPS
			printf ("Nah man\n");
			continue;
#endif
			return false;
		}
		else if (v > 0 && v <= MAX_NUM)
		{
			if (p != TOK_NUM && (p ^ v) != 0)
			{
#if NLOOPS <= VERBOSE_NLOOPS
				printf ("Nah man\n");
				continue;
#endif
				return false;
			}
		}
		else if (v == 0 || v == TOK_NOISE)
		{
			if (p != 0 && (p ^ v) != 0)
			{
#if NLOOPS <= VERBOSE_NLOOPS
				printf ("Nah man\n");
				continue;
#endif
				return false;
			}
		}
		else
			assert (false);
#if NLOOPS <= VERBOSE_NLOOPS
		printf ("OK\n");
#endif
	}
	return true;
}

bool matches_v2 (coinc_pt vec_p, coinc_pt patt_p)
{
	return false;
}

#if 0
bool is_tick_v1 (coinc_pt vec_p)
{
	uint8_t tick[NCHANNELS];
	memset (tick, TES_COINC_TOK_TICK, NCHANNELS);
	tick[0] |= (*vec_p)[0] & TES_COINC_FLAG_MASK;
	return (memcmp (&tick, vec_p, NCHANNELS) == 0);
}

bool is_tick_v2 (coinc_pt vec_p)
{
	for (int i = 0; i < NCHANNELS; i++)
		if (((*vec_p)[i] & ~TES_COINC_FLAG_MASK) != TES_COINC_TOK_TICK)
			return false;
	return true;
}
#endif

int main (void)
{
	{
		coinc_pt vec_p;
		assert (sizeof (*vec_p) == NCHANNELS);
	}
	srand (time (NULL));
	uint8_t pattern[NCHANNELS] = {0, TOK_NUM, TOK_ANY, TOK_NOISE, 1, 2, 3, 4};
	struct timespec ts;
	long long avg;

	long unsigned int matched = 0;
	tic (&ts);
	for (long unsigned int r = 0; r < NLOOPS; r++)
	{
		coinc_pt vec_p = new_vec ();
		if (matches (vec_p, &pattern))
			matched++;
		free (vec_p);
	}
	avg = toc (&ts);
	if (avg == -1)
		return -1;
	printf (
		"No. matches:  %lu\n"
		"Average time: %.5e\n",
		matched,
		(double)avg / NSEC_IN_SEC / NLOOPS);

#if 0
	long unsigned int ticks = 0;
	tic (&ts);
	for (long unsigned int r = 0; r < NLOOPS; r++)
	{
		coinc_pt vec_p = new_vec ();
		if (is_tick (vec_p))
			ticks++;
		free (vec_p);
	}
	avg = toc (&ts);
	if (avg == -1)
		return -1;
	printf (
		"No. ticks:  %lu\n"
		"Average time: %.5e\n",
		ticks,
		(double)avg / NSEC_IN_SEC / NLOOPS);
#endif
	return 0;
}
