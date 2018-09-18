/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/*
 * File:   npf_conn_map.cpp
 * Author: alexk
 *
 * Created on June 6, 2016, 10:08 PM
 */

#include <stdio.h>
#include <stdint.h>
#include <rte_branch_prediction.h>

#include "cuckoo/cuckoohash_map.hh"
#include "city.h"
#include "npf_conn_map.h"

#define NPF_CONN_MAP_IPV4_SIZE 8192

class conn_hasher {
public:
    size_t operator()(const npf_connkey_ipv4_t& key) const {
        return CityHash64((const char*) &key.data[0], CONN_KEY_SIZE);
    }
};

typedef cuckoohash_map<npf_connkey_ipv4_t, void*, 
		  conn_hasher> con_map_t;

void* npf_conn_map_init(void) {
	con_map_t* map = new con_map_t(NPF_CONN_MAP_IPV4_SIZE);
	return (void*) map;
}

void npf_conn_map_fini(void* map) {
	con_map_t* cmap = (con_map_t*) map;
	delete cmap;
}

bool operator==(const npf_connkey_ipv4_t& ck1, const npf_connkey_ipv4_t& ck2)
{
	int ret = memcmp(&ck1.data[0], &ck2.data[0], 
			  CONN_KEY_SIZE);
	return ret == 0 ? true : false;
}

size_t npf_conn_map_hash(void* map, const npf_connkey_ipv4_t *key) 
{
	con_map_t* cmap = (con_map_t*) map;
	return cmap->key_hash(*key);
}

uint64_t npf_conn_map_size(void* map) 
{
	con_map_t* cmap = (con_map_t*) map;
	return cmap->size();
}

void *
npf_conn_map_lookup(void* map, const npf_connkey_ipv4_t *key, const size_t hv)
{
	con_map_t* cmap = (con_map_t*) map;
	void* con;
	
	if (!cmap->find(*key, con, hv)) {
		return NULL;
	}
	else {
		return con;
	}
}

/*
 * npf_conndb_insert: insert the key representing the connection.
 */
bool
npf_conn_map_insert(void *map, const npf_connkey_ipv4_t *key, const size_t hv, 
		  void *con)
{
	con_map_t* cmap = (con_map_t*) map;
	return cmap->insert(*key, hv, con);
}

/*
 * npf_conndb_remove: find and delete the key and return the connection
 * it represents.
 */
void*
npf_conn_map_remove(void *map, const npf_connkey_ipv4_t *key, const size_t hv)
{
	con_map_t* cmap = (con_map_t*) map;
	cmap->erase(*key, hv);
	
//	con_map_t* cmap = (con_map_t*) map;
//	void* l_con = NULL;
//	
//	auto fn = [&l_con](void*& con) {
//		l_con = con;
//		con = NULL;
//	};
//	
//	if (cmap->update_fn(*key, hv, fn)) {
//		cmap->erase(*key, hv);
//		return l_con;
//	}
			  
	return NULL;
}
