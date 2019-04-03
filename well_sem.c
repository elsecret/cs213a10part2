#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include "uthread.h"
#include "uthread_sem.h"
#include <time.h>

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
	enum Endianness endianness;
	uthread_sem_t mx;
	uthread_sem_t big;
	uthread_sem_t little;
	int bigs_incoming;
	int littles_incoming;
	int occupancy;
	int fair_count;
	int is_new;
};

struct Well* createWell() {
	struct Well* Well = malloc(sizeof(struct Well));
	Well->mx = uthread_sem_create(1);
	Well->big = uthread_sem_create(0);
	Well->little = uthread_sem_create(0);
	Well->bigs_incoming = 0;
	Well->littles_incoming = 0;
	Well->occupancy = 0;
	Well->fair_count = 0;
	Well->is_new = 1;
	return Well;
}

struct Well* Well;

#define WAITING_HISTOGRAM_SIZE (NUM_ITERATIONS * NUM_PEOPLE)
int             entryTicker;                                          // incremented with each entry
int             waitingHistogram[WAITING_HISTOGRAM_SIZE];
int             waitingHistogramOverflow;
uthread_sem_t   waitingHistogramMutex;
int             occupancyHistogram[2][MAX_OCCUPANCY + 1];


void lock() {
	uthread_sem_wait(Well->mx);
}

void unlock() {
	uthread_sem_signal(Well->mx);
}

void recordWaitingTime(int waitingTime) {
	uthread_sem_wait(waitingHistogramMutex);
	if (waitingTime < WAITING_HISTOGRAM_SIZE)
		waitingHistogram[waitingTime] ++;
	else
		waitingHistogramOverflow++;
	// update occupancyHistogram
	occupancyHistogram[Well->endianness][Well->occupancy]++;
	uthread_sem_signal(waitingHistogramMutex);
}

// LOCKED
void signal() {
	if (Well->endianness == BIG) {
		Well->bigs_incoming++;
		uthread_sem_signal(Well->big);
	}
	else {
		Well->littles_incoming++;
		uthread_sem_signal(Well->little);
	}
}

void wait(enum Endianness g) {
	if (g == BIG) {
		uthread_sem_wait(Well->big);
	}
	else {
		uthread_sem_wait(Well->little);
	}
}

void decrement_incoming_count(enum Endianness g) {
	if (g == BIG) {
		Well->bigs_incoming--;
	}
	else {
		Well->littles_incoming--;
	}
}

// LOCKED
void wait_to_drink(enum Endianness g) {
	// If the well is fresh, then it's free to go in
	if (Well->is_new) {
		Well->is_new = 0;
		Well->endianness = g;
		// signal others of the same endianness. like, enough so that it becomes full
		for (int i = 0; i < MAX_OCCUPANCY - 1; i++) {
			signal();
		}
		return;
	}

	unlock();
	wait(g);
	lock();
	decrement_incoming_count(g);
}

void drink(enum Endianness g) {
	Well->occupancy++;
	Well->fair_count++;
	Well->endianness = g;
}

void enterWell(enum Endianness g) {
	lock();
	int initial_time = entryTicker;
	wait_to_drink(g);
	drink(g);
	recordWaitingTime(entryTicker - initial_time);
	entryTicker++;
	unlock();
}

void attempt_to_signal_bigs() {
	while (Well->occupancy + Well->bigs_incoming < MAX_OCCUPANCY) {
		Well->bigs_incoming++;
		uthread_sem_signal(Well->big);
	}
}

void attempt_to_signal_littles() {
	while (Well->occupancy + Well->littles_incoming < MAX_OCCUPANCY) {
		Well->littles_incoming++;
		uthread_sem_signal(Well->little);
	}
}

void max_fair_wait_policy() {
	// In the general case, we flip the endianness of the well,
	// then signal MAX_OCCUPANCY amount
	// then reset the fair wait
	// however...
	// it's possible that one of the populations is done

	// actually just reset it first



	if (bigs == 0) {
		Well->fair_count = 0;
		attempt_to_signal_littles();
		return;
	}
	if (littles == 0) {
		Well->fair_count = 0;
		attempt_to_signal_bigs();
		return;
	}

	// check the occupancy
	if (Well->occupancy == 0) {
		// flip endianness of well
		Well->fair_count = 0;
		Well->endianness = Well->endianness == BIG ? LITTLE : BIG;

		if (Well->endianness = BIG) {
			attempt_to_signal_bigs();
		}
		else {
			attempt_to_signal_littles();
		}
	}
	else {
		// do nothing
		return;
	}
}

void nonmax_fair_wait_policy() {
	// check if we're done one population
	if (bigs == 0){
		attempt_to_signal_littles();
		return;
	}
	if (littles == 0){
		attempt_to_signal_bigs();
		return;
	}

	// check if it is OK to send a signal
	int its_OK = Well->occupancy + Well->bigs_incoming + Well->littles_incoming < MAX_OCCUPANCY;
	its_OK = its_OK && Well->fair_count + Well->bigs_incoming + Well->littles_incoming < FAIR_WAITING_COUNT;

	if (its_OK) {
		signal();
	}
}

void leaveWell() {
	lock();
	Well->occupancy--;

	// Fair count is either maxed or it's not
	if (Well->fair_count == FAIR_WAITING_COUNT) {
		max_fair_wait_policy();
	}
	else
	{
		nonmax_fair_wait_policy();
	}
	unlock();
	return;
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
	waitingHistogramMutex = uthread_sem_create(1);

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
