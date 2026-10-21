#pragma once

// The asynchronous face of uniorm. Deliberately outside <uniorm/uniorm.hpp>:
// coroutines would otherwise enter every translation unit that includes the
// umbrella header. Nothing here opens a second database path -- a task body
// calls the synchronous code it wraps, on a thread the caller does not wait on.

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <uniorm/error.hpp>
#include <uniorm/export.hpp>
#include <uniorm/orm.hpp>
#include <uniorm/transaction.hpp>

namespace uniorm::async {

// Where a task's body runs. The core owns the default pool; a caller with a
// loop of its own -- or a test that wants to choose the moment work happens --
// hands over another executor. submit() hands the work off and returns: it runs
// nothing here, and it blocks nobody.
class UNIORM_API executor {
public:
  virtual ~executor();
  virtual void submit(std::function<void()> work) = 0;
};

// The pool a task runs on when nothing else was arranged. Its worker count is
// the ceiling on how many operations are in flight at once: a queue deeper than
// that is waiting, not going in parallel.
UNIORM_API executor& default_executor();

// The size the pool is created with, if it does not exist yet. After that it
// only grows: a worker that has entered a driver call cannot be lifted out of
// it, so shrinking is never offered.
UNIORM_API void default_executor_size(std::size_t threads);

template <class T>
class task;

class orm;

class transaction;

template <class T>
class query;

namespace detail {

template <class T>
struct result_slot {
  std::optional<T> value;

  T move_out() {
    return std::move(*value);
  }
};

template <>
struct result_slot<void> {
  void move_out() const noexcept {}
};

enum class task_drive { idle, blocked, detached, awaited };

// One per session: the FIFO that keeps a connection down to a single operation
// in flight. It answers "who is next on this connection"; the executor's own
// queue answers "which thread is free", and a request waiting here holds no
// thread at all. The bodies sit in `src/async/queue.cpp`; the class is exported
// because a task's drive action reaches it from a template in this header.
class UNIORM_API operation_queue {
public:
  static constexpr std::size_t default_submit_limit = 1024;

  using starter = std::function<void()>;

  operation_queue(operation_queue const&) = delete;
  operation_queue& operator=(operation_queue const&) = delete;

  std::size_t submit_limit() const noexcept;

  void submit_limit(std::size_t count) noexcept;

  // Requests queued plus the one running: what ~orm waits to see at zero.
  std::size_t pending() const;

  void push(starter start);

  // The running operation is over, however it ended. Frees the slot and takes
  // the next request unless a loop here already is walking the queue.
  void finish() noexcept;

  void wait_idle();

private:
  operation_queue() = default;
  friend class uniorm::async::orm;

  // Entered with the lock held, which is dropped around each start(): an
  // executor that runs the work right here has its operation call finish()
  // from inside start(), and draining_ is what keeps that nested call from
  // putting a second operation on this connection.
  void drain(std::unique_lock<std::mutex>& lock);

  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<starter> waiting_;
  std::atomic<std::size_t> limit_ = default_submit_limit;
  bool in_flight_ = false;
  bool draining_ = false;
};

template <class T>
struct task_promise;

// One per task, on the heap: the result, and the bookkeeping of how it leaves.
// The frame hangs off this and the promise points back with a bare pointer, so
// the core always outlives the frame -- a promise destructor must never read
// through that pointer.
template <class T>
struct operation_core
  : result_slot<T>, std::enable_shared_from_this<operation_core<T>> {
  std::coroutine_handle<task_promise<T>> frame;
  std::exception_ptr failure;
  std::binary_semaphore finished{ 0 };
  std::coroutine_handle<> continuation;
  std::function<void(std::exception_ptr)> on_failure;
  operation_queue* queue = nullptr;
  executor* run_on = nullptr;
  executor* resume_on = nullptr;
  task_drive driven = task_drive::idle;

  operation_core() = default;
  operation_core(operation_core const&) = delete;
  operation_core& operator=(operation_core const&) = delete;
  ~operation_core() {
    if (frame) {
      frame.destroy();
    }
  }

  // The result lands here rather than in the frame, so a thread waiting on it
  // and the thread that finally destroys the frame cannot get in each other's
  // way: whoever drops the last reference does the destroying, and nobody else
  // touches the frame after its last resume returned.
  T take() {
    if (failure) {
      std::rethrow_exception(failure);
    }
    return this->move_out();
  }

  // Exactly one drive action per task: a second one would mean two threads
  // waiting for the same body.
  void claim(task_drive mode) {
    if (driven != task_drive::idle) {
      throw uniorm_error("async task: driven twice");
    }
    driven = mode;
  }

  // Nothing was submitted, so nothing can be waiting: put the task back where
  // it was, and a caller who was refused by a full queue can drive it again.
  void unclaim() noexcept {
    driven = task_drive::idle;
  }

  // The session's FIFO decides when this body gets a thread. With no session
  // behind the task, it starts on the spot.
  void launch(executor* requested) {
    if (queue) {
      auto self = this->shared_from_this();
      queue->push([self, requested] { self->begin(requested); });
      return;
    }
    begin(requested);
  }

  // The session's own executor wins over the one named on the drive action:
  // run_on is a per-session decision, and a drive action that names an
  // executor only has a say when no session made one.
  void begin(executor* requested) {
    executor& target =
      run_on ? *run_on : (requested ? *requested : default_executor());
    auto self = this->shared_from_this();
    try {
      target.submit([self] { self->frame.resume(); });
    } catch (...) {
      self->fail_before_start(std::current_exception());
    }
  }

  // The continuation goes to the executor the session named for resuming, so
  // the thread that ran the body is free to take the next queued operation.
  // The core travels with the closure: the frame the parent owns is reached
  // through it. A hand-off that throws has no one to report to -- the body
  // already landed -- so this thread takes the continuation itself.
  void hand_over() {
    auto self = this->shared_from_this();
    auto parent = continuation;
    std::exception_ptr refused;
    try {
      resume_on->submit([self, parent] { parent.resume(); });
    } catch (...) {
      refused = std::current_exception();
    }
    if (refused) {
      parent.resume();
    }
  }

  // The caller's handler is the last thing between a failed operation and the
  // thread that owns it, and both places that run it forbid throwing: the
  // final awaiter is noexcept, and a starter that unwinds leaves the queue
  // mid-walk. So it runs here, and whatever it throws stops here.
  void deliver_failure() noexcept {
    auto report = std::move(on_failure);
    if (!report || !failure) {
      return;
    }
    try {
      report(failure);
    } catch (...) {
    }
  }

  // The body never got a thread, so the hand-off failure becomes the
  // operation's own: free the slot and let the waiter take the exception.
  // Resumed rather than symmetrically transferred, because this path already
  // gave up on a flat stack.
  void fail_before_start(std::exception_ptr error) {
    failure = error;
    if (queue) {
      queue->finish();
    }
    switch (driven) {
    case task_drive::blocked:
      finished.release();
      break;
    case task_drive::detached:
      deliver_failure();
      break;
    case task_drive::awaited:
      continuation.resume();
      break;
    case task_drive::idle:
      break;
    }
  }
};

// A promise that declares both return_value and return_void is refused, so the
// two halves of the promise sit in two halves of a base: the primary template
// hands a body's value to the core, the void specialization accepts only a body
// that returns nothing.
template <class T>
struct task_promise_base {
  // Set by get_return_object, which runs before the body can.
  operation_core<T>* core_ = nullptr;

  void unhandled_exception() {
    core_->failure = std::current_exception();
  }
};

template <class T>
struct task_promise_of : task_promise_base<T> {
  // Built in place rather than assigned: a delivered value only has to be
  // movable, and a transaction handle that moves is not one that assigns.
  void return_value(T value) {
    this->core_->value.emplace(std::move(value));
  }
};

template <>
struct task_promise_of<void> : task_promise_base<void> {
  void return_void() const noexcept {}
};

template <class T>
struct task_promise : task_promise_of<T> {
  // The body has not begun: the frame waits here until a drive action starts
  // it, which is why a task nobody drives can simply be dropped.
  std::suspend_always initial_suspend() const noexcept {
    return {};
  }

  task<T> get_return_object();

  struct final_awaiter {
    bool await_ready() const noexcept {
      return false;
    }

    // Running the continuation by symmetric transfer keeps the stack flat: a
    // chain of awaited operations costs one frame deep, not one per link.
    std::coroutine_handle<> await_suspend(
      std::coroutine_handle<task_promise> self) const noexcept;

    void await_resume() const noexcept {}
  };

  final_awaiter final_suspend() const noexcept {
    return {};
  }
};

template <class T>
std::coroutine_handle<> task_promise<T>::final_awaiter::await_suspend(
  std::coroutine_handle<task_promise> self) const noexcept {
  auto& core = *self.promise().core_;
  // The slot is handed on before the result is delivered, so the next request
  // is already moving by the time a thread waiting on block() wakes.
  if (core.queue) {
    core.queue->finish();
  }
  switch (core.driven) {
  case task_drive::blocked:
    core.finished.release();
    return std::noop_coroutine();
  case task_drive::detached:
    core.deliver_failure();
    return std::noop_coroutine();
  case task_drive::awaited:
    // Without a resume executor the worker that finished the body takes the
    // next link of the chain, which is what keeps a chain of awaited
    // operations one frame deep instead of one thread hand-off per link.
    if (core.resume_on) {
      core.hand_over();
      return std::noop_coroutine();
    }
    return core.continuation;
  case task_drive::idle:
    break;
  }
  return std::noop_coroutine();
}

// The awaiter of a task: the parent coroutine parks here and is resumed from
// whichever thread ran the body.
template <class T>
struct task_awaiter {
  std::shared_ptr<operation_core<T>> core;

  bool await_ready() const noexcept {
    return false;
  }

  // Handle rather than void, so that a refusal transfers to the parent instead
  // of unwinding out of whoever is running this frame.
  std::coroutine_handle<> await_suspend(
    std::coroutine_handle<> parent) const {
    core->continuation = parent;
    try {
      core->claim(task_drive::awaited);
      core->launch(nullptr);
    } catch (...) {
      // The queue refused this turn before it reached an executor, so the
      // refusal is this await's own failure: the parent takes it from
      // await_resume, where a body can catch it like any other operation.
      core->unclaim();
      core->failure = std::current_exception();
      return parent;
    }
    return std::noop_coroutine();
  }

  T await_resume() const {
    return core->take();
  }
};

}  // namespace detail

// A piece of work not yet done. Lazy: making one runs nothing. One of three
// drive actions starts the body -- co_await inside another coroutine, block()
// to park this thread until it is over, or start() to let it run while the
// caller keeps going. Dropping an undriven task is legal and has no effect.
template <class T>
class task {
public:
  using promise_type = detail::task_promise<T>;
  using value_type = T;

  task(task&&) noexcept = default;
  task& operator=(task&&) noexcept = default;
  task(task const&) = delete;
  task& operator=(task const&) = delete;

  // Park here until the body is over, then hand back its result or rethrow
  // what it threw. The overload that names an executor is the one a test of
  // the mechanism uses; a task a session made runs on that session's executor.
  T block() {
    park(nullptr);
    return core_->take();
  }

  T block(executor& run_on) {
    park(&run_on);
    return core_->take();
  }

  // Fire and forget. on_failure runs -- and only runs -- if the body threw, so
  // a failed write cannot go unnoticed by the shape of the call alone.
  void start(std::function<void(std::exception_ptr)> on_failure) {
    detach(nullptr, std::move(on_failure));
  }

  void start(
    executor& run_on, std::function<void(std::exception_ptr)> on_failure) {
    detach(&run_on, std::move(on_failure));
  }

  detail::task_awaiter<T> operator co_await() const {
    return { core_ };
  }

private:
  friend struct detail::task_promise<T>;
  friend class orm;

  explicit task(std::shared_ptr<detail::operation_core<T>> core) noexcept
    : core_(std::move(core)) {}

  // Stamped by the session that made the task: every drive action on it then
  // goes through that session's FIFO, which is why no operation body needs to
  // know a queue exists.
  void bind(detail::operation_queue* gate, executor* target,
    executor* resumer) noexcept {
    core_->queue = gate;
    core_->run_on = target;
    core_->resume_on = resumer;
  }

  // One claim, one hand-off. A hand-off the queue refused leaves the task
  // undriven, so catching submit_overflow and trying later is a legal shape.
  void park(executor* requested) {
    core_->claim(detail::task_drive::blocked);
    try {
      core_->launch(requested);
    } catch (...) {
      core_->unclaim();
      throw;
    }
    core_->finished.acquire();
  }

  void detach(
    executor* requested, std::function<void(std::exception_ptr)> on_failure) {
    core_->on_failure = std::move(on_failure);
    core_->claim(detail::task_drive::detached);
    try {
      core_->launch(requested);
    } catch (...) {
      core_->unclaim();
      throw;
    }
  }

  std::shared_ptr<detail::operation_core<T>> core_;
};

namespace detail {

template <class T>
task<T> task_promise<T>::get_return_object() {
  auto core = std::make_shared<operation_core<T>>();
  core->frame = std::coroutine_handle<task_promise<T>>::from_promise(*this);
  this->core_ = core.get();
  return task<T>(std::move(core));
}

// The one body every asynchronous operation shares: it calls the synchronous
// function it was handed and lets the frame turn a throw into the task's own
// failure, which is why an operation's body never has to know about a queue.
template <class R>
task<R> run_sync(std::function<R()> work) {
  if constexpr (std::is_void_v<R>) {
    work();
    co_return;
  } else {
    co_return work();
  }
}

// Whether the session a transaction handle points back at is still there. The
// session closes it in ~orm() while its own connection is still alive, so a
// handle that outlives the session has something safe to read.
struct session_door {
  std::atomic<bool> open = true;
};

// The callback face of a session: the operation is the very call the coroutine
// face hands the queue, so nothing here reaches the driver by a second path.
template <class R>
struct callback_guard {
  std::function<void(R, std::exception_ptr)> on_done;
  bool settled = false;

  // One visit to the caller's callback, whichever of the two ways an answer
  // arrives gets to make it.
  void settle(R value, std::exception_ptr error) {
    if (settled) {
      return;
    }
    settled = true;
    on_done(std::move(value), error);
  }
};

// Named rather than a generic lambda: a coroutine has to spell its return type.
// The answer is part of the body, so the callback has come back before this
// operation gives up its turn -- which is what makes ~orm a barrier over
// callbacks without a second count to wait on, and what makes a slow callback
// hold up the next request on the same session.
template <class R>
task<void> answered(std::function<R()> work,
  std::shared_ptr<callback_guard<R>> guard) {
  R value{};
  std::exception_ptr error;
  try {
    value = work();
  } catch (...) {
    error = std::current_exception();
  }
  guard->settle(std::move(value), error);
  co_return;
}

}  // namespace detail

// A session with the asynchronous face on it. It owns one uniorm::orm and adds
// no second database path: every task below is that session's synchronous call,
// handed to the session's executor and ordered by its FIFO.
class UNIORM_API orm {
public:
  orm();
  // The same three ways in as the synchronous object, and the same rule: the
  // connection opens on the thread that builds the session, before any
  // operation is queued.
  explicit orm(std::string_view connection_string);
  explicit orm(connection_spec const& spec);
  explicit orm(connection_pool& pool);
  // Takes over an orm that is already set up, connected or leasing from a
  // pool -- the mapping and the lease come with it.
  explicit orm(uniorm::orm db);

  // A queued operation points back at the session owning its FIFO, so this
  // type neither moves nor copies.
  orm(orm&&) = delete;
  orm& operator=(orm&&) = delete;
  orm(orm const&) = delete;
  orm& operator=(orm const&) = delete;

  ~orm();

  // The configuration face, forwarded by name: the mapping and the row, batch
  // and identifier knobs. Use them while nothing is in flight -- they change
  // the same state the queue exists to protect, and neither side checks.
  template <class T>
  mapping_builder<T> map(std::string_view table) {
    return db_.template map<T>(table);
  }

  std::size_t row_array_size() const noexcept;

  void row_array_size(std::size_t size) noexcept;

  std::size_t paramset_size() const noexcept;

  void paramset_size(std::size_t size) noexcept;

  // Reading the mode is here; changing it is an operation, because it reaches
  // the connection.
  bool auto_commit() const noexcept;

  dialect::identifier_case identifier_case() const noexcept;

  void identifier_case(dialect::identifier_case policy);

  // The catalog side keeps no asynchronous door on purpose: validate / schema /
  // resolve_identifiers / clear_identifier_resolution, plus native_connection.
  // A coroutine that wants one calls through here and blocks that thread.
  uniorm::orm& sync() noexcept;

  // Where this session's operations run. Unset means the default pool.
  void run_on(executor& target) noexcept;

  // Where a continuation runs once its operation is over. Unset means the
  // worker that ran the body, which hands no thread over and is the shape a
  // one-off script never has to think about. An executor that only runs work
  // when the test pumps it is not a place a continuation can land: naming one
  // here, and blocking on the same session, waits on a resume nobody starts.
  void resume_on(executor& target) noexcept;

  // Requests that may hold a turn -- 1024 to start with. A full queue throws
  // submit_overflow at the drive action and leaves the task undriven.
  std::size_t submit_limit() const noexcept;

  void submit_limit(std::size_t count) noexcept;

  // Requests waiting their turn plus the one running.
  std::size_t pending() const;

  // Blocks until every request this session accepted has finished and answered
  // -- the barrier ~orm() runs, taken without closing the session. A request
  // the queue refused is not in it, so its callback never comes, and an
  // executor that only moves when pumped blocks here.
  void wait_all_operations();

  task<void> connect(std::string_view connection_string);
  task<void> connect(connection_spec const& spec);
  task<void> disconnect();

  task<std::size_t> execute_update(
    std::string_view sql, params const& bound = {});

  // A batch sweep is one operation, not N: the whole scan goes to the
  // executor as one body, exactly as the synchronous call runs it.
  template <class Entity>
  task<std::size_t> insert(std::vector<Entity> const& rows) {
    return enqueue<std::size_t>(insert_work(rows));
  }

  // The callback form: the same operation, answered instead of awaited. Read
  // the second argument first -- on failure the first one is R{}, not part of
  // an answer.
  template <class Entity>
  void insert(std::vector<Entity> const& rows,
    std::function<void(std::size_t, std::exception_ptr)> on_done) {
    answer<std::size_t>(insert_work(rows), std::move(on_done));
  }

  template <class Entity>
  task<std::size_t> update(std::vector<Entity> const& rows) {
    return enqueue<std::size_t>(update_work(rows));
  }

  template <class Entity>
  void update(std::vector<Entity> const& rows,
    std::function<void(std::size_t, std::exception_ptr)> on_done) {
    answer<std::size_t>(update_work(rows), std::move(on_done));
  }

  template <class Entity>
  task<std::size_t> remove(std::vector<Entity> const& rows) {
    return enqueue<std::size_t>(remove_work(rows));
  }

  template <class Entity>
  void remove(std::vector<Entity> const& rows,
    std::function<void(std::size_t, std::exception_ptr)> on_done) {
    answer<std::size_t>(remove_work(rows), std::move(on_done));
  }

  template <class T>
  task<std::vector<T>> query(std::string_view sql, params const& bound = {}) {
    return enqueue<std::vector<T>>(query_work<T>(sql, bound));
  }

  template <class T>
  void query(std::string_view sql, params const& bound,
    std::function<void(std::vector<T>, std::exception_ptr)> on_done) {
    answer<std::vector<T>>(query_work<T>(sql, bound), std::move(on_done));
  }

  template <class T>
  void query(std::string_view sql,
    std::function<void(std::vector<T>, std::exception_ptr)> on_done) {
    query<T>(sql, params{}, std::move(on_done));
  }

  // The builder form: decoration happens here on the caller's thread, and the
  // terminal renders and reads on the executor, because rendering asks the
  // connection's dialect which column spelling to use.
  template <class T>
  ::uniorm::async::query<T> query() {
    return ::uniorm::async::query<T>(*this, db_, db_.template meta<T>());
  }

  task<void> auto_commit(bool enabled);
  task<void> commit();
  task<void> rollback();
  task<transaction> begin();

private:
  template <class T>
  friend class ::uniorm::async::query;
  friend class ::uniorm::async::transaction;

  template <class R>
  task<R> enqueue(std::function<R()> work) {
    auto made = detail::run_sync<R>(std::move(work));
    made.bind(&queue_, run_on_, resume_on_);
    return made;
  }

  // One wrapper per operation, shared by the two faces: what the queue is given
  // is the same synchronous call either way, so the callback face cannot send a
  // statement the coroutine face would not have sent.
  template <class Entity>
  std::function<std::size_t()> insert_work(std::vector<Entity> const& rows) {
    return [this, copy = rows] { return db_.insert(copy); };
  }

  template <class Entity>
  std::function<std::size_t()> update_work(std::vector<Entity> const& rows) {
    return [this, copy = rows] { return db_.update(copy); };
  }

  template <class Entity>
  std::function<std::size_t()> remove_work(std::vector<Entity> const& rows) {
    return [this, copy = rows] { return db_.remove(copy); };
  }

  template <class T>
  std::function<std::vector<T>()> query_work(
    std::string_view sql, params const& bound) {
    return [this, text = std::string(sql), copy = bound] {
      return db_.template query<T>(text, copy);
    };
  }

  // The callback face is a session operation like any other: bound to the same
  // FIFO, refused at the call site by the same submit_limit, run on the same
  // executor. Only its body answers rather than returning.
  template <class R>
  void answer(std::function<R()> work,
    std::function<void(R, std::exception_ptr)> on_done) {
    if (!on_done) {
      // An empty callback swallows its own bad_function_call inside the body,
      // so the answer would vanish and ~orm() would wait for nothing at all.
      throw uniorm_error("async callback: on_done is empty");
    }
    auto guard = std::make_shared<detail::callback_guard<R>>();
    guard->on_done = std::move(on_done);
    // The one path where no body runs to answer: the queue was full or the
    // executor refused the hand-off, and this is the only place that knows.
    auto settle_failure = [guard](std::exception_ptr error) {
      if (guard->settled) {
        return;
      }
      try {
        guard->settle(R{}, error);
      } catch (...) {
      }
    };
    auto work_done = detail::answered<R>(std::move(work), guard);
    work_done.bind(&queue_, run_on_, resume_on_);
    // A refused push leaves nothing in flight, and the frame -- with the
    // caller's callback inside it -- lands here: the session keeps nothing.
    work_done.start(settle_failure);
  }

  uniorm::orm db_;
  detail::operation_queue queue_;
  executor* run_on_ = nullptr;
  executor* resume_on_ = nullptr;
  std::shared_ptr<detail::session_door> door_ =
    std::make_shared<detail::session_door>();
  // Every handle this session handed out that has not been closed yet. Kept
  // weak: closing one is the handle's own business, and ~orm() only needs the
  // ones still open to roll back before its connection goes.
  std::vector<std::weak_ptr<uniorm::transaction>> open_transactions_;

  friend class transaction;
};

// Read builder with task-shaped terminals. The decoration and the rendering are
// uniorm::query<T>'s; only the three terminals differ, and each hands the same
// read to the session.
template <class T>
class query : public query_shape<T, query<T>> {
  using shape = query_shape<T, query<T>>;

public:
  query(orm& session, uniorm::orm& db, entity_meta const& meta)
    : shape(db, meta), session_(&session) {}

  // A copy of the decorated builder travels with the request, so the rendering
  // happens where the read does.
  task<std::vector<T>> all() {
    return session_->enqueue<std::vector<T>>(
      [builder = this->self()] { return builder.run_all(); });
  }

  task<std::optional<T>> one() {
    return session_->enqueue<std::optional<T>>(
      [builder = this->self()] { return builder.run_one(); });
  }

  task<std::int64_t> count() {
    return session_->enqueue<std::int64_t>(
      [builder = this->self()] { return builder.run_count(); });
  }

private:
  orm* session_;
};

// A transaction opened through orm::begin(). commit() and rollback() are
// operations on the session, so each one waits its turn behind the writes it
// closes. Reaching the end of a scope without either is legal and is not
// forgotten: the destructor rolls back. That round trip is the one thing this
// handle does on the thread that destroys it, after waiting for the session to
// go quiet -- two operations on one connection is not a shape this API makes.
// Outliving the session is legal: ~orm() rolls back whatever is still open
// while its own connection is alive, so the handle left over has nothing to
// reach. commit() and rollback() on it throw instead of dereferencing a
// session that is gone, and its destructor returns without doing anything.
class UNIORM_API transaction {
public:
  transaction(transaction&&) noexcept = default;
  // Assigning over a live handle would have to roll the old one back first,
  // and a rollback is not something an assignment should do silently.
  transaction& operator=(transaction&&) = delete;
  transaction(transaction const&) = delete;
  transaction& operator=(transaction const&) = delete;
  ~transaction();

  // False once commit() or rollback() has run, including from a handle this
  // one was moved out of.
  bool active() const noexcept;

  task<void> commit();
  task<void> rollback();

private:
  friend class orm;

  transaction(std::shared_ptr<uniorm::transaction> inner, orm& session);

  // The handle a body moves out of begin(), shared with whatever closure is
  // already queued to close it: a commit() that runs after this handle is gone
  // still reaches the transaction it belongs to.
  std::shared_ptr<uniorm::transaction> inner_;
  orm* session_ = nullptr;
  std::shared_ptr<detail::session_door> door_;
};

}  // namespace uniorm::async
