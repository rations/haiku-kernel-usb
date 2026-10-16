/*
 * Tests for running 32-bit code inside a 64-bit Haiku team: per-team LDT,
 * per-thread FS segment, context switches, signals and sigreturn checks.
 *
 * Build (in Haiku): gcc -O1 -Wall -o wow64_ldt_test wow64_ldt_test.c
 * Run:              ./wow64_ldt_test [test-number]
 *
 * Needs a kernel with the THREAD_SET_LDT_ENTRY family of "thread" generic
 * syscalls. On a stock kernel the first test reports them as missing.
 */

#include <OS.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>


/* From headers/private/system/arch/x86/arch_thread_defs.h of the patched
 * tree; the installed haiku_devel headers predate them. */
#define THREAD_SYSCALLS			"thread"
#define THREAD_SET_LDT_ENTRY	2
#define THREAD_SET_FS_BASE		3
#define THREAD_GET_FS_BASE		4
#define THREAD_GET_GS_BASE		5
#define THREAD_LDT_ENTRY_COUNT	8192

struct thread_ldt_entry {
	uint32_t	index;
	uint32_t	descriptor[2];
};

extern status_t _kern_generic_syscall(const char* subsystem, uint32 function,
	void* buffer, size_t bufferSize);

#define USER32_CODE_SELECTOR	0x1b
#define USER_CODE_SELECTOR		0x2b
#define USER_DATA_SELECTOR		0x23

/* offset in the signal context's fpu_state: fs follows trap_number (484) and
 * a pad byte */
#define FXSAVE_CS_OFFSET		480
#define FXSAVE_SS_OFFSET		482
#define FXSAVE_FS_OFFSET		486
/* DR6 of the last debug trap follows fs */
#define FXSAVE_DR6_OFFSET		488


/* --- low memory layout (one block per thread) ---------------------------- */

enum {
	LOW_SAVED_RSP	= 0x00,		/* uint64: 64-bit stack while in 32-bit code */
	LOW_FAR_PTR		= 0x08,		/* m16:32 far pointer back to the 64-bit stub */
	LOW_RESULT		= 0x10,		/* uint32: value read through %fs:0 */
	LOW_SELECTOR	= 0x14,		/* uint32: selector to load into %fs */
	LOW_COUNT		= 0x18,		/* uint32: loop iterations */
	LOW_EXPECTED	= 0x1c,		/* uint32: expected %fs:0 value */
	LOW_MISMATCHES	= 0x20,		/* uint32: loop iterations that read wrong */
	LOW_OPCODE		= 0x24,		/* uint32: which "bad" instruction to run */
	LOW_CODE32		= 0x100,
	LOW_STUB64		= 0x800,
	LOW_STACK_TOP	= 0x8000,
	LOW_TEB			= 0x8000,	/* the FS segment's target page */
	LOW_SIZE		= 0x10000,
};

/* 32-bit code, copied below 4 GB. %edi holds the block's address. */
extern const uint8_t code32_start[], code32_end[];
__asm__(
	".pushsection .text\n"
	".code32\n"
	"code32_start:\n"
	"	movl 0x14(%edi), %eax\n"		/* LOW_SELECTOR */
	"	movw %ax, %fs\n"
	"	movl 0x24(%edi), %eax\n"		/* LOW_OPCODE */
	"	cmpl $1, %eax\n"
	"	je 10f\n"
	"	cmpl $2, %eax\n"
	"	je 11f\n"
	"	cmpl $3, %eax\n"
	"	je 12f\n"
	"	cmpl $4, %eax\n"
	"	je 13f\n"
	"	movl %fs:0, %eax\n"
	"	movl %eax, 0x10(%edi)\n"		/* LOW_RESULT */
	"	movl 0x18(%edi), %ecx\n"		/* LOW_COUNT */
	"	testl %ecx, %ecx\n"
	"	jz 3f\n"
	"1:	movl %fs:0, %eax\n"
	"	cmpl 0x1c(%edi), %eax\n"		/* LOW_EXPECTED */
	"	je 2f\n"
	"	incl 0x20(%edi)\n"				/* LOW_MISMATCHES */
	"2:	decl %ecx\n"
	"	jnz 1b\n"
	"3:	ljmp *0x08(%edi)\n"				/* LOW_FAR_PTR */
	"10: int $0x80\n"
	"	jmp 3b\n"
	"11: int $0x63\n"
	"	jmp 3b\n"
	"12: sysenter\n"
	"	jmp 3b\n"
	"13: syscall\n"
	"	jmp 3b\n"
	"code32_end:\n"
	".code64\n"
	".popsection\n");

/* 64-bit stub, also copied below 4 GB: back on the 64-bit stack, return. */
extern const uint8_t stub64_start[], stub64_end[];
__asm__(
	".pushsection .text\n"
	"stub64_start:\n"
	"	movl %edi, %edi\n"
	"	movq (%rdi), %rsp\n"
	"	popq %r15\n"
	"	popq %r14\n"
	"	popq %r13\n"
	"	popq %r12\n"
	"	popq %rbp\n"
	"	popq %rbx\n"
	"	ret\n"
	"stub64_end:\n"
	".popsection\n");

/* void run32(uint8_t* low): runs the copied 32-bit code. */
extern void run32(uint8_t* low);
__asm__(
	".pushsection .text\n"
	".globl run32\n"
	"run32:\n"
	"	pushq %rbx\n"
	"	pushq %rbp\n"
	"	pushq %r12\n"
	"	pushq %r13\n"
	"	pushq %r14\n"
	"	pushq %r15\n"
	"	movq %rsp, (%rdi)\n"
	/* 32-bit code addresses memory through DS and ES (ignored in 64-bit
	 * mode, so usually null there) */
	"	movl $0x23, %eax\n"
	"	movw %ax, %ds\n"
	"	movw %ax, %es\n"
	"	leaq 0x8000(%rdi), %rsp\n"
	"	pushq $0x1b\n"
	"	leaq 0x100(%rdi), %rax\n"
	"	pushq %rax\n"
	"	lretq\n"
	".popsection\n");


static int sFailures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		sFailures++; \
	} \
} while (0)


static status_t
set_fs_base(uint64_t base)
{
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_FS_BASE, &base,
		sizeof(base));
}


/* Back to the 64-bit TLS after 32-bit code left an LDT selector in %fs.
 * Must come before anything touches TLS. */
static void
restore_tls(void)
{
	set_fs_base(0);
}


static uint32_t
data_descriptor_high(uint32_t base, uint32_t limit, uint32_t type,
	uint32_t dpl, uint32_t db, uint32_t longMode, uint32_t system)
{
	return ((base >> 16) & 0xff) | (type << 8) | ((system ? 0 : 1) << 12)
		| (dpl << 13) | (1u << 15) | (((limit >> 16) & 0xf) << 16)
		| (longMode << 21) | (db << 22) | (base & 0xff000000);
}


static status_t
set_ldt_raw(uint32_t index, uint32_t low, uint32_t high)
{
	struct thread_ldt_entry entry = { index, { low, high } };
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_LDT_ENTRY,
		&entry, sizeof(entry));
}


/* a writable 32-bit data segment, DPL 3 */
static status_t
set_ldt_data(uint32_t index, uint32_t base, uint32_t limit)
{
	return set_ldt_raw(index, (limit & 0xffff) | (base << 16),
		data_descriptor_high(base, limit, 0x2, 3, 1, 0, 0));
}


static uint16_t
ldt_selector(uint32_t index)
{
	return (uint16_t)((index << 3) | 7);
}


static uint8_t*
alloc_low(int slot)
{
	void* address = (void*)(uintptr_t)(0x50000000u + slot * LOW_SIZE);
	area_id area = create_area("wow64 test low", &address, B_EXACT_ADDRESS,
		LOW_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA);
	if (area < 0) {
		printf("  cannot create low area at %p: %s\n", address,
			strerror(area));
		return NULL;
	}

	uint8_t* low = (uint8_t*)address;
	memcpy(low + LOW_CODE32, code32_start, code32_end - code32_start);
	memcpy(low + LOW_STUB64, stub64_start, stub64_end - stub64_start);
	*(uint32_t*)(low + LOW_FAR_PTR) = (uint32_t)(uintptr_t)(low + LOW_STUB64);
	*(uint16_t*)(low + LOW_FAR_PTR + 4) = USER_CODE_SELECTOR;
	return low;
}


/* Gives slot its own LDT entry (index 16 + slot) over its TEB page. */
static uint16_t
setup_slot(uint8_t* low, int slot, uint32_t magic)
{
	uint32_t index = 16 + slot;
	*(uint32_t*)(low + LOW_TEB) = magic;
	status_t status = set_ldt_data(index, (uint32_t)(uintptr_t)(low + LOW_TEB),
		0xfff);
	CHECK(status == B_OK, "set_ldt_entry(%u) = %s", index, strerror(status));
	*(uint32_t*)(low + LOW_SELECTOR) = ldt_selector(index);
	*(uint32_t*)(low + LOW_EXPECTED) = magic;
	*(uint32_t*)(low + LOW_MISMATCHES) = 0;
	*(uint32_t*)(low + LOW_OPCODE) = 0;
	return ldt_selector(index);
}


/* --- test 1: syscalls exist, basic 32-bit round trip ---------------------- */

static void
test_basic(void)
{
	printf("1: basic 32-bit round trip\n");

	uint64_t base = 0;
	status_t status = _kern_generic_syscall(THREAD_SYSCALLS,
		THREAD_GET_FS_BASE, &base, sizeof(base));
	if (status != B_OK) {
		printf("  FAIL: THREAD_GET_FS_BASE: %s (stock kernel?)\n",
			strerror(status));
		sFailures++;
		return;
	}
	uint64_t tlsBase;
	__asm__ volatile("movq %%fs:0, %0" : "=r" (tlsBase));
	CHECK(base == tlsBase, "fs base %#llx != TLS self pointer %#llx",
		(unsigned long long)base, (unsigned long long)tlsBase);

	uint64_t gsBase = 1;
	status = _kern_generic_syscall(THREAD_SYSCALLS, THREAD_GET_GS_BASE,
		&gsBase, sizeof(gsBase));
	CHECK(status == B_OK, "THREAD_GET_GS_BASE: %s", strerror(status));

	uint8_t* low = alloc_low(0);
	if (low == NULL) {
		sFailures++;
		return;
	}
	setup_slot(low, 0, 0x12345678);
	*(uint32_t*)(low + LOW_COUNT) = 0;
	run32(low);
	restore_tls();
	CHECK(*(uint32_t*)(low + LOW_RESULT) == 0x12345678,
		"read %#x through %%fs:0", *(uint32_t*)(low + LOW_RESULT));
	CHECK(find_thread(NULL) > 0, "TLS broken after return");
}


/* --- test 2: FS survives context switches -------------------------------- */

#define SWITCH_THREADS 8

static void*
switch_worker(void* arg)
{
	int slot = (int)(intptr_t)arg;
	uint8_t* low = (uint8_t*)(uintptr_t)(0x50000000u + slot * LOW_SIZE);
	*(uint32_t*)(low + LOW_COUNT) = 400000000;
	run32(low);
	restore_tls();
	return NULL;
}


static void*
hog(void* arg)
{
	volatile int* stop = (volatile int*)arg;
	while (!*stop)
		;
	return NULL;
}


static void
test_context_switch(void)
{
	printf("2: FS kept across context switches (%d threads + hogs)\n",
		SWITCH_THREADS);

	pthread_t workers[SWITCH_THREADS];
	uint8_t* lows[SWITCH_THREADS];
	for (int i = 0; i < SWITCH_THREADS; i++) {
		lows[i] = alloc_low(1 + i);
		if (lows[i] == NULL) {
			sFailures++;
			return;
		}
		setup_slot(lows[i], 1 + i, 0xa0000000u + i);
	}

	system_info info;
	get_system_info(&info);
	int hogCount = info.cpu_count * 2;
	pthread_t hogs[hogCount];
	volatile int stop = 0;
	for (int i = 0; i < hogCount; i++)
		pthread_create(&hogs[i], NULL, hog, (void*)&stop);

	for (int i = 0; i < SWITCH_THREADS; i++)
		pthread_create(&workers[i], NULL, switch_worker, (void*)(intptr_t)(1 + i));
	for (int i = 0; i < SWITCH_THREADS; i++)
		pthread_join(workers[i], NULL);

	stop = 1;
	for (int i = 0; i < hogCount; i++)
		pthread_join(hogs[i], NULL);

	for (int i = 0; i < SWITCH_THREADS; i++) {
		CHECK(*(uint32_t*)(lows[i] + LOW_MISMATCHES) == 0,
			"thread %d read a wrong %%fs:0 %u times", i,
			*(uint32_t*)(lows[i] + LOW_MISMATCHES));
	}
}


/* --- test 3: signal in 32-bit code --------------------------------------- */

static volatile int sSignals = 0;
static volatile int sBadContexts = 0;
static volatile int sBadTLS = 0;
static thread_id sMainThread;
static volatile uint16_t sExpectedFS;

static void
alarm_handler(int sig, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	uint8_t* fx = (uint8_t*)&context->uc_mcontext.fpu.fp_fxsave;
	uint16_t cs = *(uint16_t*)(fx + FXSAVE_CS_OFFSET);
	uint16_t fs = *(uint16_t*)(fx + FXSAVE_FS_OFFSET);

	if (find_thread(NULL) != sMainThread)
		sBadTLS++;
	/* the signal may land in the 64-bit parts, too */
	if (cs == USER32_CODE_SELECTOR && fs != sExpectedFS)
		sBadContexts++;
	if (cs == USER32_CODE_SELECTOR)
		sSignals++;
}


static void
test_signal(void)
{
	printf("3: signals interrupting 32-bit code\n");

	uint8_t* low = alloc_low(20);
	if (low == NULL) {
		sFailures++;
		return;
	}
	sExpectedFS = setup_slot(low, 20, 0x5151f00d);
	sMainThread = find_thread(NULL);

	struct sigaction action = {};
	action.sa_sigaction = alarm_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGALRM, &action, NULL);

	struct itimerval timer = { { 0, 2000 }, { 0, 2000 } };
	setitimer(ITIMER_REAL, &timer, NULL);
	*(uint32_t*)(low + LOW_COUNT) = 1500000000;
	run32(low);
	restore_tls();
	struct itimerval off = {};
	setitimer(ITIMER_REAL, &off, NULL);
	signal(SIGALRM, SIG_DFL);

	printf("  %d signals landed in 32-bit code\n", sSignals);
	CHECK(sSignals > 0, "no signal interrupted 32-bit code");
	CHECK(sBadTLS == 0, "find_thread() wrong in %d handlers", sBadTLS);
	CHECK(sBadContexts == 0, "fs wrong in %d contexts", sBadContexts);
	CHECK(*(uint32_t*)(low + LOW_MISMATCHES) == 0,
		"%u wrong %%fs:0 reads after signals",
		*(uint32_t*)(low + LOW_MISMATCHES));
}


/* --- test 4: forged sigreturn contexts are refused ------------------------ */

static int sForgeMode;

static void
forge_handler(int sig, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	uint8_t* fx = (uint8_t*)&context->uc_mcontext.fpu.fp_fxsave;
	switch (sForgeMode) {
		case 0:
			*(uint16_t*)(fx + FXSAVE_CS_OFFSET) = 0x08;	/* kernel code */
			break;
		case 1:
			*(uint16_t*)(fx + FXSAVE_SS_OFFSET) = 0x10;	/* kernel data */
			break;
		case 2:
			*(uint16_t*)(fx + FXSAVE_FS_OFFSET) = ldt_selector(4000);
				/* never written */
			break;
		case 3:
			*(uint16_t*)(fx + FXSAVE_CS_OFFSET) = USER32_CODE_SELECTOR;
			context->uc_mcontext.rip = 0x100000000ull;	/* above 4 GB */
			break;
	}
}


static void
test_forged_sigreturn(void)
{
	printf("4: forged sigreturn contexts kill the thread, not the system\n");

	for (int mode = 0; mode < 4; mode++) {
		pid_t child = fork();
		if (child == 0) {
			sForgeMode = mode;
			/* give the team an LDT, so mode 2 is checked against it */
			set_ldt_data(17, 0x50000000u, 0xfff);
			struct sigaction action = {};
			action.sa_sigaction = forge_handler;
			action.sa_flags = SA_SIGINFO;
			sigaction(SIGUSR1, &action, NULL);
			raise(SIGUSR1);
			_exit(0);	/* reached only if the forged frame was accepted */
		}
		int status;
		waitpid(child, &status, 0);
		CHECK(!(WIFEXITED(status) && WEXITSTATUS(status) == 0),
			"mode %d: forged context was accepted", mode);
	}
}


/* --- test 5: invalid descriptors are refused ------------------------------ */

static void
test_bad_descriptors(void)
{
	printf("5: invalid LDT descriptors are refused\n");

	uint32_t base = 0x50000000u;
	uint32_t low = 0xfff | (base << 16);
	struct {
		const char*	name;
		uint32_t	index;
		uint32_t	high;
		status_t	expected;
	} cases[] = {
		{ "DPL 0", 30, data_descriptor_high(base, 0xfff, 0x2, 0, 1, 0, 0),
			B_BAD_VALUE },
		{ "system (call gate)", 31,
			data_descriptor_high(base, 0xfff, 0xc, 3, 1, 0, 1), B_BAD_VALUE },
		{ "code segment", 32,
			data_descriptor_high(base, 0xfff, 0xa, 3, 1, 0, 0), B_BAD_VALUE },
		{ "read-only data", 33,
			data_descriptor_high(base, 0xfff, 0x0, 3, 1, 0, 0), B_BAD_VALUE },
		{ "long mode bit", 34,
			data_descriptor_high(base, 0xfff, 0x2, 3, 0, 1, 0), B_BAD_VALUE },
		{ "16-bit", 35, data_descriptor_high(base, 0xfff, 0x2, 3, 0, 0, 0),
			B_BAD_VALUE },
		{ "not present", 36,
			data_descriptor_high(base, 0xfff, 0x2, 3, 1, 0, 0) & ~(1u << 15),
			B_BAD_VALUE },
		{ "index 8192", THREAD_LDT_ENTRY_COUNT,
			data_descriptor_high(base, 0xfff, 0x2, 3, 1, 0, 0), B_BAD_INDEX },
		{ "valid", 37, data_descriptor_high(base, 0xfff, 0x2, 3, 1, 0, 0),
			B_OK },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		status_t status = set_ldt_raw(cases[i].index, low, cases[i].high);
		CHECK(status == cases[i].expected, "%s: got %s, expected %s",
			cases[i].name, strerror(status), strerror(cases[i].expected));
	}

	/* clearing an entry is allowed and leaves it loadable */
	CHECK(set_ldt_raw(37, 0, 0) == B_OK, "clearing entry 37 refused");
	uint16_t selector = ldt_selector(37);
	__asm__ volatile("movw %0, %%fs" : : "r" (selector));
	restore_tls();
}


/* --- tests 6 and 7: run in children, expect a signal ---------------------- */

static void
fault_handler(int sig)
{
	restore_tls();
	_exit(100 + sig);
}


static int
run_child(void (*function)(void))
{
	pid_t child = fork();
	if (child == 0) {
		signal(SIGSEGV, fault_handler);
		signal(SIGBUS, fault_handler);
		signal(SIGILL, fault_handler);
		function();
		_exit(0);
	}
	int status;
	waitpid(child, &status, 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -WTERMSIG(status);
}


static void
load_ldt_selector_17(void)
{
	uint16_t selector = ldt_selector(17);
	__asm__ volatile("movw %0, %%fs" : : "r" (selector));
	restore_tls();
}


static void
test_fork_exec(const char* self)
{
	printf("6: a forked or exec'd child has no LDT\n");

	/* the parent has an LDT (from earlier tests); entry 17 is valid there */
	set_ldt_data(17, 0x50000000u, 0xfff);

	int result = run_child(load_ldt_selector_17);
	CHECK(result >= 100, "forked child loaded an LDT selector (exit %d)",
		result);

	pid_t child = fork();
	if (child == 0) {
		execl(self, self, "--load-ldt-selector", (char*)NULL);
		_exit(99);
	}
	int status;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) >= 100,
		"exec'd child loaded an LDT selector (status %#x)", status);
}


static int sOpcode;

static void
run_bad_instruction(void)
{
	uint8_t* low = alloc_low(40);
	if (low == NULL)
		_exit(98);
	setup_slot(low, 40, 1);
	*(uint32_t*)(low + LOW_OPCODE) = sOpcode;
	*(uint32_t*)(low + LOW_COUNT) = 0;
	run32(low);
	restore_tls();
}


static void
test_32bit_syscall_instructions(void)
{
	printf("7: 32-bit system call instructions from a 64-bit team\n");
	const char* names[] = { "", "int $0x80", "int $0x63", "sysenter",
		"syscall" };
	for (sOpcode = 1; sOpcode <= 4; sOpcode++) {
		int result = run_child(run_bad_instruction);
		printf("  %-10s -> %s %d\n", names[sOpcode],
			result >= 100 ? "signal" : "exit", result >= 100
				? result - 100 : result);
		/* syscall on AMD returns at once (CSTAR stub): exit 0 is fine */
		if (sOpcode != 4)
			CHECK(result >= 100, "%s did not raise a signal", names[sOpcode]);
	}
}


/* --- test 8: many threads, LDT entries churning --------------------------- */

#define CHURN_THREADS 200

static void*
churn_worker(void* arg)
{
	int i = (int)(intptr_t)arg;
	uint32_t index = 1000 + i;
	for (int round = 0; round < 200; round++) {
		if (set_ldt_data(index, 0x50000000u, 0xfff) != B_OK)
			return (void*)1;
		uint16_t selector = ldt_selector(index);
		__asm__ volatile("movw %0, %%fs" : : "r" (selector));
		restore_tls();
		if (set_ldt_raw(index, 0, 0) != B_OK)
			return (void*)1;
	}
	return NULL;
}


static void
test_churn(void)
{
	printf("8: %d threads setting, loading and clearing entries\n",
		CHURN_THREADS);
	pthread_t threads[CHURN_THREADS];
	for (int i = 0; i < CHURN_THREADS; i++)
		pthread_create(&threads[i], NULL, churn_worker, (void*)(intptr_t)i);
	int failed = 0;
	for (int i = 0; i < CHURN_THREADS; i++) {
		void* result;
		pthread_join(threads[i], &result);
		if (result != NULL)
			failed++;
	}
	CHECK(failed == 0, "%d threads failed", failed);
}


/* --- test 9: unused LDT entry raises a signal, not a panic --------------- */

static void
load_unused_entry(void)
{
	set_ldt_data(17, 0x50000000u, 0xfff);
	uint16_t selector = ldt_selector(5000);	/* never written: not present */
	__asm__ volatile("movw %0, %%fs" : : "r" (selector));
	restore_tls();
}


static void
test_not_present(void)
{
	printf("9: loading a never-written LDT entry (#NP)\n");
	int result = run_child(load_unused_entry);
	CHECK(result >= 100, "no signal (exit %d)", result);
}


/* --- test 10: debug traps reach the program as SIGTRAP ------------------- */

#define EFLAGS_TF	0x100

static volatile int sTraps;
static volatile int sBadTraps;
extern char trap_int3_after[];

static void
trap_handler(int sig, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	int trap = context->uc_mcontext.fpu.fp_fxsave.trap_number;

	sTraps++;
	if (info->si_code == TRAP_TRACE) {
		if (trap != 1 || (context->uc_mcontext.rflags & EFLAGS_TF) == 0) {
			printf("  single step: trap %d, rflags %#lx\n", trap,
				(unsigned long)context->uc_mcontext.rflags);
			sBadTraps++;
		}
		/* as Wine does: the step is done, continue normally */
		context->uc_mcontext.rflags &= ~EFLAGS_TF;
	} else if (info->si_code == TRAP_BRKPT) {
		if (trap != 3
			|| context->uc_mcontext.rip != (uint64_t)trap_int3_after) {
			printf("  int3: trap %d, rip %#lx, expected %p\n", trap,
				(unsigned long)context->uc_mcontext.rip, trap_int3_after);
			sBadTraps++;
		}
	} else {
		printf("  unexpected si_code %d\n", info->si_code);
		sBadTraps++;
	}
}


static void
install_trap_handler(void)
{
	struct sigaction action = {};
	action.sa_sigaction = trap_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGTRAP, &action, NULL);
}


static void
single_step_child(void)
{
	install_trap_handler();
	/* Set the trap flag like Windows anti-debugging code does. The trap
	 * comes after the instruction following popfq. Skip the red zone. */
	__asm__ volatile(
		"lea -128(%%rsp), %%rsp\n\t"
		"pushfq\n\t"
		"orq $0x100, (%%rsp)\n\t"
		"popfq\n\t"
		"nop\n\t"
		"nop\n\t"
		"lea 128(%%rsp), %%rsp\n\t"
		::: "memory", "cc");
	if (sTraps != 1 || sBadTraps != 0)
		printf("  single step: %d traps, %d bad\n", sTraps, sBadTraps);
	_exit(sTraps == 1 && sBadTraps == 0 ? 0 : 1);
}


static void
int3_child(void)
{
	install_trap_handler();
	__asm__ volatile(
		"int3\n"
		".globl trap_int3_after\n"
		"trap_int3_after:\n\t"
		::: "memory");
	if (sTraps != 1 || sBadTraps != 0)
		printf("  int3: %d traps, %d bad\n", sTraps, sBadTraps);
	_exit(sTraps == 1 && sBadTraps == 0 ? 0 : 1);
}


static void
test_traps(void)
{
	printf("10: single step and int3 reach the program as SIGTRAP\n");
	int result = run_child(single_step_child);
	CHECK(result == 0, "single step (exit %d)", result);
	result = run_child(int3_child);
	CHECK(result == 0, "int3 (exit %d)", result);
}


/* --- test 11: a thread's own hardware breakpoints ------------------------ */

#define THREAD_SET_DEBUG_REGISTERS	6
#define EFLAGS_RF	0x10000
#define DR7_L0		0x1
#define DR7_L1		0x4
#define DR7_GD		0x2000
#define DR7_RW1_WRITE	(0x1 << 20)
#define DR7_LEN1_4		(0x3 << 22)
#define DR7_RW0_IO		(0x2 << 16)
#define DR7_LEN0_4		(0x3 << 18)

struct thread_debug_registers {
	uint64_t	address[4];
	uint64_t	dr7;
};

static volatile int sBreakpointHits;
static volatile int sWatchHits;
static volatile int sBadHits;
static volatile int sWatched;

static status_t
set_debug_registers(uint64_t dr0, uint64_t dr1, uint64_t dr7)
{
	struct thread_debug_registers registers = { { dr0, dr1, 0, 0 }, dr7 };
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_DEBUG_REGISTERS,
		&registers, sizeof(registers));
}


__attribute__((noinline)) static int
breakpoint_target(int value)
{
	__asm__ volatile("" ::: "memory");
	return value + 1;
}


static void
breakpoint_handler(int sig, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	uint8_t* fx = (uint8_t*)&context->uc_mcontext.fpu.fp_fxsave;
	unsigned long dr6 = *(unsigned long*)(fx + FXSAVE_DR6_OFFSET);
	uint64_t rip = context->uc_mcontext.rip;

	if (dr6 & 0x1) {
		sBreakpointHits++;
		if (info->si_code != TRAP_BRKPT || rip != (uint64_t)breakpoint_target
			|| (context->uc_mcontext.rflags & EFLAGS_RF) == 0) {
			printf("  breakpoint: si_code %d, rip %#lx, rflags %#lx\n",
				info->si_code, (unsigned long)rip,
				(unsigned long)context->uc_mcontext.rflags);
			sBadHits++;
		}
	} else if (dr6 & 0x2) {
		sWatchHits++;
		if (info->si_code != TRAP_BRKPT || sWatched != 7) {
			printf("  watchpoint: si_code %d, value %d\n", info->si_code,
				sWatched);
			sBadHits++;
		}
	} else {
		printf("  SIGTRAP without a breakpoint: dr6 %#lx\n", dr6);
		sBadHits++;
		/* do not loop on an unexplained fault */
		context->uc_mcontext.rflags |= EFLAGS_RF;
	}
}


static void
install_breakpoint_handler(void)
{
	struct sigaction action = {};
	action.sa_sigaction = breakpoint_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGTRAP, &action, NULL);
}


static void*
other_thread_calls_target(void* arg)
{
	for (int i = 0; i < 1000; i++)
		breakpoint_target(i);
	return NULL;
}


static void
fork_child_calls_target(void)
{
	install_breakpoint_handler();
	sBreakpointHits = 0;	/* the parent's count came along */
	breakpoint_target(1);
	_exit(sBreakpointHits == 0 ? 0 : 1);
}


static void
test_breakpoints(void)
{
	printf("11: a thread's own hardware breakpoints\n");
	install_breakpoint_handler();
	uint64_t target = (uint64_t)breakpoint_target;

	/* refused: kernel address, I/O type, GD, 4-byte instruction breakpoint,
	 * misaligned watchpoint */
	CHECK(set_debug_registers(0xffffffff80000000ull, 0, DR7_L0) != B_OK,
		"kernel address accepted");
	CHECK(set_debug_registers(target, 0, DR7_L0 | DR7_RW0_IO) != B_OK,
		"I/O breakpoint accepted");
	CHECK(set_debug_registers(target, 0, DR7_L0 | DR7_GD) != B_OK,
		"GD accepted");
	CHECK(set_debug_registers(target, 0, DR7_L0 | DR7_LEN0_4) != B_OK,
		"4-byte instruction breakpoint accepted");
	CHECK(set_debug_registers(0, (uint64_t)&sWatched + 1,
		DR7_L1 | DR7_RW1_WRITE | DR7_LEN1_4) != B_OK,
		"misaligned watchpoint accepted");

	/* instruction breakpoint, resumed with RF, and again on the next call */
	status_t status = set_debug_registers(target, 0, DR7_L0);
	CHECK(status == B_OK, "setting a breakpoint failed: %s", strerror(status));
	int value = breakpoint_target(1);
	value += breakpoint_target(2);
	CHECK(value == 5, "target returned %d", value);
	CHECK(sBreakpointHits == 2, "%d breakpoint hits, expected 2",
		sBreakpointHits);

	/* other threads do not have it */
	pthread_t other;
	pthread_create(&other, NULL, other_thread_calls_target, NULL);
	pthread_join(other, NULL);
	CHECK(sBreakpointHits == 2, "another thread hit the breakpoint");

	/* nor a forked child */
	int result = run_child(fork_child_calls_target);
	CHECK(result == 0, "the forked child hit the breakpoint (exit %d)",
		result);

	/* it survives context switches */
	for (int i = 0; i < 50; i++)
		snooze(1000);
	breakpoint_target(3);
	CHECK(sBreakpointHits == 3, "%d hits after sleeping, expected 3",
		sBreakpointHits);

	/* a write watchpoint in slot 1 */
	status = set_debug_registers(0, (uint64_t)&sWatched,
		DR7_L1 | DR7_RW1_WRITE | DR7_LEN1_4);
	CHECK(status == B_OK, "setting a watchpoint failed: %s",
		strerror(status));
	sWatched = 7;
	CHECK(sWatchHits == 1, "%d watchpoint hits, expected 1", sWatchHits);

	/* cleared */
	set_debug_registers(0, 0, 0);
	breakpoint_target(4);
	sWatched = 8;
	CHECK(sBreakpointHits == 3 && sWatchHits == 1,
		"hits after clearing: %d, %d", sBreakpointHits, sWatchHits);
	CHECK(sBadHits == 0, "%d wrong traps", sBadHits);
	signal(SIGTRAP, SIG_DFL);
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--load-ldt-selector") == 0) {
		signal(SIGSEGV, fault_handler);
		signal(SIGBUS, fault_handler);
		load_ldt_selector_17();
		return 0;
	}

	int only = argc > 1 ? atoi(argv[1]) : 0;
	setvbuf(stdout, NULL, _IONBF, 0);

	if (only == 0 || only == 1) test_basic();
	if (sFailures > 0 && only == 0) {
		printf("basic test failed, stopping\n");
		return 1;
	}
	if (only == 0 || only == 5) test_bad_descriptors();
	if (only == 0 || only == 9) test_not_present();
	if (only == 0 || only == 2) test_context_switch();
	if (only == 0 || only == 3) test_signal();
	if (only == 0 || only == 4) test_forged_sigreturn();
	if (only == 0 || only == 6) test_fork_exec(argv[0]);
	if (only == 0 || only == 7) test_32bit_syscall_instructions();
	if (only == 0 || only == 8) test_churn();
	if (only == 0 || only == 10) test_traps();
	if (only == 0 || only == 11) test_breakpoints();

	printf(sFailures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
