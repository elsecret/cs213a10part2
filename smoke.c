#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include "uthread.h"
#include "uthread_mutex_cond.h"

#define NUM_ITERATIONS 1000

#ifdef VERBOSE
#define VERBOSE_PRINT(S, ...) printf (S, ##__VA_ARGS__);
#else
#define VERBOSE_PRINT(S, ...) ;
#endif

struct Agent {
	uthread_mutex_t mutex;
	uthread_cond_t  match;
	uthread_cond_t  paper;
	uthread_cond_t  tobacco;
	uthread_cond_t  smoke;
};

struct Agent* createAgent() {
	struct Agent* agent = malloc(sizeof(struct Agent));
	agent->mutex = uthread_mutex_create();
	agent->paper = uthread_cond_create(agent->mutex);
	agent->match = uthread_cond_create(agent->mutex);
	agent->tobacco = uthread_cond_create(agent->mutex);
	agent->smoke = uthread_cond_create(agent->mutex);
	return agent;
}

//
// TODO
// You will probably need to add some procedures and struct etc.
//

/**
 * You might find these declarations helpful.
 *   Note that Resource enum had values 1, 2 and 4 so you can combine resources;
 *   e.g., having a MATCH and PAPER is the value MATCH | PAPER == 1 | 2 == 3
 */
enum Resource { MATCH = 1, PAPER = 2, TOBACCO = 4 };
char* resource_name[] = { "", "match",   "paper", "", "tobacco" };

int signal_count[5];  // # of times resource signalled
int smoke_count[5];  // # of times smoker with resource smoked

/**
 * This is the agent procedure.  It is complete and you shouldn't change it in
 * any material way.  You can re-write it if you like, but be sure that all it does
 * is choose 2 random reasources, signal their condition variables, and then wait
 * wait for a smoker to smoke.
 */
void* agent(void* av) {
	struct Agent* a = av;
	static const int choices[] = { MATCH | PAPER, MATCH | TOBACCO, PAPER | TOBACCO };
	static const int matching_smoker[] = { TOBACCO,     PAPER,         MATCH };

	uthread_mutex_lock(a->mutex);
	for (int i = 0; i < NUM_ITERATIONS; i++) {
		int r = random() % 3;
		signal_count[matching_smoker[r]] ++;
		int c = choices[r];
		if (c & MATCH) {
			VERBOSE_PRINT("match available\n");
			uthread_cond_signal(a->match);
		}
		if (c & PAPER) {
			VERBOSE_PRINT("paper available\n");
			uthread_cond_signal(a->paper);
		}
		if (c & TOBACCO) {
			VERBOSE_PRINT("tobacco available\n");
			uthread_cond_signal(a->tobacco);
		}
		VERBOSE_PRINT("agent is waiting for smoker to smoke\n");
		uthread_cond_wait(a->smoke);
	}
	uthread_mutex_unlock(a->mutex);
	return NULL;
}

void lock(struct Agent * agent) {
	uthread_mutex_lock(agent->mutex);
}

void unlock(struct Agent * agent) {
	uthread_mutex_unlock(agent->mutex);
}

uthread_cond_t t_cond;
uthread_cond_t p_cond;
uthread_cond_t m_cond;
uthread_cond_t kira_yamato;

// used by helpers and the ultimate coordinator kira yamato
int resource[] = { 0, 0, 0, 0, 0 };

uthread_cond_t get_smoker_cond(int resource) {
	switch (resource) {
	case TOBACCO:
		return t_cond;
	case PAPER:
		return p_cond;
	case MATCH:
		return m_cond;
	}
}

void debug_smoker_smoked(int smoker) {
	switch (smoker) {
	case TOBACCO:
		printf("The tobacco smoker has smoked.\n");
		return;
	case PAPER:
		printf("The paper smoker has smoked.\n");
		return;
	case MATCH:
		printf("The match smoker has smoked.\n");
		return;
	}
}

// whole thing happens while holding agent's mutex
void smoke(int resource, struct Agent * a) {
	//debug_smoker_smoked(resource);
	smoke_count[resource]++;
	uthread_cond_signal(a->smoke);
}

void smoker(int resource, struct Agent * a) {
	lock(a);
	while (1) {
		uthread_cond_wait(get_smoker_cond(resource));
		smoke(resource, a);
	}
	unlock(a);
}

void * tobacco(void * agent) {
	smoker(TOBACCO, (struct Agent*) agent);
	return NULL;
}

void * match(void * agent) {
	smoker(MATCH, (struct Agent*) agent);
	return NULL;
}

void * paper(void * agent) {
	smoker(PAPER, (struct Agent*) agent);
	return NULL;
}

// Returns the other required resource
int r2(int resource) {
	switch (resource) {
	case TOBACCO:
		return MATCH;
	case PAPER:
		return TOBACCO;
	case MATCH:
		return PAPER;
	}
}

// Returns one of the required resources
int r1(int resource) {
	switch (resource) {
	case TOBACCO:
		return PAPER;
	case PAPER:
		return MATCH;
	case MATCH:
		return TOBACCO;
	}
}

uthread_cond_t get_resource_cond(int resource, struct Agent * a) {
	switch (resource) {
	case TOBACCO:
		return a->tobacco;
	case PAPER:
		return a->paper;
	case MATCH:
		return a->match;
	}
}


void helper(int r, struct Agent * a) {
	uthread_cond_t c = get_resource_cond(r, a);
	lock(a);
	while (1) {
		uthread_cond_wait(c);
		resource[r] = 1;
		uthread_cond_signal(kira_yamato);
	}
	unlock(a);
}

void * tobacco_helper(void * agent) {
	helper(TOBACCO, (struct Agent *) agent);
	return NULL;
}

void * match_helper(void * agent) {
	helper(MATCH, (struct Agent *) agent);
	return NULL;
}

void * paper_helper(void * agent) {
	helper(PAPER, (struct Agent *) agent);
	return NULL;
}

void SEED_mode(){
	if (resource[TOBACCO] == 1 && resource[PAPER] == 1){
		resource[TOBACCO] = 0;
		resource[PAPER] = 0;
		uthread_cond_signal(get_smoker_cond(MATCH));
	} else if (resource[PAPER] == 1 && resource[MATCH] == 1) {
		resource[PAPER] = 0;
		resource[MATCH] = 0;
		uthread_cond_signal(get_smoker_cond(TOBACCO));
	} else {
		resource[TOBACCO] = 0;
		resource[MATCH] = 0;
		uthread_cond_signal(get_smoker_cond(PAPER));
	}
}

void* ultimate_coordinator(void * a) {
	lock(a);
	while (1) {
		while (resource[TOBACCO] + resource[MATCH] + resource[PAPER] < 2) {
			uthread_cond_wait(kira_yamato);
		}
		// Got enough resources to signal a smoker
		SEED_mode();
	}
	unlock(a);
	return NULL;
}

int main(int argc, char** argv) {
	uthread_init(8);
	struct Agent*  a = createAgent();

	t_cond = uthread_cond_create(a->mutex);
	p_cond = uthread_cond_create(a->mutex);
	m_cond = uthread_cond_create(a->mutex);
	kira_yamato = uthread_cond_create(a->mutex);

	uthread_create(tobacco_helper, a);
	uthread_create(paper_helper, a);
	uthread_create(match_helper, a);

	uthread_create(ultimate_coordinator, a);

	uthread_create(tobacco, a);
	uthread_create(paper, a);
	uthread_create(match, a);

	// TODO
	uthread_join(uthread_create(agent, a), 0);
	assert(signal_count[MATCH] == smoke_count[MATCH]);
	assert(signal_count[PAPER] == smoke_count[PAPER]);
	assert(signal_count[TOBACCO] == smoke_count[TOBACCO]);
	assert(smoke_count[MATCH] + smoke_count[PAPER] + smoke_count[TOBACCO] == NUM_ITERATIONS);
	printf("Smoke counts: %d matches, %d paper, %d tobacco\n",
		smoke_count[MATCH], smoke_count[PAPER], smoke_count[TOBACCO]);
}