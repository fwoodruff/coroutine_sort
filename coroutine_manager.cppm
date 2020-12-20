
raw download edit delete
// copyright (c) 2020 - Freddie Woodruff
 
import <coroutine>
import <exception>
import <iterator>
import <concepts>
import <ranges>
import <atomic>
import <string>
import <queue>
import <mutex>
 
/*
 
This module provides a utility for sorting coroutines by the
order they will complete.
 
This improves the concurrency and eagerness of coroutine code.
Coroutines that complete asynchronously are run concurrently.
 
Public interface:
 
template<typename T, 
    template<typename> typename Queue = DEFAULT > 
struct manager {
    struct consumable {
        bool await_ready();
        auto await_suspend(std::coroutine_handle<>);
        T await_resume();
    };
    
    struct awaiter_view {
        struct sentinel;
        struct iterator {
            consumable operator*();
            iterator& operator++();
        };
        
        iterator begin();
        sentinel end();
    };
 
    manager();
    
    template<Awaitable A>
    void submit(A);
    
    awaiter_view view();
};
 
Manager objects are noncopyable, single-use, exception-safe and
thread-safe.
 
Usage:
 
using namespace cppcoro; // See Lewis Baker's cppcoro library
task<int> wait_and_return(int x, static_thread_pool& tp) {
    co_await tp;
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(x * 50ms);
    co_return x;
}
 
task<> co_main() {
    static_thread_pool tp;
    fbw::manager time_sort;
    std::array<int, 5> unsorted {8, 4, 3, 1, 6};
    try {
        for(auto i : unsorted) {
            time_sort.submit(wait_and_return(i, tp));
        }
        for(auto consumable : time_sort.view()) {
            int i = co_await consumable;
            std::cout << i << std::endl;
        }
    } catch(std::exception e) {
        std::cerr << e.what() << std::flush;
    }
}
 
int main () {
    sync_wait(co_main());
}
 
Possible output:
1
3
4
6
8
 
*/
    
namespace fbw {
    
concept Containable = std::moveable and
                      std::default_initializable;
 
template<typename T>
concept Awaitable = 
requires(T a, std::coroutine_handle<> h)
              { a.await_suspend(h)} &&
requires(T a) { a.await_resume()  } -> Containable &&
requires(T a) { a.await_ready ()  } -> bool;
    
template<typename T, typename U>
concept Promise =
 requires(T p)     { p.initial_suspend()     } -> Awaitable &&
 requires(T p)     { p.final_suspend()       } -> Awaitable &&
 requires(T p)     { p.unhandled_exception() } -> void      &&
(requires(T p)     { p.return_void ( )       } -> void      ||
 requires(T p, U v){ p.return_value(v)       } -> void   );
    
template<typename T>
concept Coroutine_Future = 
requires(T future) { Promise T::promise_type };
    
template<typename Q, std::moveable T>
concept Thread_Safe_Queue = requires (Q q) {
    q = Q();
    T val = q.dequeue(); 
    q.enqueue(val);
};
    
// Simple queue following the thread-safe queue concept.
// Concepts cannot enforce thread-safety but pop() and front()
// methods must be combined.
template<Containable T>
class standard_mutex_queue {
public:
    enqueue(T value) {
        std::scoped_lock lock(global_mutex)
        standard_queue.push(std::move(value));
        
    }
    T dequeue() {
        std::scoped_lock lock(global_mutex);
        standard_queue.pop();
        assert(!standard_queue.empty());
        return standard_queue.front();
    }
    bool empty() {
        return standard_queue.empty();
    }
private:
    std::queue standard_queue;
    std::mutex global_mutex;
};
 
template<Containable T>
class expected final {
public:
    expected(T value) :
         expected_result(std::move(value)) {}
    expected(exception_ptr exception) noexcept : 
         expected_result(std::move(exception)) {}
    expected() noexcept : expected_result(){}
    T get() const { 
       if(auto exception = std::get_if<
                  std::exception_ptr>(&expected_result)) {
           throw *exception;
       }
       return std::get<T>(result);
    }
private:
    std::variant<T, exception_ptr> expected_result;
};
    
template<typename T>
class move_only_interface {
public:
    T(const T&) = delete;
    T& operator=(const T&) = delete;
    T(T&&) = default;
    T& operator=(T&&) = default;
};
    
    
/*
If the queue used is lock-free, the manager is lock-free 
overall.
*/
template <Containable T,
          template<Containable> Thread_Safe_Queue
          Queue = standard_mutex_queue>
export class [[nodiscard("submit")]] manager final {
private:
    using enum memory_order;
    /*
    See definition below
    */
    [[nodiscard("resume")]] std::coroutine_handle<>
    pair_off(uint_least64_t current_count);
    
    /*
    When a coroutine co_returns, we enqueue the result for
    consumption, before trying to pair the result with an
    awaiting consumer.
    */
    void enqueue(expected<T> value) {
        results.enqueue(std::move(value));
        auto current_count = dual_counter.fetch_add(1, release);
        auto continuation = pair_off(current_count);
        continuation.resume();
    }
    
    /*
    enqueue_task objects satisfy the Coroutine_Future concept 
    and transfer co_returned results to the manager queue.
    */
    class [[nodiscard("start")]] enqueue_task {
        class promise_type;
        using co_handle_t = std::coroutine_handle<promise_type>;
    public:
        explicit enqueue_task(co_handle_t coroutine) :
                                producer(coroutine) {}
        ~enqueue_task() {
            if(producer) {
                producer.destroy();
            } 
        }
        enqueue_task(const enqueue_task&) = delete;
        enqueue_task& operator=(const enqueue_task&) = delete;
                   
        class promise_type {
        public:
            std::suspend_always initial_suspend()
                const noexcept {return{};}
            std::suspend_always final_suspend()
                const noexcept {return{};}
            /*
            If moving the task into the manager queue fails,
            or an exception escapes the user-provided coroutine,
            unhandled_exception attempts to enqueue an
            exception_ptr which is rethrown during consumption.
            */
            void unhandled_exception() const {
                context->enqueue({
                       std::current_exception()});
            }
            promise_type(const promise_type&) = delete;
            promise_type& operator=(const promise_type&) 
                                                   = delete;
            /*
            Return values are transferred to the manager.
            */
            void return_value(T value) const {
                context->enqueue({std::move(value)});
            }
        private:
            manager* context;
        };
        
        void start(manager* manager) const {
            producer.promise().context = manager;
            producer.resume();
        }
    private:
        co_handle_t producer;
    };
    static_assert(Coroutine_Future<enqueue_task>);
    
    /*
    This is a coroutine that enqueues the result of
    an input coroutine on completion.
    */
    template<Awaitable A>
    enqueue_task make_enqueue_task(A task) {
        co_return co_await std::forward<A>(task);
    }
    
    
    /*
    Counts the number of tasks submitted. The two
    most-significant bits are reserved for a kill-switch
    to ensure no tasks are submitted after requesting a view.
    */
    std::atomic<uint_least64_t> submission_count;
    
    /* 
    We need to set two high bits due to the potential
    race between increments and decrements in submit() once
    exceptions are being thrown with relaxed memory ordering.
    */
    static constexpr uint_least64_t dead_count = 0b11ULL << 62;
    
    /*
    MPMC queue of continuations that can be 
    resumed when we have task return values.
    This must be a queue in case a user decides to pass
    awaitables between threads.
    */
    Queue<consumable> awaitables;
    
    /*
    Queue of return values.
    Note that to an outside observer, only one of these queues
    is ever non-empty.
    */
    Queue<expected<T>> results;
    
    /*
    bits 0-31 for the value queue
    bits 32-63 for the continuations queue
    dual_counter is integral which allows us to use
    fetch_add operations and avoid many CAS-retry loops.
    */
    std::atomic<uint_least64_t> dual_counter;
 
    
public:
    /*
    */
    manager() : awaitables(), results(),
                submission_count(0), dual_counter(0) {}
    
    /*
    Submit awaitables and start them eagerly.
    */
    template<Awaitable A>
    void submit(A task) {
        const auto count =
            submission_count.fetch_add(1, relaxed);
        if(count & dead_count) {
            // code path unreachable until manager::view is
            // called
            submission_count.fetch_sub(1, relaxed);
            throw std::logic_error("Work cannot be submitted "
                                   "to a finished manager.\n");
        }
        const auto enqueue_task =
                      make_enqueue_task(std::move(task));
        enqueue_task.start(this);
    }
    
    /*
    consumable objects satisfy the Awaitable concept.
    Calling co_await on the same consumable from multiple
    threads causes undefined behaviour.
    It is safe but not necessarily expressive to concurrently
    co_await on different awaitables obtained from the same
    view.
    */
    class [[nodiscard("co_await")]] consumable final :
        public move_only_interface<consumable> {
    public:
        consumable(manager* manager) noexcept : 
                                 context(manager),
                                 awaited_result(),
                                 consumer(nullptr){}
        
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<> coroutine) {
            if(consumer) {
                throw std::logic_error("Coroutine already "
                                       "awaited.\n");
            }
            consumer = coroutine;
            
            // This emphasises that we are using context
            // after it has been moved-from.
            static_assert(
               std::is_trivially_move_constructible<manager*>);
            
            context->awaitables.enqueue(std::move(*this));
            const auto current_count = 
              context->
                  dual_counter.fetch_add(1ULL << 32, release);
            return context->pair_off(current_count);
        }
                
        bool await_ready() const noexcept {
            return false;
        }
        /*
        This resumes the coroutine with the result
        from a task, rethrowing if the task threw.
        */
        T await_resume() const {
            return awaited_result.get();
        }
    private:
        std::coroutine_handle<> consumer;
        expected<T> awaited_result;
        manager* context;
    };
    static_assert(Awaitable<consumable>);
 
    /*
    awaiter_view objects satisfy the ranges::view concept with
    iterators returned from begin() that satisfy the
    forward_iterator concept.
    */
    class awaiter_view final :
         public std::ranges::view_interface<awaiter_view> {
    public:
        struct sentinel {};
        class iterator final : move_only_interface<iterator> {
        public:
            iterator(manager* manager_ptr,
                     uint_least64_t sentinel_count) : 
                      context(manager_ptr),
                      count(0),
                      end(sentinel_count)
                      {}
            
            friend auto operator<=>(const iterator& it,
                              const sentinel&) noexcept {
                return count <=> end;
            }
            /*
            operator++ counts up to the number of
            tasks submitted.
            */
            iterator& operator++() {
                ++count;
                return *this;
            }
            void operator++(int) {
                (void)operator++();
            }
            consumable operator*() const noexcept {
                return {context};
            }
            auto operator->() const noexcept {
                return std::make_shared<consumable>({context});
            }
        private:
            manager* context;
            intptr_t count;
            intptr_t end;
        };
        static_assert(std::forward_iterator<iterator>);
        
        iterator begin() const noexcept {
            return {context, task_count};
        }
        
        sentinel end() const noexcept {
            return {};
        }
        awaiter_view(manager* manager_ptr, 
                     uint_least64_t count) :
                                   context(manager_ptr),
                                   task_count(count) {}                               
    private:
        manager* context;
        uint_least64_t task_count;
    };
    static_assert(std::ranges::view<awaiter_view>);
    
    /*
    view disables further task submissions inter-thread.
    An awaiter_view object over the consumables
    produced by submitted tasks is returned.
    */
    auto view() const noexcept {
        /*
        submission_count is atomic and we only care about the
        set of values it can take, not the order it takes them
        in. Operations may therefore have relaxed memory 
        ordering.
        */
        const auto count = submission_count
                                .exchange(dead_count, relaxed);
        if(count & dead_count) {
            throw std::logic_error("Manager has already "
                                   "been viewed.\n");
        }
        return awaiter_view{this, count};
    }
};
                
 
/*
pair_off attempts to decrease both the consumables and
results parts of the dual_count atomically to address
the dining philosophers' problem.
If the CAS succeeds, we have rights to ownership for both 
a result and a consumable, which we can dequeue, pair and
resume.
*/
template<Containable T,
         template<Containable> Thread_Safe_Queue Queue>
std::coroutine_handle<>
  manager<T, Queue>::pair_off(uint_least64_t current_count) {
    // await_suspend resumes a coroutine which can lead to
    // stack-overflow if we do not use symmetric transfer.
    uint_least64_t decrease;
    do {
       // If upper 32 bits are not all zero and lower 32 bits
       // are not all zero, subtract one from both the upper
       // and lower bits.
       decrease = ((current_count & 0xffffffffULL) &&
                current_count >> 32)) * 0x100000001ULL;
       const auto new_count = current_count - decrease;
       // CAS in this new value to claim ownership rights 
       // for both (or neither) a value and a continuation.
    } while(!dual_counter.compare_exchange_weak(current_count,
                                                new_count,
                                              acquire, relaxed);
    /*
    Swapped reasoning applies for awaitable::await_suspend().
    When manager::enqueue() calls pair_off(), values.dequeue()
    is sequenced-before values.enqueue(). awaitables.enqueue()
    is sequenced-before the fetch_add on count. Release-acquire
    semantics ensures this fetch_add happens-before any
    compare_exchange on count that decrements the counter.
    If the counter is decremented, this is sequenced-before 
    awaitables.dequeue().
    Dequeuing will therefore never encounter an empty queue.
    
    Release-consume semantics would be inappropriate because
    the queue does not carry-dependency from dual_count.
 
    Operations on count are atomic. Therefore one CAS decrement
    always succeeds following both a 'result' and a 
    'consumable' fetch_add (in manager::enqueue() and
    consumable::await_suspend()).
    This is best illustrated by a situation that
    cannot occur:
         
    Consider:
        count = 0.
        One process increments the 'value' count.
        count = 0x1.
        Another process increments the 'continuation' count, the
        'value' increment not visible.
        count = 0x100000000.
         
    Both FAAs are sequenced-before the pair-off CAS.
    In the above, CAS does not decrement count for either
    process. However the value was torn, which
    cannot occur for atomic variables.
         
    Values will always therefore be paired-off with 
    a continuation:
    This is the purpose of the split count.
         
    */
    if(decrease) {
       auto current_awaitable = awaitables.dequeue();
       auto value = results.dequeue();
       current_awaitable.awaited_result = std::move(value);
       return current_awaitable.consumer;
    }
    return std::noop_coroutine_handle;
}
} // namespace fbw
