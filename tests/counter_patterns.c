/* #define _GNU_SOURCE */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define NITEMS 10
#define SEP ','
#define ECHAR ':'
#define TOK_NOISE 17
#define SYM_NOISE '-'
#define SYM_NUM 'N'
#define SYM_ANY 'X'
#define NO_AUTO_EXTEND

int main (void)
{
	const char* patts[] = {
		",N,X,,,1,16,-",
		",N,X,,,1,16,-:",
		",N,X,,,1,16,-:23",
		",N,X,,,1,16,-:-23",
		"",
		",",
		"2,N,X,,,1,16,-,2,3,4,5",
		"2N",
		"1,2N,",
		"N2",
		"NX",
		"F",
		"02",
		"19",
		"+2",
		"-2",
	};
	for (int i = 0; i < sizeof (patts) / sizeof (patts[0]); i++)
	{
		printf ("\n--------------------\nPattern: %s\n", patts[i]);
		unsigned int tok = 0;
		bool seek = false;
		bool symbolic = true;
		int ntoks = 0;
    const char* p = patts[i];
		for (; *p != '\0' && *p != ECHAR; p++)
		{
			if (ntoks == NITEMS)
			{
				printf ("Too many tokens\n");
				seek = true;
				break;
			}

			if (*p == SEP)
			{
				if ( ! seek )
				{
					if (symbolic && tok == 0)
						tok = SYM_ANY;
					printf ("\t--> Token: %d\n", tok);
					ntoks++;
				}
				tok = 0;
				symbolic = true;
				seek = false;
				continue;
			}
			if (seek)
				continue;
			printf ("%c ", *p);

			if (*p > 47 && *p < 58)
			{ /* ASCII 0 to 9 */
				if (symbolic && tok != 0)
				{
					printf ("Extra digits after symbols\n");
					goto invalid;
				}
				symbolic = false;

				if (tok != 0)
					tok *= 10;
				tok += (*p) - 48;
				if (tok > 16)
				{
					printf ("Invalid number\n");
					goto invalid;
				}
				continue;
			}

			if ( ! symbolic )
			{
				printf ("Extra symbols after digits\n");
				goto invalid;
			}
			if (tok != 0)
			{
				printf ("Symbolic tokens must be a single character\n");
				goto invalid;
			}

			switch (*p)
			{
				case SYM_NOISE:
					tok = TOK_NOISE;
					break;
				case SYM_NUM:
				case SYM_ANY:
					tok = *p;
					break;
				default:
					printf ("Invalid token\n");
					goto invalid;
			}

			continue;
invalid:
			seek = true;
#ifdef NO_AUTO_EXTEND
			break;
#endif
		}
		if ( ! seek )
		{
			if (ntoks == NITEMS)
				printf ("Too many tokens\n");
			else
			{
				if (symbolic && tok == 0)
					tok = SYM_ANY;
				printf ("\t--> Token: %d\n", tok);
				ntoks++;
			}
		}
#ifndef NO_AUTO_EXTEND
		for (; ntoks < NITEMS; ntoks++)
			printf ("\t--> Token: %d\n", SYM_ANY);
#endif
    long long int ticks = -1;
    char* buf;
    if (*p == ECHAR)
    {
      ticks = strtoll (p+1, &buf, 10);
      if (strlen (buf) > 0 || ticks <= 0)
        printf ("Invalid tick number\n");
      else
        printf ("Ticks = %lld\n", ticks);
    }

		printf ("Num tokens: %d\n", ntoks);
	}
	return 0;
}
