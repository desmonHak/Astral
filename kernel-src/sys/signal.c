#include <kernel/scheduler.h>
#include <arch/signal.h>
#include <logging.h>

#define ACTION_TERM 0
#define ACTION_IGN 1
#define ACTION_CORE 2
#define ACTION_STOP 3
#define ACTION_CONT 4

static int defaultactions[NSIG] = {
	[SIGABRT] = ACTION_CORE,
	[SIGALRM] = ACTION_TERM,
	[SIGBUS] = ACTION_CORE,
	[SIGCHLD] = ACTION_IGN,
	[SIGCONT] = ACTION_CONT,
	[SIGFPE] = ACTION_CORE,
	[SIGHUP] = ACTION_TERM,
	[SIGILL] = ACTION_CORE,
	[SIGINT] = ACTION_TERM,
	[SIGIO] = ACTION_TERM,
	[SIGKILL] = ACTION_TERM,
	[SIGPIPE] = ACTION_TERM,
	[SIGPOLL] = ACTION_TERM,
	[SIGPROF] = ACTION_TERM,
	[SIGPWR] = ACTION_TERM,
	[SIGQUIT] = ACTION_CORE,
	[SIGSEGV] = ACTION_CORE,
	[SIGSTOP] = ACTION_STOP,
	[SIGTSTP] = ACTION_STOP,
	[SIGSYS] = ACTION_CORE,
	[SIGTERM] = ACTION_TERM,
	[SIGTRAP] = ACTION_CORE,
	[SIGTTIN] = ACTION_STOP,
	[SIGTTOU] = ACTION_STOP,
	[SIGURG] = ACTION_IGN,
	[SIGUSR1] = ACTION_TERM,
	[SIGUSR2] = ACTION_TERM,
	[SIGVTALRM] = ACTION_TERM,
	[SIGXCPU] = ACTION_CORE,
	[SIGXFSZ] = ACTION_CORE,
	[SIGWINCH] = ACTION_IGN
};

#define PROCESS_ENTER(proc) \
	bool procintstatus = interrupt_set(false); \
	spinlock_acquire(&proc->signals.lock);

#define PROCESS_LEAVE(proc) \
	spinlock_release(&proc->signals.lock); \
	interrupt_set(procintstatus);

#define THREAD_ENTER(thread) \
	bool threadintstatus = interrupt_set(false); \
	spinlock_acquire(&thread->signals.lock);

#define THREAD_LEAVE(thread) \
	spinlock_release(&thread->signals.lock); \
	interrupt_set(threadintstatus);

#define SIGNAL_ASSERT(s) __assert((s) < NSIG && (s) >= 0)

void signal_action(struct proc_t *proc, int signal, sigaction_t *new, sigaction_t *old) {
	SIGNAL_ASSERT(signal);
	PROCESS_ENTER(proc);

	if (old)
		*old = proc->signals.actions[signal];

	if (new)
		proc->signals.actions[signal] = *new;

	PROCESS_LEAVE(proc);
}

void signal_altstack(struct thread_t *thread, stack_t *new, stack_t *old) {
	THREAD_ENTER(thread);

	if (old)
		*old = thread->signals.stack;

	if (new)
		thread->signals.stack = *new;

	THREAD_LEAVE(thread);
}

void signal_changemask(struct thread_t *thread, int how, sigset_t *new, sigset_t *old) {
	THREAD_ENTER(thread);

	if (old)
		*old = thread->signals.mask;

	if (new) {
		switch (how) {
			case SIG_BLOCK:
			case SIG_UNBLOCK: {
				for (int i = 0; i < NSIG; ++i) {
					if (SIGNAL_GET(new, i) == 0)
						continue;

					if (how == SIG_BLOCK && SIGNAL_GET(&thread->signals.mask, i) == 0)
						SIGNAL_SETON(&thread->signals.mask, i);
					else if (how == SIG_UNBLOCK && SIGNAL_GET(&thread->signals.mask, i))
						SIGNAL_SETOFF(&thread->signals.mask, i);
				}
				break;
			}
			case SIG_SETMASK: {
				thread->signals.mask = *new;
				break;
			}
			default:
				__assert(!"bad how");
		}
	}

	THREAD_LEAVE(thread);
}

void signal_pending(struct thread_t *thread, sigset_t *sigset) {
	proc_t *proc = thread->proc;
	PROCESS_ENTER(proc);
	THREAD_ENTER(thread);

	// urgent set doesn't get returned here as it will always be handled
	// before a return to userspace
	memset(sigset, 0, sizeof(sigset_t));
	for (int i = 0; i < NSIG; ++i) {
		if (SIGNAL_GET(&proc->signals.pending, i))
			SIGNAL_SETON(sigset, i);
		
		if (SIGNAL_GET(&thread->signals.pending, i))
			SIGNAL_SETON(sigset, i);
	}

	THREAD_LEAVE(thread);
	PROCESS_LEAVE(proc);
}

void signal_signalthread(struct thread_t *thread, int signal, bool urgent) {
	PROCESS_ENTER(thread->proc);
	THREAD_ENTER(thread);

	sigset_t *sigset = urgent ? &thread->signals.urgent : &thread->signals.pending;
	SIGNAL_SETON(sigset, signal);

	void *address = thread->proc->signals.actions[signal].address;
	bool notignored = address != SIG_IGN && ((address == SIG_DFL && defaultactions[signal] != ACTION_IGN) || address != SIG_DFL);

	if (signal == SIGKILL || signal == SIGSTOP || urgent || (SIGNAL_GET(&thread->signals.mask, signal) == 0 && notignored)) {
		sched_wakeup(thread, SCHED_WAKEUP_REASON_INTERRUPTED);
	}

	THREAD_LEAVE(thread);
	PROCESS_LEAVE(thread->proc);
}

void signal_signalproc(struct proc_t *proc, int signal) {
	// first, check if any threads have the signal unmasked, and set as pending for the first one
	MUTEX_ACQUIRE(&proc->mutex, false);
	PROCESS_ENTER(proc);
	void *address = proc->signals.actions[signal].address;
	bool notignored = address != SIG_IGN && ((address == SIG_DFL && defaultactions[signal] != ACTION_IGN) || address != SIG_DFL);

	for (int i = 0; i < proc->threadtablesize; ++i) {
		if (proc->threads[i] == NULL || (proc->threads[i]->flags & SCHED_THREAD_FLAGS_DEAD))
			continue;

		THREAD_ENTER(proc->threads[i]);

		// sigcont will wake up all the threads or unset sigstop as pending
		if (signal == SIGCONT) {
			// TODO this
		} else if (SIGNAL_GET(&proc->threads[i]->signals.mask, signal) == 0 || signal == SIGKILL || signal == SIGSTOP) {
			SIGNAL_SETON(&proc->threads[i]->signals.pending, signal);
			if (notignored) {
				sched_wakeup(proc->threads[i], SCHED_WAKEUP_REASON_INTERRUPTED);
				// TODO ipi if running isn't self
			}
			THREAD_LEAVE(proc->threads[i]);
			PROCESS_LEAVE(proc);
			MUTEX_RELEASE(&proc->mutex);
			return;
		}

		THREAD_LEAVE(proc->threads[i]);
	}
	MUTEX_RELEASE(&proc->mutex);

	// there wasn't any thread with it unmasked, so set it as pending for the whole process

	SIGNAL_SETON(&proc->signals.pending, signal);

	PROCESS_LEAVE(proc);
}


void signal_check(struct thread_t *thread, context_t *context, bool syscall, uint64_t syscallret, uint64_t syscallerrno) {
	proc_t *proc = thread->proc;

	PROCESS_ENTER(proc);
	THREAD_ENTER(thread);

	if (SIGNAL_GET(&proc->signals.pending, SIGKILL) || SIGNAL_GET(&thread->signals.pending, SIGKILL)) {
		THREAD_LEAVE(thread);
		PROCESS_LEAVE(proc);
		proc->status = SIGKILL;
		interrupt_set(true);
		sched_threadexit();
	}

	// find the first pending signal which is unmasked

	int signal;
	sigset_t *sigset = NULL;

	// check the urgent sigset first
	for (int i = 0; i < NSIG; ++i) {
		if (SIGNAL_GET(&thread->signals.urgent, i) == 0)
			continue;

		signal = i;
		sigset = &thread->signals.urgent;
	}

	for (int i = 0; i < NSIG && sigset == NULL; ++i) {
		if (SIGNAL_GET(&thread->signals.mask, i))
			continue;

		if (SIGNAL_GET(&thread->signals.pending, i)) {
			signal = i;
			sigset = &thread->signals.pending;
			break;
		}

		if (SIGNAL_GET(&proc->signals.pending, i)) {
			signal = i;
			sigset = &proc->signals.pending;
			break;
		}
	}

	if (sigset == NULL)
		goto leave;

	sigaction_t *action = &proc->signals.actions[signal];
	SIGNAL_SETOFF(sigset, signal);

	// urgent signals cannot be ignored, if they are the default action will be run
	if (action->address == SIG_IGN && sigset != &thread->signals.urgent) {
		goto leave;
	} else if ((action->address == SIG_DFL && (action->flags & SA_SIGINFO) == 0) || (action->address == SIG_IGN && sigset == &thread->signals.urgent)) {
		// default action
		int defaction = defaultactions[signal];
		switch (defaction) {
			case ACTION_IGN:
				goto leave;
			case ACTION_CORE:
			case ACTION_TERM:
				proc->status = signal;
				THREAD_LEAVE(thread);
				PROCESS_LEAVE(proc);
				interrupt_set(true);
				sched_threadexit();
			default:
				__assert(!"unsupported signal action");
		}
	} else {
		// execute handler
		ARCH_CONTEXT_THREADSAVE(thread, context);

		void *altstack = NULL;
		// figure out the stack we will be running the signal handler in
		if (action->flags & SA_ONSTACK && thread->signals.stack.size > 0)
			altstack = thread->signals.stack.base;

		// get where in memory to put the frame
		void *stack = altstack ? altstack : (void *)CTX_SP(context);
		#if ARCH_SIGNAL_STACK_GROWS_DOWNWARDS == 1
		stack = (void *)(((uintptr_t)stack - ARCH_SIGNAL_REDZONE_SIZE - sizeof(sigframe_t)) & ~0xf);
		#else
			#error unsupported
		#endif

		// configure context for sigreturn() when a system call
		if (syscall) {
			CTX_ERRNO(context) = syscallerrno;
			CTX_RET(context) = syscallret;
			// TODO restartable system call stuff goes here
		}

		// configure stack frame
		sigframe_t *sigframe = stack;
		sigframe->restorer = action->restorer;
		if (altstack) {
			memcpy(&sigframe->oldstack, &thread->signals.stack, sizeof(stack_t));
			memset(&thread->signals.stack, 0, sizeof(stack_t));
		}
		memcpy(&sigframe->oldmask, &thread->signals.mask, sizeof(sigset_t));
		memcpy(&sigframe->context, context, sizeof(context_t));
		memcpy(&sigframe->extracontext, &thread->extracontext, sizeof(extracontext_t));
		// TODO siginfo

		// configure new thread signal mask
		if ((action->flags & SA_NODEFER) == 0)
			SIGNAL_SETON(&thread->signals.mask, signal);

		for (int i = 0; i < signal; ++i) {
			if (SIGNAL_GET(&action->mask, i) == 0)
				continue;

			SIGNAL_SETON(&thread->signals.mask, i);
		}

		// configure return context
		memset(context, 0, sizeof(context_t));
		CTX_INIT(context, true, true);
		CTX_IP(context) = (uint64_t)action->address;
		CTX_SP(context) = (uint64_t)stack;
		CTX_ARG0(context) = signal;
		CTX_ARG1(context) = 0; // TODO pass siginfo
		CTX_ARG2(context) = 0; // TODO ucontext

		// reset handler if asked for
		if (action->flags & SA_RESETHAND)
			memset(&action, 0, sizeof(sigaction_t));
	}

	leave:
	THREAD_LEAVE(thread);
	PROCESS_LEAVE(proc);
}