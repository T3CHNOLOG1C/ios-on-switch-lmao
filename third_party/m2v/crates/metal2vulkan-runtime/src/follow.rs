//! The runtime's threads that take a command buffer from its commit to its handlers
//! ([`exit_guard::spawn_user_interactive`](crate::exit_guard)) follow the threads that commit:
//! where a committing thread runs at a fixed priority above theirs (`pthread_setschedparam`'s
//! `SCHED_FIFO` or `SCHED_RR`), each of them takes its policy and priority.
//!
//! iOS only. Core Animation's render server commits from fixed-priority threads (QuartzCore's
//! `CA::Render::Server::set_thread_priority`: `SCHED_FIFO` 55 for the render server, 54 for the
//! display) and waits for the scheduling each frame, while XNU clamps a daemon's user-interactive
//! class to user-initiated (37): at the class alone these threads ran below the thread waiting on
//! them, level with every busy daemon. A committer at a quality of service class changes nothing,
//! so an app's threads keep the user-interactive class.

#[cfg(target_os = "ios")]
mod imp {
    use std::cell::{Cell, RefCell};
    use std::sync::{Mutex, MutexGuard, PoisonError};

    /// `<sys/sched.h>`: `SCHED_OTHER`, and `struct sched_param` with its opaque tail.
    const SCHED_OTHER: libc::c_int = 1;

    #[repr(C)]
    struct SchedParam {
        sched_priority: libc::c_int,
        opaque: [libc::c_char; 4],
    }

    extern "C" {
        fn pthread_getschedparam(
            thread: libc::pthread_t,
            policy: *mut libc::c_int,
            param: *mut SchedParam,
        ) -> libc::c_int;
        fn pthread_setschedparam(
            thread: libc::pthread_t,
            policy: libc::c_int,
            param: *const SchedParam,
        ) -> libc::c_int;
    }

    struct Followers {
        threads: Vec<libc::pthread_t>,
        policy: libc::c_int,
        priority: libc::c_int,
    }

    static FOLLOWERS: Mutex<Followers> = Mutex::new(Followers {
        threads: Vec::new(),
        policy: SCHED_OTHER,
        priority: 0,
    });

    fn followers() -> MutexGuard<'static, Followers> {
        FOLLOWERS.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// A follower's place in the list, given up as its thread ends (pool threads come and go).
    struct Member(libc::pthread_t);

    impl Drop for Member {
        fn drop(&mut self) {
            followers().threads.retain(|&thread| thread != self.0);
        }
    }

    thread_local! {
        static MEMBER: RefCell<Option<Member>> = const { RefCell::new(None) };
    }

    fn apply(thread: libc::pthread_t, policy: libc::c_int, priority: libc::c_int) {
        let param = SchedParam {
            sched_priority: priority,
            opaque: [0; 4],
        };
        // SAFETY: `thread` is one of the runtime's threads still running: each leaves the list as
        // it ends ([`Member`]). `param` lives for the call.
        unsafe { pthread_setschedparam(thread, policy, &param) };
    }

    pub(crate) fn join() {
        // SAFETY: no argument.
        let me = unsafe { libc::pthread_self() };
        {
            let mut followers = followers();
            followers.threads.push(me);
            if followers.policy != SCHED_OTHER {
                apply(me, followers.policy, followers.priority);
            }
        }
        MEMBER.with(|member| *member.borrow_mut() = Some(Member(me)));
    }

    pub(crate) fn committing() {
        thread_local! {
            static FIXED: Cell<bool> = const { Cell::new(false) };
        }
        // libpthread keeps a thread's policy in its pthread (the read makes no call). A
        // fixed-priority committer is read once; a timeshare one at each commit, since Core
        // Animation sets its threads' priority from the thread that created them, which can come
        // after the new thread's first commit.
        if FIXED.with(Cell::get) {
            return;
        }
        let mut policy: libc::c_int = SCHED_OTHER;
        let mut param = SchedParam {
            sched_priority: 0,
            opaque: [0; 4],
        };
        // SAFETY: reads the calling thread's policy into `policy` and `param`, which live for the
        // call.
        let read = unsafe { pthread_getschedparam(libc::pthread_self(), &mut policy, &mut param) };
        if read != 0 || policy == SCHED_OTHER {
            return;
        }
        FIXED.with(|fixed| fixed.set(true));
        let mut followers = followers();
        if followers.policy != SCHED_OTHER && param.sched_priority <= followers.priority {
            return;
        }
        followers.policy = policy;
        followers.priority = param.sched_priority;
        for &thread in &followers.threads {
            apply(thread, policy, param.sched_priority);
        }
    }
}

#[cfg(not(target_os = "ios"))]
mod imp {
    pub(crate) fn join() {}

    pub(crate) fn committing() {}
}

/// On one of the runtime's threads that take command buffers to their handlers, as it starts.
pub(crate) use imp::join;

/// On the thread that commits a command buffer.
pub(crate) use imp::committing;
