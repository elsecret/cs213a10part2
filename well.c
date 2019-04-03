#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "uthread.h"
#include "uthread_mutex_cond.h"

#ifdef VERBOSE
#define VERBOSE_PRINT(S, ...) printf (S, ##__VA_ARGS__);
#else
#define VERBOSE_PRINT(S, ...) ;
#endif

#define MAX_OCCUPANCY      3
#define NUM_ITERATIONS     100
#define NUM_PEOPLE         20
#define FAIR_WAITING_COUNT 4


/**
 * You might find these declarations useful.
 */
enum Endianness { LITTLE = 0, BIG = 1 };
const static enum Endianness oppositeEnd[] = { BIG, LITTLE };

int bigs = 0;
int littles = 0;

struct Well {
	int occupancy;
	int fair_wait_counter;
	int is_new;
	enum Endianness endianness;
	uthread_mutex_t mx;
	uthread_cond_t big;
	uthread_cond_t little;
};

struct Well* createWell() {
	struct Well* Well = malloc(sizeof(struct Well));
	Well->mx = uthread_mutex_create();
	Well->big = uthread_cond_create(Well->mx);
	Well->little = uthread_cond_create(Well->mx);
	Well->fair_wait_counter = 0;
	Well->occupancy = 0;
	Well->is_new = 1;
	return Well;
}

struct Well* Well;

#define WAITING_HISTOGRAM_SIZE (NUM_ITERATIONS * NUM_PEOPLE)
int             entryTicker;                                          // incremented with each entry
int             waitingHistogram[WAITING_HISTOGRAM_SIZE];
int             waitingHistogramOverflow;
uthread_mutex_t waitingHistogrammutex;
int             occupancyHistogram[2][MAX_OCCUPANCY + 1];

void lock() {
	uthread_mutex_lock(Well->mx);
}

void unlock() {
	uthread_mutex_unlock(Well->mx);
}

void wait(enum Endianness endianness) {
	if (endianness == BIG) {
		uthread_cond_wait(Well->big);
	}
	else {
		uthread_cond_wait(Well->little);
	}
}

void recordWaitingTime(int waitingTime) {
	uthread_mutex_lock(waitingHistogrammutex);
	if (waitingTime < WAITING_HISTOGRAM_SIZE)
		waitingHistogram[waitingTime] ++;
	else
		waitingHistogramOverflow++;

	// update occupancyHistogram
	occupancyHistogram[Well->endianness][Well->occupancy]++;

	uthread_mutex_unlock(waitingHistogrammutex);
}

// Note: this is critical section (lock is held)
void wait_for_entry(enum Endianness g) {
	// Cases:
	// 1. the well is fresh. In that case, drink out of it.
	if (Well->is_new) {
		Well->is_new = 0;
		return;
	}
	// 2. Wait until the well is available with re-waiting if there is competition.
	while (Well->occupancy == MAX_OCCUPANCY || Well->fair_wait_counter == FAIR_WAITING_COUNT || Well->endianness != g)
	{
		wait(g);
	}
	return;
}

// Critical. Lock held.
void drink(enum Endianness g) {
	Well->endianness = g;
	Well->occupancy++;
	Well->fair_wait_counter++;
}

// attempt to enter the well
void enterWell(enum Endianness g) {
	// attempt to get in the well
	lock();
	int initial_time = entryTicker;

	wait_for_entry(g);
	drink(g);

	recordWaitingTime(entryTicker - initial_time);
	entryTicker++;

	unlock();
}

// Lock held.
void change_well_endianness() {
	if (bigs == 0){
		Well->endianness = LITTLE;
		return;
	}
	if (littles == 0){
		Well->endianness = BIG;
		return;
	}

	if (Well->endianness == BIG) {
		Well->endianness = LITTLE;
	}
	else {
		Well->endianness = BIG;
	}
}

void broadcast(){
	//printf("Broadcasting. Bigs = %d", bigs);
	//printf(", Littles = %d\n", littles);
	if (bigs == 0) {
		uthread_cond_broadcast(Well->little);
		return;
	}
	if (littles == 0) {
		uthread_cond_broadcast(Well->big);
		return;
	}
	if (Well->endianness == BIG) {
		uthread_cond_broadcast(Well->big);
	}
	else {
		uthread_cond_broadcast(Well->little);
	}
}

void signal() {
	if (bigs == 0) {
		uthread_cond_signal(Well->little);
		return;
	}
	if (littles == 0) {
		uthread_cond_signal(Well->big);
		return;
	}
	if (Well->endianness == BIG) {
		uthread_cond_signal(Well->big);
	}
	else {
		uthread_cond_signal(Well->little);
	}
}

// Lock held.
void zero_occupancy_policy() {
	if (Well->fair_wait_counter == FAIR_WAITING_COUNT) {
		change_well_endianness();
		// reset fairness
		Well->fair_wait_counter = 0;
		broadcast();
	}
}

//Lock held.
void nonzero_occupancy_policy() {
	for (int i = 0; i < MAX_OCCUPANCY - Well->occupancy; i++) {
		broadcast();
	}
}

// Lock held.
void signal_the_next() {
	if (Well->occupancy == 0) {
		zero_occupancy_policy();
	}
	else
	{
		nonzero_occupancy_policy();
	}
}



void leaveWell() {
	lock();
	Well->occupancy--;
	signal_the_next();
	unlock();
}

void decrement_drinker_count(enum Endianness g, int i) {
	if (i == NUM_ITERATIONS - 1) {
		lock();
		if (g == BIG) {
			bigs--;
			//printf("A big is done drinking 100x\n");
		}
		else {
			littles--;
			//printf("A little is done drinking 100x\n");
		}
		unlock();
	}
}

void drinker(enum Endianness g) {
	for (int i = 0; i < NUM_ITERATIONS; i++) {
		enterWell(g);
		decrement_drinker_count(g, i);
		for (int j = 0; j < NUM_PEOPLE; j++) {
			uthread_yield();
		}
		leaveWell();
		for (int j = 0; j < NUM_PEOPLE; j++) {
			uthread_yield();
		}
	}
}

void* big_endian_drinker(void* arg) {
	drinker(BIG);
	return NULL;
}

void* little_endian_drinker(void* arg) {
	drinker(LITTLE);
	return NULL;
}


int main(int argc, char** argv) {
	uthread_init(1);
	Well = createWell();
	uthread_t pt[NUM_PEOPLE];
	waitingHistogrammutex = uthread_mutex_create();

	srand(time(NULL));

	// Start the threads, half big half little
	for (int i = 0; i < NUM_PEOPLE; i++)
	{
		int random = rand();
		//printf("%d\n", random);
		if (random % 2 == 0) {
			bigs++;
			pt[i] = uthread_create(big_endian_drinker, NULL);
		}
		else {
			littles++;
			pt[i] = uthread_create(little_endian_drinker, NULL);
		}
	}
	//printf("There are %d many bigs\n", bigs);
	//printf("There are %d many littles\n", littles);


	for (int i = 0; i < NUM_PEOPLE; i++) {
		uthread_join(pt[i], NULL);
	}

	printf("Times with 1 little endian %d\n", occupancyHistogram[LITTLE][1]);
	printf("Times with 2 little endian %d\n", occupancyHistogram[LITTLE][2]);
	printf("Times with 3 little endian %d\n", occupancyHistogram[LITTLE][3]);
	printf("Times with 1 big endian    %d\n", occupancyHistogram[BIG][1]);
	printf("Times with 2 big endian    %d\n", occupancyHistogram[BIG][2]);
	printf("Times with 3 big endian    %d\n", occupancyHistogram[BIG][3]);
	printf("Waiting Histogram\n");
	for (int i = 0; i < WAITING_HISTOGRAM_SIZE; i++)
		if (waitingHistogram[i])
			printf("  Number of times people waited for %d %s to enter: %d\n", i, i == 1 ? "person" : "people", waitingHistogram[i]);
	if (waitingHistogramOverflow)
		printf("  Number of times people waited more than %d entries: %d\n", WAITING_HISTOGRAM_SIZE, waitingHistogramOverflow);
}
