/*
 * Copyright 2018 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "wallClock.h"
#include "profiler.h"
#include "stackFrame.h"
#include <atomic>
#include <functional>
#include <chrono>


// Maximum number of threads sampled in one iteration. This limit serves as a throttle
// when generating profiling signals. Otherwise applications with too many threads may
// suffer from a big profiling overhead. Also, keeping this limit low enough helps
// to avoid contention on a spin lock inside Profiler::recordSample().
const int THREADS_PER_TICK = 8;

// Set the hard limit for thread walking interval to 100 microseconds.
// Smaller intervals are practically unusable due to large overhead.
const long MIN_INTERVAL = 100000;


long WallClock::_interval;
bool WallClock::_sample_idle_threads;

ThreadState WallClock::getThreadState(void* ucontext) {
    StackFrame frame(ucontext);
    uintptr_t pc = frame.pc();

    // Consider a thread sleeping, if it has been interrupted in the middle of syscall execution,
    // either when PC points to the syscall instruction, or if syscall has just returned with EINTR
    if (StackFrame::isSyscall((instruction_t*)pc)) {
        return THREAD_SLEEPING;
    }

    // Make sure the previous instruction address is readable
    uintptr_t prev_pc = pc - SYSCALL_SIZE;
    if ((pc & 0xfff) >= SYSCALL_SIZE || Profiler::instance()->findLibraryByAddress((instruction_t*)prev_pc) != NULL) {
        if (StackFrame::isSyscall((instruction_t*)prev_pc) && frame.checkInterruptedSyscall()) {
            return THREAD_SLEEPING;
        }
    }

    return THREAD_RUNNING;
}

/** waits as long as condition holds with a timeout, returns false if timeout is hit*/
bool waitWhile(std::function<bool()> condition, long timeout_ns = -1) {
    long start = OS::nanotime();
    while (condition()) {
        long diff = OS::nanotime() - start;
        if (timeout_ns != -1 && diff > timeout_ns) {
            return false;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    return true;
}

struct Data {
    std::atomic<void*> ucontext;
    std::atomic<JNIEnv*> jni;
};

std::atomic<int> _thread_id;
std::atomic<Data*> _thread_data;
std::atomic<bool> _thread_data_settable;
std::atomic<bool> _thread_data_ready;
std::atomic<bool> _stack_walked;

bool WallClock::walkStack(int thread_id) {
    // set the current thread
    _thread_id = thread_id;
    _thread_data = nullptr;
    _thread_data_settable = true;
    _stack_walked = false;
    _thread_data_ready = false;

    // send the signal to the sampled thread
    if (!OS::sendSignalToThread(thread_id, SIGVTALRM)) { // send signal to thread
        _thread_id = -1;
        return false;
    }
    // wait till the signal handler has set the ucontext and jni
    if (!waitWhile([&](){ return !_thread_data_ready;}, 10 * 1000 * 1000)) {
        _thread_id = -1;
        return false;
    }
    Data *data = _thread_data.load();
    // walk the stack
    ExecutionEvent event;
    event._thread_state = _sample_idle_threads ? getThreadState(data->ucontext) : THREAD_UNKNOWN;
    u64 ret = Profiler::instance()->recordSample(data->ucontext, _interval, EXECUTION_SAMPLE, &event, data->jni);
    // reset the ucontext, triggering the signal handler
    _stack_walked = true;
    return true;
}

void WallClock::signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
    Data* expected = nullptr;
    // check that we are in the thread we are supposed to be
    if (OS::threadId() != _thread_id) {
        return;
    }

    bool f = true;
    if (!_thread_data_settable.compare_exchange_strong(f, false)) {
        // another signal handler invocation is already in progress
        return;
    }
    Data data{ucontext, VM::jni()};
    // VM::jni() calls without a sleep afterwards cause problems
    // I'm unsure why
        // VM::jni() calls without a sleep afterwards cause problems
    // I'm unsure why
    std::atomic_thread_fence(std::memory_order_seq_cst);
    _thread_data = &data;
    _thread_data_ready = true;
    // wait for the stack to be walked, and block the thread from executing
    // we do not timeout here, as this leads to difficult bugs
    waitWhile([&](){ return !_stack_walked;});
}

long WallClock::adjustInterval(long interval, int thread_count) {
    if (thread_count > THREADS_PER_TICK) {
        interval /= (thread_count + THREADS_PER_TICK - 1) / THREADS_PER_TICK;
    }
    return interval;
}

Error WallClock::start(Arguments& args) {
    _sample_idle_threads = args._wall >= 0 || strcmp(args._event, EVENT_WALL) == 0;

    _interval = args._wall >= 0 ? args._wall : args._interval;
    if (_interval == 0) {
        // Increase default interval for wall clock mode due to larger number of sampled threads
        _interval = _sample_idle_threads ? DEFAULT_INTERVAL * 5 : DEFAULT_INTERVAL;
    }

    OS::installSignalHandler(SIGVTALRM, signalHandler);

    _running = true;

    if (pthread_create(&_thread, NULL, threadEntry, this) != 0) {
        return Error("Unable to create timer thread");
    }

    return Error::OK;
}

void WallClock::stop() {
    _running = false;
    pthread_kill(_thread, WAKEUP_SIGNAL);
    pthread_join(_thread, NULL);
}

void WallClock::timerLoop() {
    int self = OS::threadId();
    ThreadFilter* thread_filter = Profiler::instance()->threadFilter();
    bool thread_filter_enabled = thread_filter->enabled();
    bool sample_idle_threads = _sample_idle_threads;

    ThreadList* thread_list = OS::listThreads();
    long long next_cycle_time = OS::nanotime();

    while (_running) {
        if (!_enabled) {
            OS::sleep(_interval);
            continue;
        }

        if (sample_idle_threads) {
            // Try to keep the wall clock interval stable, regardless of the number of profiled threads
            int estimated_thread_count = thread_filter_enabled ? thread_filter->size() : thread_list->size();
            next_cycle_time += adjustInterval(_interval, estimated_thread_count);
        }

        for (int count = 0; count < THREADS_PER_TICK; ) {
            int thread_id = thread_list->next();
            if (thread_id == -1) {
                thread_list->rewind();
                break;
            }

            if (thread_id == self || (thread_filter_enabled && !thread_filter->accept(thread_id))) {
                continue;
            }

            if (sample_idle_threads || OS::threadState(thread_id) == THREAD_RUNNING) {
                if (walkStack(thread_id)) {
                    count++;
                }
            }
        }

        if (sample_idle_threads) {
            long long current_time = OS::nanotime();
            if (next_cycle_time - current_time > MIN_INTERVAL) {
                OS::sleep(next_cycle_time - current_time);
            } else {
                next_cycle_time = current_time + MIN_INTERVAL;
                OS::sleep(MIN_INTERVAL);
            }
        } else {
            OS::sleep(_interval);
        }
    }

    delete thread_list;
}
