#include <stdio.h>
#include <stdint.h>

static int check_thres (int8_t (*th)[4])
{
	for (int t = 1, rest_is_unset = 0; t < 4; t++)
	{
		/* printf ("Checking val %hhd\n", (*th)[t]); */
#ifdef USE_UINT
		int is_set = (*th)[t] > 0;
#else
		int is_set = (*th)[t] >= 0;
#endif
		if (is_set && (rest_is_unset || (*th)[t] <= (*th)[t-1]))
		{
			printf ("Threshold is invalid: %s\n",
				rest_is_unset ? "set" : "less than previous");
			return -1;
		}
		else if ( ! is_set )
			rest_is_unset = 1;
	}
	return 0;
}

int main (void)
{
#ifdef USE_UINT
	int8_t thres[][4] = {
		{0,0,0,0},
		{0,1,3,0},
		{0,1,3,5},
		{1,0,0,0},
		{1,3,0,0},
		{1,3,5,7},
	};
#else
	int8_t thres[][4] = {
		{-1,-1,-1,-1},
		{0,-1,-1,-1},
		{0,1,3,-1},
		{0,1,3,5},
		{1,-1,-1,-1},
		{1,3,-1,-1},
		{1,3,5,7},
	};
#endif
	uint8_t vals[] = {0, 1, 2, 3, 9};
	/* printf ("%lu, %lu\n", sizeof (thres), sizeof (vals)); */
	for (size_t t = 0; t < sizeof (thres) / sizeof (thres[0]); t++)
	{
		int8_t (*th)[4] = &thres[t];
		printf ("t = [%hhd, %hhd, %hhd, %hhd]\n", (*th)[0], (*th)[1], (*th)[2], (*th)[3]);
		if (check_thres (th) == -1)
			continue;
		
		for (size_t v = 0; v < sizeof (vals) / sizeof (vals[0]); v++)
		{
			uint8_t val = vals[v];
			int p = 0;
#ifdef USE_UINT
			for (; val >= (*th)[p] && p < 4 && (p == 0 || (*th)[p] > 0); p++)
#else
			for (; val >= (*th)[p] && p < 4 && (*th)[p] >= 0; p++)
#endif
				;
			printf ("  v = %hhu, p = %d\n", val, p);
		}
	}
	return 0;
}
