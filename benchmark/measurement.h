#ifndef MEASUREMENT_H
#define MEASUREMENT_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>

struct msrt {
	uint64_t data_processed;
	const char* name;
	struct timespec start, end;
};

#define MSRT_DECLARE(s) struct msrt msrt_##s##_t = {.name = #s}

#define MSRT_START(s) msrt_##s##_t.data_processed = 0;			\
		clock_gettime(CLOCK_MONOTONIC, &msrt_##s##_t.start)

#define MSRT_STOP(s) clock_gettime(CLOCK_MONOTONIC, &msrt_##s##_t.end)

#define MSRT_DATA(s, d) msrt_##s##_t.data_processed += (uint64_t)d

#define MSRT_DISPLAY_RESULT(s)						\
do {									\
	/* Delta in micro seconds (10^(-6) secs) */			\
	const uint64_t delta_usec =					\
		(msrt_##s##_t.end.tv_sec - msrt_##s##_t.start.tv_sec)	\
		* 1e6 +							\
		(msrt_##s##_t.end.tv_nsec - msrt_##s##_t.start.tv_nsec) \
		/ 1e3;							\
	const double sec = delta_usec / 1e6;				\
	char format[64] = {0};						\
	const double throughput = msrt_##s##_t.data_processed / sec;	\
	if (throughput < 1e3)						\
		sprintf(format, "%3.3f bytes / sec",			\
			throughput);					\
	else if (throughput < 1e6)					\
		sprintf(format, "%3.3f Kbytes / sec",			\
			throughput / 1e3);				\
	else if (throughput < 1e9)					\
		sprintf(format, "%3.3f Mbytes / sec",			\
			throughput / 1e6);				\
	else								\
		sprintf(format, "%3.3f Gbytes / sec",			\
			throughput / 1e9 );				\
	fprintf(stdout, "'%s' processed %lu bytes "			\
		"in %3.3f secs (%s)\n",					\
		msrt_##s##_t.name, msrt_##s##_t.data_processed,		\
	       sec, format);						\
	fflush(stdout);							\
} while (0);								\

#endif	/* MEASUREMENT_H */
