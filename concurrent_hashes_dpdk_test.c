#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_random.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_timer.h>
#include <rte_malloc.h>

#include <time.h>
#include <unistd.h>
#include <assert.h>
#include "npf_conn_map.h"
#include "npf_city_hasher.h"
#include "thmap.h"

/*
 * Parameters
 */

#define WORKER_ITERATIONS 100000000
#define WORKER_CONNS 400000

/* range of number operation that are executed on each key */
#define OPI_MIN 0
#define OPI_MAX 30

/* connection ttl */
#define CONN_MAX_TTL 30 /* secs */

/* number of connections to iterate in one step */
#define CONN_STEP 100

#define NUM_WORKERS 4

/* number of new connections to generate per second */
#define NEW_CONN_PS 15000

#define TEST_TIME 30 /* seconds */

/* conndb storage type */
#define HASH_CUCKOO 1
#define HASH_TH 2
uint8_t g_hash_engine = HASH_TH;

void
timespec_diff(struct timespec *start, struct timespec *stop,
                   struct timespec *result)
{
    if ((stop->tv_nsec - start->tv_nsec) < 0) {
        result->tv_sec = stop->tv_sec - start->tv_sec - 1;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
    } else {
        result->tv_sec = stop->tv_sec - start->tv_sec;
        result->tv_nsec = stop->tv_nsec - start->tv_nsec;
    }

    return;
}

inline static uint64_t
get_time() {
	struct timespec tsnow;
	clock_gettime(CLOCK_MONOTONIC, &tsnow);
	uint64_t ret = (uint64_t) tsnow.tv_sec * 1000 + tsnow.tv_nsec / 1000000;
	return ret;
}

#define MAX_KEY_SIZE 1024

void
random_key(uint8_t *key, const uint16_t key_size)
{
	int i;

	for (i = 0; i < key_size; i++)
		key[i] = (uint8_t) rte_rand();
}

uint8_t*
init_keys(const uint16_t key_size, uint32_t num_keys)
{
	uint8_t *keys, *cur_key;
	int i;
	uint8_t key[MAX_KEY_SIZE];

	assert(key_size < MAX_KEY_SIZE);

	keys = rte_malloc(NULL, num_keys * key_size, RTE_CACHE_LINE_SIZE);
	if (keys == NULL)
		return NULL;

	cur_key = keys;

	/* fill keys with random values */
	for (i = 0; i < num_keys; i++,cur_key += key_size) {
		/* get a random key */
		random_key(key, key_size);
		/* check that key is unique */
		// todo
		/* put key to the global key store */
		rte_memcpy(cur_key, key, key_size);
	}

	return keys;
}

struct conn {
	/* index of a key in the global key store */
	uint32_t key_index;
	/* number of operations per iteration */
	uint32_t opi;
};

struct worker {
	struct rte_mempool *timers;
	uint16_t nb_conn_to_add;
	struct conn conns[WORKER_CONNS];
}
__rte_cache_aligned;

#define EMPTY_KEY_IND UINT32_MAX

/*
 * globals
 */
uint8_t *g_keys;
const uint32_t g_num_keys = 1000000;
const uint32_t g_key_size = 16; /* bytes */
struct worker g_workers[NUM_WORKERS];
void *g_cuckoo_conndb;
thmap_t *g_th_conndb;

rte_atomic64_t g_conndb_put_cnt;
rte_atomic64_t g_conndb_lookup_cnt;
rte_atomic64_t g_conndb_size;

void
npf_conn_map_simple_test(void)
{
	uint8_t key[g_key_size];
	uint64_t hv;
	bool res;
	void *con;

	memset(key, 0, g_key_size);
	key[0] = 1;
	key[1] = 2;
	hv = npf_city_hash(key, g_key_size);

	/* insert */
	res = npf_conn_map_insert(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key, hv,
			  (void *)(uintptr_t) 123);
	assert(res == true);

	/* lookup */
	con = npf_conn_map_lookup(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key,
			  hv);
	assert(con == (void *)(uintptr_t) 123);

	/* delete */
	npf_conn_map_remove(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key,
			  hv);
	con = npf_conn_map_lookup(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key,
			  hv);
	assert(con == NULL);

	printf("npf conn map simple test is OK\n");
}

/*
 * Init a worker
 */
void
worker_init(struct worker *worker)
{
	int i;
	char name[1024];

	/* timer pool */
	snprintf(name, sizeof(name), "lcore%u_timer_pool", rte_lcore_id());
	worker->timers = rte_mempool_create(
		name,
		WORKER_CONNS + 100,
		sizeof(struct rte_timer),
		256,
		0,
		NULL, NULL, NULL, NULL,
		0,
		MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET
	);
	if (worker->timers == NULL)
		rte_exit(EXIT_FAILURE, "lcore %u failed to create timer mempool",
				  rte_lcore_id());

	/* init connections */
	for (i = 0; i < WORKER_CONNS; i++)
		worker->conns[i].key_index = EMPTY_KEY_IND; /* empty */

	worker->nb_conn_to_add = 0;
}

/*
 * Returns:
 *		a new time on success
 *		NULL: on failure
 */
static struct rte_timer *
worker_timer_get(struct worker *worker)
{
	struct rte_timer *timer;
	int ret;

	ret = rte_mempool_get(worker->timers, (void **) &timer);
	if (unlikely(ret != 0))
		return NULL;

	rte_timer_init(timer);
	return timer;
}

/*
 *
 */
static inline void
worker_timer_put(struct worker *worker, struct rte_timer *timer)
{
   rte_mempool_put(worker->timers, timer);
}

/*
 * Indicate worker to add new connections
 */
static void
new_conn_timer_cb(struct rte_timer *unused, void *data)
{
	g_workers[rte_lcore_id()].nb_conn_to_add++;
}

void
cuckoo_lookup_insert(uint8_t *key)
{
	void *con;
	uint64_t hv;

	hv = npf_city_hash(key, g_key_size);
	con = npf_conn_map_lookup(g_cuckoo_conndb,
			  (const npf_connkey_ipv4_t *) key, hv);
	if (con == NULL) {
		npf_conn_map_insert(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key,
				  hv, (void *)(uintptr_t) 123);
		rte_atomic64_inc(&g_conndb_put_cnt);
		rte_atomic64_inc(&g_conndb_size);
	}
	else
		rte_atomic64_inc(&g_conndb_lookup_cnt);
}

void
thhash_lookup_insert(uint8_t *key)
{
	void *con;

	con = thmap_get(g_th_conndb, key, g_key_size);
	if (con == NULL) {
		thmap_put(g_th_conndb, key, g_key_size, (void *)(uintptr_t) 123);
		rte_atomic64_inc(&g_conndb_put_cnt);
		rte_atomic64_inc(&g_conndb_size);
	}
	else
		rte_atomic64_inc(&g_conndb_lookup_cnt);
}

void
cuckoo_delete(uint8_t *key)
{
	uint64_t hv;

	hv = npf_city_hash(key, g_key_size);
	npf_conn_map_remove(g_cuckoo_conndb, (const npf_connkey_ipv4_t *) key,
			  hv);
	rte_atomic64_dec(&g_conndb_size);
}

void
th_delete(uint8_t *key)
{
	if (thmap_del(g_th_conndb, key, g_key_size) != NULL)
		rte_atomic64_dec(&g_conndb_size);
}


/*
 * Delete a worker connection
 * Just mark its index as EMPTY_KEY_IND
 */
static void
conn_ttl_timer_cb(struct rte_timer *timer, void *data)
{
	struct worker *worker;
	uint32_t conn_ind;
	uint8_t *key;

	worker = &g_workers[rte_lcore_id()];
	conn_ind = (uint32_t)(uintptr_t) data;
	assert(conn_ind < WORKER_CONNS);

	/* delete connection key from conndb */
	key = &g_keys[worker->conns[conn_ind].key_index * g_key_size];

	if (g_hash_engine == HASH_CUCKOO)
		cuckoo_delete(key);
	else
		th_delete(key);

	/* mark connection slot as empty,
	 * stop and free timer
	 */
	worker->conns[conn_ind].key_index = EMPTY_KEY_IND;
	rte_timer_stop(timer);
	worker_timer_put(worker, timer);
	// printf("lcore %u deleted connection %u\n", rte_lcore_id(), conn_ind);
}

/*
 * worker main routine
 */
static int
worker(__attribute__((unused)) void *dummy) {
	uint64_t iter;
	int i,j;
	struct rte_timer *new_conn_timer, *conn_ttl_timer;
	uint32_t conn_ind;
	struct conn *conns;
	struct worker *worker;
	uint64_t tics;
	uint32_t ttl;
	uint8_t *key;
	uint32_t print_i;
	uint64_t tm1, tm2;

	printf("lcore %u started\n", rte_lcore_id());

	worker = &g_workers[rte_lcore_id()];
	worker_init(worker);

	/* start new connection timer */
	new_conn_timer = worker_timer_get(worker);
	if (new_conn_timer == NULL)
		rte_exit(EXIT_FAILURE, "no timers left");

	tics = rte_get_timer_hz() / NEW_CONN_PS;
	rte_timer_reset(new_conn_timer, tics, PERIODICAL, rte_lcore_id(),
			  new_conn_timer_cb, NULL);

	/* start from the first connection */
	conns = worker->conns;
	conn_ind = 0;
	print_i = 0;

	tm1 = get_time();

	/* worker loop */
	for (iter = 0; iter <= WORKER_ITERATIONS; iter++) {
		tm2 = get_time();
		if (tm2 - tm1 > (uint64_t) TEST_TIME * MS_PER_S)
			break;

		/* iterate next N connections and execute conndb operation on them */
		i = 0;
		while (true) {
			/* run timers */
			rte_timer_manage();

			if (g_hash_engine == HASH_TH)
				thmap_gc(g_th_conndb, thmap_stage_gc(g_th_conndb));

			if (conns[conn_ind].key_index == EMPTY_KEY_IND) {
				if (worker->nb_conn_to_add > 0) {
					/* add new connection */
					conns[conn_ind].key_index = rte_rand() % g_num_keys;
					conns[conn_ind].opi = OPI_MIN + (rte_rand() % (OPI_MAX - OPI_MIN));
					/* start the Time To Live timer for a new connection */
					conn_ttl_timer = worker_timer_get(worker);
					if (conn_ttl_timer == NULL)
						rte_exit(EXIT_FAILURE, "no timers left");
					ttl = 1 + rte_rand() % (CONN_MAX_TTL - 1);
					tics = rte_get_timer_hz() * ttl;
					rte_timer_reset(conn_ttl_timer, tics, SINGLE, rte_lcore_id(),
							  conn_ttl_timer_cb, (void *)(uintptr_t) conn_ind);

					/* connection has been added */
					worker->nb_conn_to_add--;
				}
			}
			else {
				/* execute conndb operations with the connection key */
				for (j = 0; j < conns[conn_ind].opi; j++) {
					/* lookup the connection key OPI times.
					 * if lookup miss add the connection key
					 */
					key = &g_keys[conns[conn_ind].key_index * g_key_size];
					if (g_hash_engine == HASH_CUCKOO)
						cuckoo_lookup_insert(key);
					else
						thhash_lookup_insert(key);
				}
			}

			i++;
			if (i == CONN_STEP && worker->nb_conn_to_add == 0)
				/* step is completed and no need to add new connections */
				break;
			/* else
			 *		continue unnless we have added all new connections  */

			if (i == WORKER_CONNS)
				/* all worker connections slots has been checked, no empty slot */
				break;

			/* start from the first connection */
			if (++conn_ind == WORKER_CONNS)
				conn_ind = 0;
		}
	}

	printf("lcore %u finished: time %lu ms\n", rte_lcore_id(), tm2 - tm1);
}

/*
 *
 */
int
main(int argc, char **argv)
{
	int ret;
	unsigned lcore_id;

	printf("sizeof npf_connkey_ipv4 %zu\n", sizeof(struct npf_connkey_ipv4));

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
	argc -= ret;
	argv += ret;

	rte_srand(rte_rdtsc());

	g_keys = init_keys(g_key_size, g_num_keys);
	if (g_keys == NULL)
		rte_exit(EXIT_FAILURE, "failure to init keys");
	printf("keys initialization done\n");

	if (g_hash_engine == HASH_CUCKOO) {
		g_cuckoo_conndb = npf_conn_map_init();
		assert(g_cuckoo_conndb != NULL);
		printf("cuckoo npf_conn_map initialization done\n");
	}
	else {
		g_th_conndb = thmap_create(0, NULL, 0);
		assert(g_th_conndb != NULL);
		printf("thmap npf_conn_map initialization done\n");
	}

	if (g_hash_engine == HASH_CUCKOO)
		npf_conn_map_simple_test();

	rte_atomic64_init(&g_conndb_put_cnt);
	rte_atomic64_init(&g_conndb_lookup_cnt);
	rte_atomic64_init(&g_conndb_size);

	rte_eal_mp_remote_launch(worker, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id)
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;

	if (g_hash_engine == HASH_CUCKOO)
		printf("conndb size %lu, put %lu, get %lu\n",
				  npf_conn_map_size(g_cuckoo_conndb),
				  rte_atomic64_read(&g_conndb_put_cnt),
				  rte_atomic64_read(&g_conndb_lookup_cnt));
	else
		printf("conndb size %lu, put %lu, get %lu\n",
				  rte_atomic64_read(&g_conndb_size),
				  rte_atomic64_read(&g_conndb_put_cnt),
				  rte_atomic64_read(&g_conndb_lookup_cnt));
}
