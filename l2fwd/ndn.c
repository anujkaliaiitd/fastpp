#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "city.h"
#include "ndn.h"
#include "util.h"

/**< Create a mutable prefix from a URL */
char *ndn_get_prefix(const char *url, int len)
{
	char *prefix = malloc(len + 1);
	memcpy(prefix, url, len);
	prefix[len] = 0;
	return prefix;
}

/**< Check if a prefix exists in the NDN hash table. If it does:
  *  If this is non-terminal, then downgrade existing prefix to non-terminal.
  *  If this is terminal, then override the dst port of existing prefix.
  *  This is OK because terminal prefixes should take priority over
  *  non-terminal, and the same terminal prefix should *not* be inserted twice.
  *
  *  DO NOT USE THIS FUNCTION TO SIMPLY CHECK IF A PREFIX IS PRESENT.
  *
  *  Returns 1 if the prefix was found, 0 otherwise. */
int ndn_contains(const char *prefix, int len,
	int is_terminal, int dst_port, struct ndn_bucket *ht)
{
	/**< A prefix ends with a '/', so it contains at least 2 characters */
	assert(len >= 2);
	assert(prefix[len - 1] == '/');

	int i;
	int bkt_num, bkt_1, bkt_2;

	uint64_t prefix_hash = CityHash64(prefix, len);
	uint16_t tag = prefix_hash >> 48;

	struct ndn_slot *slots;

	/**< Test the two candidate buckets */
	for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {

		/**< Get the slot array for this bucket */
		if(bkt_num == 1) {
			bkt_1 = prefix_hash & NDN_NUM_BKT_;
			slots = ht[bkt_1].slots;
		} else {
			bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
			slots = ht[bkt_2].slots;
		}

		/**< Now, "slot" points to an ndn_bucket. Find a valid slot that
		  *  contains the same hash. */
		for(i = 0; i < NDN_NUM_SLOTS; i ++) {
			/**< Underscored variables are per-slot variables */
			int8_t _dst_port = slots[i].dst_port;
			uint64_t _hash = slots[i].cityhash;

			if(_dst_port >= 0 && _hash == prefix_hash) {
				/**< Should we downgrade this prefix to "non-terminal" ? */
				if(is_terminal == 0) {
					slots[i].is_terminal = 0;
				} else {
					slots[i].dst_port = dst_port;
				}

				return 1;
			}
		}
	}

	return 0;
}

/**< Insert a prefix into the NDN hash table. 
  *  Returns 0 on success and -1 on failure. */
int ndn_ht_insert(const char *prefix, int len, 
	int is_terminal, int dst_port, struct ndn_bucket *ht) 
{
	/**< A prefix ends with a '/', so it contains at least 2 characters */
	assert(len >= 2 && len <= NDN_MAX_NAME_LENGTH && len < 256);
	assert(prefix[len - 1] == '/');

	assert(is_terminal == 0 || is_terminal == 1);
	assert(dst_port < NDN_MAX_ETHPORTS);

	if(ndn_contains(prefix, len, is_terminal, dst_port, ht)) {
		return 0;
	}

	int i;
	int bkt_num, bkt_1, bkt_2;

	uint64_t prefix_hash = CityHash64(prefix, len);
	uint16_t tag = prefix_hash >> 48;

	struct ndn_slot *slots;

	/**< Check if the two candidate buckets contain an empty slot. */
	for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {

		/**< Get the slot array for this bucket */
		if(bkt_num == 1) {
			bkt_1 = prefix_hash & NDN_NUM_BKT_;
			slots = ht[bkt_1].slots;
		} else {
			bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
			slots = ht[bkt_2].slots;
		}

		/**< Now, "slot" points to an ndn_bucket */
		for(i = 0; i < NDN_NUM_SLOTS; i ++) {

			if(slots[i].dst_port == -1) {

				/**< Initialize the slot */
				slots[i].dst_port = dst_port;
				slots[i].is_terminal = is_terminal;
				slots[i].cityhash = prefix_hash;

				return 0;
			}
		}
	}

	/**< We do not perform cuckoo evictions: each key has 16 (8x2) candidate
	  *  slots which should be enough. */
	printf("\tndn: Unable to insert prefix: %s\n", prefix);
	return -1;
}

/**< Try to find a match for the 1st component of this trace in the hash
  *  table. This function gets called when the clever matching trick of
  *  starting from 2nd component matches fails.
  *
  *  This is a specialized function that works for fixed length traces of
  *  length NDN_TRACE_LEN. The corresponding function in the CPU-only NDN
  *  implementation works for null-terminatd traces. */
int lookup_one_component_gpu_only(char *trace, struct ndn_bucket *ht)
{
	int c_i, i;	/**< URL char iterator and slot iterator */
	int bkt_num, bkt_1, bkt_2;

	for(c_i = 0; c_i < NDN_TRACE_LEN; c_i ++) {
		if(trace[c_i] == '/') {
			break;
		}
	}

	/**< c_i is now at the boundary of the 1st component */
	uint64_t prefix_hash = CityHash64(trace, c_i + 1);
	uint16_t tag = prefix_hash >> 48;

	struct ndn_slot *slots;

	/**< trace[0] -> trace[c_i] is a prefix of length c_i + 1 */
	for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {
		if(bkt_num == 1) {
			bkt_1 = prefix_hash & NDN_NUM_BKT_;
			slots = ht[bkt_1].slots;
		} else {
			bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
			slots = ht[bkt_2].slots;
		}

		/**< Now, "slots" points to an ndn_bucket. Find a valid slot
		  *  that contains the same hash. */
		for(i = 0; i < NDN_NUM_SLOTS; i ++) {
			int8_t _dst_port = slots[i].dst_port;
			uint64_t _hash = slots[i].cityhash;

			if(_dst_port >= 0 && _hash == prefix_hash) {

				/**< As we're only matching this component, we're done! */
				return slots[i].dst_port;
			}
		}
	}

	/**< No match even for the 1st component? */
	return -1;
}

/**< A specialized NDN lookup function that works for fixed-size traces of
  *  length NDN_TRACE_LEN bytes. The general function works for
  *  null-terminated strings. */
int ndn_lookup_gpu_only(struct ndn_trace *t, struct ndn_bucket *ht)
{
	char *trace = t->bytes;

	int fwd_port = -1;
	int c_i, i;	/**< URL char iterator and slot iterator */
	int bkt_num, bkt_1 = 0, bkt_2;

	int terminate = 0;			/**< Stop processing this URL? */
	int prefix_match_found = 0;	/**< Stop this hash-table lookup ? */

	/**< Completely ignore 1-component matches */		
	for(c_i = 0; c_i < NDN_TRACE_LEN; c_i ++) {
		if(trace[c_i] == '/') {
			break;
		}
	}
	c_i ++;

	for(; c_i < NDN_TRACE_LEN; c_i ++) {
		if(trace[c_i] != '/') {
			continue;
		}

		/**< c_i is now at the boundary of a component longer than the 1st */
		uint64_t prefix_hash = CityHash64(trace, c_i + 1);
		uint16_t tag = prefix_hash >> 48;

		struct ndn_slot *slots;

		/**< trace[0] -> trace[c_i] is a prefix of length c_i + 1 */
		for(bkt_num = 1; bkt_num <= 2; bkt_num ++) {
			if(bkt_num == 1) {
				bkt_1 = prefix_hash & NDN_NUM_BKT_;
				slots = ht[bkt_1].slots;
			} else {
				bkt_2 = (bkt_1 ^ CityHash64((char *) &tag, 2)) & NDN_NUM_BKT_;
				slots = ht[bkt_2].slots;
			}

			/**< Now, "slots" points to an ndn_bucket. Find a valid slot
			  *  that contains the same hash. */
			for(i = 0; i < NDN_NUM_SLOTS; i ++) {
				int8_t _dst_port = slots[i].dst_port;
				uint64_t _hash = slots[i].cityhash;

				if(_dst_port >= 0 && _hash == prefix_hash) {

					/**< Record the dst port: this may get overwritten by
					  *  longer prefix matches later */
					fwd_port = slots[i].dst_port;

					if(slots[i].is_terminal == 1) {
						/**< A terminal FIB entry: we're done! */
						terminate = 1;
					}

					prefix_match_found = 1;
					break;
				}
			}

			/**< Stop the hash-table lookup for trace[0 ... c_i] */
			if(prefix_match_found == 1) {
				break;
			}
		}

		/**< Stop processing the trace if we found a terminal FIB entry */
		if(terminate == 1) {
			break;
		}
	}	/**< Loop over trace characters ends here */

	/**< We failed to match with prefixes that contain 2 or more
	  *  components. Try matching the 1st component of this trace now */
	if(fwd_port == -1) {
		fwd_port = lookup_one_component_gpu_only(trace, ht);
	}

	return fwd_port;
}

void ndn_init(const char *urls_file, int portmask, struct ndn_bucket **ht)
{
	int i, j, nb_urls = 0;
	char url[NDN_MAX_NAME_LENGTH] = {0};
	int shm_flags = IPC_CREAT | 0666 | SHM_HUGETLB;

	int index_size = (int) (NDN_NUM_BKT * sizeof(struct ndn_bucket));

	int num_active_ports = bitcount(portmask);
	int *port_arr = get_active_bits(portmask);

	/**< Allocate the hash index */
	red_printf("ndn: Init NDN index of size = %lu bytes\n", index_size);
	int index_sid = shmget(NDN_HT_INDEX_KEY, index_size, shm_flags);
	assert(index_sid >= 0);
	*ht = shmat(index_sid, 0, 0);
	memset((char *) *ht, 0, index_size);

	/**< Set dst_port to -1 to invalidate all slots*/
	for(i = 0; i < NDN_NUM_BKT; i ++) {
		struct ndn_slot *slots = ((*ht)[i]).slots;
		for(j = 0; j < NDN_NUM_SLOTS; j ++) {
			slots[j].dst_port = -1;
		}
	}

	red_printf("ndn: Reading URLs from file %s\n", urls_file);
	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);
	int nb_fail = 0;

	while(1) {

		/**< Read a new URL from the file and check if its valid */
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}

		int url_len = strlen(url);
		assert(url[url_len - 1] == '/' && url[url_len] == 0);
		assert(url_len < NDN_MAX_NAME_LENGTH - 3);	/**< Plenty of headroom */

		/**< The destination port for all prefixes from this URL */
		int dst_port = port_arr[rand() % num_active_ports];

		#if NDN_DEBUG == 1
		printf("ndn: Inserting FIB entry: URL %s -> port %d\n", url, dst_port);
		#endif

		/**< Is this prefix terminal? */
		int is_terminal;

		int i;
		for(i = 0; i < url_len; i ++) {
			/**< Testing url[i + 1] is OK because of headroom after url_len */

			if(url[i] == '/' && url[i + 1] != 0) {
				/**< Non-terminal prefixes */
				is_terminal = 0;
				if(ndn_ht_insert(url, i + 1, is_terminal, dst_port, *ht) != 0) {
					nb_fail ++;
				}
			} else if(url[i] == '/' && url[i + 1] == 0) {
				/**< Terminal prefix. All inserted prefixes end with '/' */
				is_terminal = 1;
				if(ndn_ht_insert(url, i + 1, is_terminal, dst_port, *ht) != 0) {
					nb_fail ++;
				}
				break;
			}
		}
		
		memset(url, 0, NDN_MAX_NAME_LENGTH * sizeof(char));
		nb_urls ++;

		if((nb_urls & K_512_) == 0) {
			printf("\tndn: Total urls = %d. Fails = %d\n", nb_urls, nb_fail);
		}
	}

	red_printf("ndn: Total urls = %d. Fails = %d.\n", nb_urls, nb_fail);

}

/**< Return the number of lines in a file */
int ndn_get_num_lines(const char *file_name)
{
	int nb_lines = 0;
	FILE *fp = fopen(file_name, "r");
	assert(fp != NULL);

	char line[NDN_MAX_LINE_LENGTH] = {0};

	while(1) {
		/**< Read a new URL from the file and check if its valid */
		fscanf(fp, "%s", line);
		if(line[0] == 0) {
			break;
		}

		/**< As we're only counting URLs, no need to zero out all bytes */
		line[0] = 0;
		nb_lines ++;
	}

	return nb_lines;
}


/**< Put all lookup names in a linear array with fixed sized slots */
struct ndn_name *ndn_get_name_array(const char *names_file)
{
	int i;
	int nb_names = ndn_get_num_lines(names_file);

	int shm_flags = IPC_CREAT | 0666 | SHM_HUGETLB;

	/**< XXX: On xia-router1, allocating non hugepage-aligned regions causes
	  *  segfault during memset. Temporary fix. */
	int alloc_size = 0, req_size = nb_names * sizeof(struct ndn_name);
	assert(req_size >= 1 && req_size < M_2048 - M_2);
	while(alloc_size < req_size) {
		alloc_size += M_2;
	}

	int name_sid = shmget(NDN_NAMES_KEY, alloc_size, shm_flags);
	assert(name_sid >= 0);
	
	struct ndn_name *name_arr = shmat(name_sid, 0, 0);
	assert(name_arr != NULL);

	memset(name_arr, 0, nb_names * sizeof(struct ndn_name));

	red_printf("ndn: Reading %d names from file %s\n", nb_names, names_file);
	FILE *name_fp = fopen(names_file, "r");
	assert(name_fp != NULL);

	int tot_len = 0;

	for(i = 0; i < nb_names; i ++) {
		char temp_name[NDN_MAX_NAME_LENGTH] = {0};
		fscanf(name_fp, "%s", temp_name);

		if(temp_name[0] == 0) {
			break;
		}
		assert(temp_name[NDN_MAX_NAME_LENGTH - 1] == 0);

		int len = strlen(temp_name);
		tot_len += len;

		/**< The file's names should end with a '/' */
		assert(temp_name[len - 1] == '/');
		memcpy(name_arr[i].bytes, temp_name, len);
		memset(temp_name, 0, NDN_MAX_NAME_LENGTH);
	}

	printf("\tndn: Average name length = %.2f\n",
		(double) tot_len / nb_names);

	/**< Shuffle */
	printf("\tndn: Shuffling names\n");
	struct ndn_name temp;
	for(i = 0; i < nb_names; i ++) {
		int t = rand() % (i + 1);
		temp = name_arr[i];
		name_arr[i] = name_arr[t];
		name_arr[t] = temp;
	}

	return name_arr;
}

/**< Print some useful stats for the URLs in this file */
void ndn_print_url_stats(const char *urls_file)
{
	int i;

	/**< Maximum number of components = 15 */
	int components_stats[NDN_MAX_NAME_LENGTH + 1] = {0};
	char url[NDN_MAX_NAME_LENGTH] = {0};

	FILE *url_fp = fopen(urls_file, "r");
	assert(url_fp != NULL);

	while(1) {
		fscanf(url_fp, "%s", url);
		if(url[0] == 0) {
			break;
		}
		assert(url[NDN_MAX_NAME_LENGTH - 1] == 0);

		int num_components = ndn_num_components(url);
		assert(num_components <= NDN_MAX_COMPONENTS);
		components_stats[num_components] ++;
	
		memset(url, 0, NDN_MAX_NAME_LENGTH);
	}

	red_printf("ndn: URL stats:\n");
	for(i = 0; i <= NDN_MAX_COMPONENTS; i ++) {
		printf("ndn: %d URLs have %d components\n", components_stats[i], i);
	}
}

/**< Count the number of components in this URL. Example of expected format:
  *  "com/google/" i.e. no beginning slash, yes trailing slash. */
inline int ndn_num_components(const char *url)
{
	int i, num_slash = 0;
	for(i = 0; i < NDN_MAX_NAME_LENGTH; i ++) {
		if(url[i] == '/') {
			num_slash ++;
		}

		if(url[i] == 0) {
			break;
		}
	}

	/**< Each component is ended by a slash */
	return num_slash;
}


