#ifndef TIMING_H
#define TIMING_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

// EXAMPLE FROM MSDN
/*
LARGE_INTEGER StartingTime, EndingTime, ElapsedMicroseconds;
LARGE_INTEGER Frequency;

QueryPerformanceFrequency(&Frequency); 
QueryPerformanceCounter(&StartingTime);

// Activity to be timed

QueryPerformanceCounter(&EndingTime);
ElapsedMicroseconds.QuadPart = EndingTime.QuadPart - StartingTime.QuadPart;


//
// We now have the elapsed number of ticks, along with the
// number of ticks-per-second. We use these values
// to convert to the number of elapsed microseconds.
// To guard against loss-of-precision, we convert
// to microseconds *before* dividing by ticks-per-second.
//

ElapsedMicroseconds.QuadPart *= 1000000;
ElapsedMicroseconds.QuadPart /= Frequency.QuadPart;
*/



static bool GLOBAL_INIT = false;
static LARGE_INTEGER GLOBAL_FREQUENCY     = {0};
static LARGE_INTEGER GLOBAL_START_TICKS   = {0};
static LARGE_INTEGER GLOBAL_END_TICKS     = {0};
static LARGE_INTEGER GLOBAL_ELAPSED_TICKS = {0};

inline void Win32StartTimer() {
	if(!GLOBAL_INIT) {
		QueryPerformanceFrequency(&GLOBAL_FREQUENCY);
		GLOBAL_INIT = true;
	}

	QueryPerformanceCounter(&GLOBAL_START_TICKS);
}

inline void Win32StopTimer() {
	QueryPerformanceCounter(&GLOBAL_END_TICKS);

	GLOBAL_ELAPSED_TICKS.QuadPart = GLOBAL_END_TICKS.QuadPart - 
                                    GLOBAL_START_TICKS.QuadPart;
}

inline double Win32GetSecondsElapsed() {
	double seconds_elapsed = (double)GLOBAL_ELAPSED_TICKS.QuadPart /
                             (double)GLOBAL_FREQUENCY.QuadPart;
	return seconds_elapsed;
}

inline double Win32GetMillisecondsElapsed() {
	GLOBAL_ELAPSED_TICKS.QuadPart *= 1000L;
	double ms_elapsed = (double)GLOBAL_ELAPSED_TICKS.QuadPart /
                        (double)GLOBAL_FREQUENCY.QuadPart;
	return ms_elapsed;
}

#endif
