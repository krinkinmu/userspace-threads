#define _XOPEN_SOURCE 600
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#include <stdatomic.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <signal.h>
#include <sys/time.h>



struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

void list_init(struct list_head *list)
{
	list->next = list->prev = list;
}

int list_empty(const struct list_head *list)
{
	return list->next == list;
}

void _list_insert(struct list_head *node, struct list_head *prev,
			struct list_head *next)
{
	node->prev = prev;
	node->next = next;
	prev->next = node;
	next->prev = node;
}

void list_add_tail(struct list_head *node, struct list_head *list)
{
	_list_insert(node, list->prev, list);
}

void list_add(struct list_head *node, struct list_head *list)
{
	_list_insert(node, list, list->next);
}

void list_del(struct list_head *node)
{
	struct list_head *prev = node->prev;
	struct list_head *next = node->next;

	prev->next = next;
	next->prev = prev;
}

void _list_splice(struct list_head *first, struct list_head *last,
			struct list_head *prev)
{
	struct list_head *next = prev->next;

	first->prev = prev;
	last->next = next;
	prev->next = first;
	next->prev = last;
}

void list_splice(struct list_head *from, struct list_head *to)
{
	if (list_empty(from))
		return;

	struct list_head *first = from->next;
	struct list_head *last = from->prev;

	list_init(from);
	_list_splice(first, last, to);
}

void list_splice_tail(struct list_head *from, struct list_head *to)
{
	if (list_empty(from))
		return;

	struct list_head *first = from->next;
	struct list_head *last = from->prev;

	list_init(from);
	_list_splice(first, last, to->prev);
}


enum thread_state {
	THREAD_ACTIVE,
	THREAD_BLOCKED,
	THREAD_FINISHING,
	THREAD_DEAD
};

struct thread {
	struct list_head ll;
	void *stack;
	size_t stack_size;
	enum thread_state state;
	jmp_buf ctx;
	void (*entry)(void *);
	void *arg;
	int first_time;
};


static struct thread *current;
static struct thread *idle;
static struct list_head ready;
static int blocked;


int block_signals(void)
{
	const int ret = !blocked;
	sigset_t ss;

	sigfillset(&ss);
	assert(!sigprocmask(SIG_BLOCK, &ss, NULL));
	blocked = 1;
	return ret;
}

void unblock_signals(int enable)
{
	if (!enable)
		return;

	sigset_t ss;

	blocked = 0;
	sigfillset(&ss);
	assert(!sigprocmask(SIG_UNBLOCK, &ss, NULL));
}


void _thread_place(struct thread *thread)
{
	if (current->state == THREAD_FINISHING)
		current->state = THREAD_DEAD;
	current = thread;
}

void _thread_entry(struct thread *thread)
{
	void thread_exit(void);

	assert(blocked);

	_thread_place(thread);
	thread->first_time = 0;
	unblock_signals(1);

	thread->entry(thread->arg);
	thread_exit();
}

void _thread_switch(struct thread *from, struct thread *to)
{
	assert(blocked);

	if (setjmp(from->ctx)) {
		_thread_place(from);
		return;
	}

	if (!to->first_time)
		longjmp(to->ctx, 1);

	char *ptr = (char *)to->stack + to->stack_size;

	/**
	   We need to change stack. Probably it's possible to avoid assembly
	   here using sigaltstack in the following way:
	     - allocate a memory region and setup it as an alternate stack
	     - generate the signal that uses the alternate stack
	     - inside the handler call setjmp to save the handler context
	     - use longjmp to jump to the saved context as usual

	   As you can see this approach is quite complicated. Moreover since
	   there is no way to pass an argument to a signal handler we would
	   need to use global variables and so need to consider protection
	   agains concurrent calls to the routine creating new thread.

	   All in all, it's much simpler to check whether we switch to a
	   thread for a first time and switch stacks using assembly code.
	**/
	__asm__ ("movq %0, %%rdi; movq %1, %%rsp; callq _thread_entry\n"
			:
			: "r"(to), "r"(ptr)
			: "rdi", "rsp", "memory");
}


void _thread_setup(struct thread *thread,
			void (*entry)(void *), void *arg,
			size_t stack)
{
	struct sigaction sa;

	assert(thread->stack = malloc(stack));
	thread->stack_size = stack;
	thread->entry = entry;
	thread->arg = arg;
	thread->first_time = 1;
}

void thread_setup(struct thread *thread, void (*entry)(void *), void *arg)
{
	const size_t DEFAULT_STACK = (size_t)2 * 1024 * 1024;

	_thread_setup(thread, entry, arg, DEFAULT_STACK);
}

void thread_release(struct thread *thread)
{
	free(thread->stack);
}


struct thread *thread_current(void)
{
	return current;
}

void timer_handler(int unused)
{
	void schedule(void);

	(void) unused;

	schedule();
}

void scheduler_setup(void)
{
	static struct thread me;
	struct itimerval period;
	struct sigaction sa;

	list_init(&ready);
	me.state = THREAD_ACTIVE;
	current = &me;
	idle = &me;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = &timer_handler;
	sa.sa_flags = SA_NODEFER | SA_RESTART;
	assert(!sigaction(SIGVTALRM, &sa, NULL));

	memset(&period, 0, sizeof(period));
	period.it_interval.tv_usec = 5000;
	period.it_value.tv_usec = 5000;
	assert(!setitimer(ITIMER_VIRTUAL, &period, NULL));
}

void schedule(void)
{
	struct thread *me = thread_current();
	struct thread *next = NULL;
	const int enabled = block_signals();

	assert(enabled);

	if (!list_empty(&ready)) {
		next = (struct thread *)ready.next;
		list_del(&next->ll);
	}

	if (!next && me->state != THREAD_ACTIVE)
		next = idle;

	if (!next) {
		unblock_signals(enabled);
		return;
	}

	if (me != idle && me->state == THREAD_ACTIVE)
		list_add_tail(&me->ll, &ready);
	_thread_switch(me, next);
	unblock_signals(enabled);
}

void thread_join(struct thread *thread)
{
	int enabled = block_signals();

	assert(enabled);
	while (thread->state != THREAD_DEAD) {
		unblock_signals(enabled);
		schedule();
		enabled = block_signals();
	}
	unblock_signals(enabled);
}

void thread_start(struct thread *thread)
{
	const int enabled = block_signals();

	thread->state = THREAD_ACTIVE;
	list_add_tail(&thread->ll, &ready);
	unblock_signals(enabled);
}

void thread_wake(struct thread *thread)
{
	thread_start(thread);
}

void thread_block(void)
{
	struct thread *me = thread_current();
	const int enabled = block_signals();

	me->state = THREAD_BLOCKED;
	unblock_signals(enabled);
}

void thread_exit(void)
{
	struct thread *me = thread_current();
	const int enabled = block_signals();

	assert(enabled);
	me->state = THREAD_FINISHING;
	unblock_signals(enabled);
	schedule();
}


struct lock {
	struct list_head wait;
	struct thread *owner;
	int magic;
};

struct _wait {
	struct list_head ll;
	struct thread *thread;
};


void lock_init(struct lock *lock)
{
	list_init(&lock->wait);
	lock->owner = NULL;
	lock->magic = 1263575;
}

void lock_check_magic(const struct lock *lock)
{
	assert(lock->magic == 1263575);
}

void lock(struct lock *lock)
{
	lock_check_magic(lock);

	int enabled = block_signals();
	struct thread *thread = thread_current();
	struct _wait wait;

	assert(enabled);
	wait.thread = thread;
	list_add_tail(&wait.ll, &lock->wait); 

	while (lock->owner || lock->wait.next != &wait.ll) {
		thread_block();
		unblock_signals(enabled);
		schedule();
		enabled = block_signals();
		assert(enabled);
	}
	lock->owner = thread;
	list_del(&wait.ll);
	unblock_signals(enabled);
}

void unlock(struct lock *lock)
{
	lock_check_magic(lock);

	const int enabled = block_signals();

	lock->owner = NULL;
	if (!list_empty(&lock->wait)) {
		struct _wait *wait = (struct _wait *)lock->wait.next;

		thread_wake(wait->thread);
	}
	unblock_signals(enabled);
}


struct condition {
	struct list_head wait;
	int magic;
};

void condition_init(struct condition *cv)
{
	list_init(&cv->wait);
	cv->magic = 3981279;
}

void condition_check_magic(const struct condition *cv)
{
	assert(cv->magic == 3981279);
}

void wait(struct condition *cv, struct lock *l)
{
	condition_check_magic(cv);

	const int enabled = block_signals();
	struct thread *thread = thread_current();
	struct _wait wait;

	assert(enabled);
	wait.thread = thread;
	list_add_tail(&wait.ll, &cv->wait);	
	thread_block();
	unblock_signals(enabled);
	unlock(l);
	schedule();
	lock(l);
}

void notify_one(struct condition *cv)
{
	condition_check_magic(cv);

	const int enabled = block_signals();

	if (!list_empty(&cv->wait)) {
		struct _wait *wait = (struct _wait *)cv->wait.next;

		list_del(&wait->ll);
		thread_wake(wait->thread);
	}
	unblock_signals(enabled);
}

void notify_all(struct condition *cv)
{
	condition_check_magic(cv);

	const int enabled = block_signals();
	struct list_head list;

	list_init(&list);
	list_splice(&cv->wait, &list);
	unblock_signals(enabled);

	for (struct list_head *ptr = list.next; ptr != &list; ) {
		struct _wait *wait = (struct _wait *)ptr;

		ptr = ptr->next;
		thread_wake(wait->thread);
	}
}


void threadx(void *arg)
{
	struct thread *me = thread_current();
	struct lock *io_lock = arg;

	for (int i = 0; i != 5; ++i) {
		lock(io_lock);
		printf("Hello from %p\n", me);
		unlock(io_lock);
		schedule();
	}
}

int main(void)
{
	struct lock stdio_lock;
	struct thread t[2];

	scheduler_setup();
	lock_init(&stdio_lock);

	thread_setup(&t[0], &threadx, &stdio_lock); thread_start(&t[0]);
	thread_setup(&t[1], &threadx, &stdio_lock); thread_start(&t[1]);

	thread_join(&t[1]); thread_release(&t[1]);
	thread_join(&t[0]); thread_release(&t[0]);

	return 0;
}
