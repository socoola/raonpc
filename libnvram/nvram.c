#include <sys/types.h> /* for open */
#include <sys/stat.h> /* for open */
#include <fcntl.h>     /* for open */
#include <unistd.h>    /* for lseek and write */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nvram.h"

char libnvram_debug = 0;
#define LIBNV_PRINT(x, ...) do { if (libnvram_debug) printf("%s %d: " x, __FILE__, __LINE__, ## __VA_ARGS__); } while(0)
#define LIBNV_ERROR(x, ...) do { printf("%s %d: ERROR! " x, __FILE__, __LINE__, ## __VA_ARGS__); } while(0)
//x is the value returned if the check failed
#define LIBNV_CHECK_INDEX(x) do { \
	if (index < 0 || index >= FLASH_BLOCK_NUM) { \
		LIBNV_PRINT("index(%d) is out of range\n", index); \
		return x; \
	} \
} while (0)

#define LIBNV_CHECK_VALID() do { \
	if (!fb[index].valid) { \
		LIBNV_PRINT("fb[%d] invalid, init again\n", index); \
		nvram_init(index); \
	} \
} while (0)

#define FREE(x) do { if (x != NULL) {free(x); x=NULL;} } while(0)

static block_t fb[FLASH_BLOCK_NUM] =
{
#ifdef CONFIG_DUAL_IMAGE
	{
		.name = "uboot",
		.flash_offset =  0x0,
		.flash_max_len = ENV_UBOOT_SIZE,
		.valid = 0
	},
#endif
	{
		.name = "2860",
		.flash_offset =  0x2000,
		.flash_max_len = ENV_BLK_SIZE*4,
		.valid = 0
	},
	{
		.name = "rtdev",
		.flash_offset = 0x6000,
		.flash_max_len = ENV_BLK_SIZE*2,
		.valid = 0
	},
	{
		.name = "cert",
		.flash_offset = 0x8000,
		.flash_max_len = ENV_BLK_SIZE*2,
		.valid = 0
	},
	{
		.name = "wapi",
		.flash_offset = 0xa000,
		.flash_max_len = ENV_BLK_SIZE*5,
		.valid = 0
	}
};

/*
 * return idx (0 ~ iMAX_CACHE_ENTRY)
 * return -1 if no such value or empty cache
 */
static int cache_idx(int index, char *name)
{
	int i;

	for (i = 0; i < MAX_CACHE_ENTRY; i++) {
		if (!fb[index].cache[i].name)
			return -1;
		if (!strcmp(name, fb[index].cache[i].name))
			return i;
	}
	return -1;
}

/*
 * 1. read env from flash
 * 2. parse entries
 * 3. save the entries to cache
 */
void nvram_init(int index)
{
	unsigned long from;
	int i, len;
	char *p, *q;
	int fd;
	nvram_ioctl_t nvr;
	struct stat stats;
	char fname[64];

	LIBNV_PRINT("--> nvram_init %d\n", index);
	LIBNV_CHECK_INDEX();

	if (fb[index].valid)
		return;

	sprintf(fname, "%s/%s", NV_DEV, fb[index].name);
	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		perror(NV_DEV);
		goto out;
	}

	if(fstat(fd, &stats) < 0)
	{
		perror(NV_DEV);
		close(fd);
		goto out;
	}

	//read data from flash
	len = stats.st_size;
	fb[index].env.data = (char *)malloc(len);
	read(fd, fb[index].env.data, len);
	close(fd);

	//parse env to cache
	p = fb[index].env.data;
	for (i = 0; i < MAX_CACHE_ENTRY; i++) {
		if (NULL == (q = strchr(p, '='))) {
			LIBNV_PRINT("parsed failed - cannot find '='\n");
			break;
		}
		*q = '\0'; //strip '='
		fb[index].cache[i].name = strdup(p);
		//printf("  %d '%s'->", i, p);

		p = q + 1; //value
		if (NULL == (q = strchr(p, '\n'))) {
			LIBNV_PRINT("parsed failed - cannot find '\\0'\n");
			break;
		}
		fb[index].cache[i].value = strdup(p);
		//printf("'%s'\n", p);

		p = q + 1; //next entry
		if (p - fb[index].env.data + 1 >= len) //end of block
			break;
		if (*p == '\0') //end of env
			break;
	}
	if (i == MAX_CACHE_ENTRY)
		LIBNV_PRINT("run out of env cache, please increase MAX_CACHE_ENTRY\n");

	fb[index].valid = 1;
	fb[index].dirty = 0;

out:
	FREE(fb[index].env.data); //free it to save momery
}

void nvram_close(int index)
{
	int i;

	LIBNV_PRINT("--> nvram_close %d\n", index);
	LIBNV_CHECK_INDEX();

	if (!fb[index].valid)
		return;
	if (fb[index].dirty)
		nvram_commit(index);

	//free env
	FREE(fb[index].env.data);

	//free cache
	for (i = 0; i < MAX_CACHE_ENTRY; i++) {
		FREE(fb[index].cache[i].name);
		FREE(fb[index].cache[i].value);
	}

	fb[index].valid = 0;
}


const char *nvram_get(int index, char *name)
{
	//LIBNV_PRINT("--> nvram_get\n");

#ifdef CONFIG_KERNEL_NVRAM
	/* Get the fresh value from Kernel NVRAM moduel,
	 * so there is no need to do nvram_close() and nvram_init() again
	 */
	//nvram_close(index);
	//nvram_init(index);
#else
	nvram_close(index);
	nvram_init(index);
#endif

	return nvram_bufget(index, name);
}

int nvram_set(int index, char *name, char *value)
{
	//LIBNV_PRINT("--> nvram_set\n");

	if (-1 == nvram_bufset(index, name, value))

		return -1;
	return nvram_commit(index);
}

char const *nvram_bufget(int index, char *name)
{
	int idx;
	static char const *ret;

	//LIBNV_PRINT("--> nvram_bufget %d\n", index);
	LIBNV_CHECK_INDEX("");
	LIBNV_CHECK_VALID();

	idx = cache_idx(index, name);

	if (-1 != idx) {
		if (fb[index].cache[idx].value) {
			//duplicate the value in case caller modify it
			//ret = strdup(fb[index].cache[idx].value);

			ret = fb[index].cache[idx].value;
			LIBNV_PRINT("bufget %d '%s'->'%s'\n", index, name, ret);
			return ret;
		}
	}

	//no default value set?
	//btw, we don't return NULL anymore!
	LIBNV_PRINT("bufget %d '%s'->''(empty) Warning!\n", index, name);

	return "";
}

int nvram_bufset(int index, char *name, char *value)
{
	int idx;

	//LIBNV_PRINT("--> nvram_bufset\n");

	LIBNV_CHECK_INDEX(-1);
	LIBNV_CHECK_VALID();


	idx = cache_idx(index, name);

	if (-1 == idx) {
		//find the first empty room
		for (idx = 0; idx < MAX_CACHE_ENTRY; idx++) {
			if (!fb[index].cache[idx].name)
				break;
		}
		//no any empty room
		if (idx == MAX_CACHE_ENTRY) {
			LIBNV_ERROR("run out of env cache, please increase MAX_CACHE_ENTRY\n");
			return -1;
		}
		fb[index].cache[idx].name = strdup(name);
		fb[index].cache[idx].value = strdup(value);
	}
	else {
		//abandon the previous value
		FREE(fb[index].cache[idx].value);
		fb[index].cache[idx].value = strdup(value);
	}
	LIBNV_PRINT("bufset %d '%s'->'%s'\n", index, name, value);
	fb[index].dirty = 1;
	return 0;
}

void nvram_buflist(int index)
{
	int i;

	//LIBNV_PRINT("--> nvram_buflist %d\n", index);
	LIBNV_CHECK_INDEX();
	LIBNV_CHECK_VALID();

	for (i = 0; i < MAX_CACHE_ENTRY; i++) {
		if (!fb[index].cache[i].name)
			break;
		printf("  '%s'='%s'\n", fb[index].cache[i].name, fb[index].cache[i].value);
	}
}

/*
 * write flash from cache
 */
int nvram_commit(int index)
{
	int fd;
	unsigned long to;
	int i, len;
	char *p;
	char fname[64];

	//LIBNV_PRINT("--> nvram_commit %d\n", index);
	LIBNV_CHECK_INDEX(-1);
	LIBNV_CHECK_VALID();

	sprintf(fname, "%s/%s", NV_DEV, fb[index].name);
	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		perror(NV_DEV);
		return -1;
	}
	
	if (!fb[index].dirty) {
		LIBNV_PRINT("nothing to be committed\n");
		return 0;
	}

	//construct env block
	len = fb[index].flash_max_len;
	fb[index].env.data = (char *)malloc(len);
	bzero(fb[index].env.data, len);
	p = fb[index].env.data;
	for (i = 0; i < MAX_CACHE_ENTRY; i++) {
		int l;
		if (!fb[index].cache[i].name || !fb[index].cache[i].value)
			break;
		l = strlen(fb[index].cache[i].name) + strlen(fb[index].cache[i].value) + 2;
		if (p - fb[index].env.data + 2 >= fb[index].flash_max_len) {
			LIBNV_ERROR("ENV_BLK_SIZE 0x%x is not enough!", ENV_BLK_SIZE);
			FREE(fb[index].env.data);
			return -1;
		}
		snprintf(p, l, "%s=%s\n", fb[index].cache[i].name, fb[index].cache[i].value);
		p += l;
	}
	*p = '\0'; //ending null


	len = p - fb[index].env.data;
	write(fd, fb[index].env.data, len);
	FREE(fb[index].env.data);

	fb[index].dirty = 0;

	close(fd);

	return 0;
}

/*
 * clear flash by writing all 1's value
 */
int nvram_clear(int index)
{
	int fd;
	char fname[64];

	LIBNV_PRINT("--> nvram_clear %d\n", index);
	LIBNV_CHECK_INDEX(-1);
	nvram_close(index);

	sprintf(fname, "%s/%s", NV_DEV, fb[index].name);
	fd = open(fname, O_WRONLY);
	if (fd < 0) {
		perror(NV_DEV);
		return -1;
	}

	//construct all 1s env block
	write(fd, "\0", 1);
	close(fd);
	fb[index].dirty = 0;

	return 0;
}

int getNvramNum(void)
{
	return FLASH_BLOCK_NUM;
}

unsigned int getNvramOffset(int index)
{
	LIBNV_CHECK_INDEX(0);
	return fb[index].flash_offset;
}

char *getNvramName(int index)
{
	LIBNV_CHECK_INDEX(NULL);
	return fb[index].name;
}

unsigned int getNvramBlockSize(int index)
{
	LIBNV_CHECK_INDEX(0);
	return fb[index].flash_max_len;
}

unsigned int getNvramIndex(char *name)
{
	int i;
	for (i = 0; i < FLASH_BLOCK_NUM; i++) {
		if (!strcmp(fb[i].name, name)) {
			return i;
		}
	}
	return -1;
}

