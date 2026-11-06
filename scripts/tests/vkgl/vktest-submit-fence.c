/*
 * Deterministic submission-fence state-machine test.  This file intentionally
 * uses the C test name convention but is compiled as C++ with the production
 * submit-fence.hpp.  No Vulkan loader, EGL display, GLES context, or GPU is used.
 */
#include "submit-fence.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { \
   std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (0)

static void check_result(vkgl_fence::Result actual, vkgl_fence::Result expected, int line) {
   if (actual != expected) {
      std::fprintf(stderr, "FAIL line %d: result %d != %d\n", line,
                   (int)actual, (int)expected);
      std::exit(1);
   }
}
#define CHECK_RESULT(actual, expected) check_result((actual), (expected), __LINE__)

struct FakeClock {
   uint64_t now = 1000000;
   uint64_t now_ns() const { return now; }
};

struct FakeBackend {
   FakeClock &clock;
   bool allow_create = true;
   bool fail_wait = false;
   uintptr_t next_handle = 1;
   int create_calls = 0;
   int flush_calls = 0;
   int finish_calls = 0;
   int wait_calls = 0;
   int destroy_calls = 0;
   std::unordered_map<void *, uint64_t> ready_at;
   std::vector<uint64_t> wait_timeouts;
   std::vector<uint64_t> sleeps;

   explicit FakeBackend(FakeClock &c) : clock(c) {}

   void *create_sync() {
      ++create_calls;
      if (!allow_create) return nullptr;
      void *handle = (void *)next_handle++;
      ready_at.emplace(handle, vkgl_fence::infinite_timeout);
      return handle;
   }
   void flush_commands() { ++flush_calls; }
   void finish_commands() { ++finish_calls; }

   vkgl_fence::Result wait_sync(void *handle, uint64_t timeout_ns) {
      ++wait_calls;
      wait_timeouts.push_back(timeout_ns);
      if (fail_wait) return vkgl_fence::Result::Error;
      auto it = ready_at.find(handle);
      if (it == ready_at.end()) return vkgl_fence::Result::Error;
      const uint64_t ready = it->second;
      if (clock.now >= ready && ready != vkgl_fence::infinite_timeout)
         return vkgl_fence::Result::Signaled;
      if (timeout_ns == 0) return vkgl_fence::Result::Timeout;
      if (timeout_ns == vkgl_fence::infinite_timeout) {
         if (ready == vkgl_fence::infinite_timeout) return vkgl_fence::Result::Error;
         clock.now = ready;
         return vkgl_fence::Result::Signaled;
      }
      const uint64_t target = timeout_ns > vkgl_fence::infinite_timeout - clock.now
         ? vkgl_fence::infinite_timeout : clock.now + timeout_ns;
      if (ready != vkgl_fence::infinite_timeout && ready <= target) {
         clock.now = ready;
         return vkgl_fence::Result::Signaled;
      }
      clock.now = target;
      return vkgl_fence::Result::Timeout;
   }

   void destroy_sync(void *handle) {
      ++destroy_calls;
      ready_at.erase(handle);
   }

   void sleep_for(uint64_t timeout_ns) {
      sleeps.push_back(timeout_ns);
      clock.now = timeout_ns > vkgl_fence::infinite_timeout - clock.now
         ? vkgl_fence::infinite_timeout : clock.now + timeout_ns;
   }

   void ready_after(void *handle, uint64_t delay_ns) {
      ready_at.at(handle) = clock.now + delay_ns;
   }
};

static void test_never_submitted_zero_timeout() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State never;
   vkgl_fence::State *one[] = {&never};
   CHECK_RESULT(vkgl_fence::observe(never, backend), vkgl_fence::Result::Timeout);
   CHECK_RESULT(vkgl_fence::wait(one, 1, false, 0, backend, clock),
                vkgl_fence::Result::Timeout);
   CHECK_RESULT(vkgl_fence::wait(one, 1, true, 0, backend, clock),
                vkgl_fence::Result::Timeout);
   CHECK(!never.signaled);
   CHECK(never.sync == nullptr);
   CHECK(backend.wait_calls == 0);
   CHECK(clock.now == 1000000);
}

static void test_zero_timeout_wait_all_and_any() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State never;
   vkgl_fence::State host;
   host.signaled = true;
   vkgl_fence::State *both[] = {&never, &host};
   CHECK_RESULT(vkgl_fence::wait(both, 2, false, 0, backend, clock),
                vkgl_fence::Result::Signaled);
   CHECK_RESULT(vkgl_fence::wait(both, 2, true, 0, backend, clock),
                vkgl_fence::Result::Timeout);
   CHECK(!never.signaled);
   CHECK(host.signaled);
   CHECK(backend.wait_calls == 0);
}

static void test_submission_tracks_sync_completion() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State state;
   vkgl_fence::submit(state, backend);
   CHECK(backend.create_calls == 1);
   CHECK(backend.flush_calls == 1);
   CHECK(backend.finish_calls == 0);
   CHECK(!state.signaled);
   CHECK(state.sync != nullptr);
   CHECK_RESULT(vkgl_fence::observe(state, backend), vkgl_fence::Result::Timeout);
   CHECK(!state.signaled);
   backend.ready_after(state.sync, 0);
   CHECK_RESULT(vkgl_fence::observe(state, backend), vkgl_fence::Result::Signaled);
   CHECK(state.signaled);
   CHECK(state.sync == nullptr);
   CHECK(backend.destroy_calls == 1);
   const int waits = backend.wait_calls;
   CHECK_RESULT(vkgl_fence::observe(state, backend), vkgl_fence::Result::Signaled);
   CHECK(backend.wait_calls == waits);
}

static void test_conservative_finish_fallback() {
   FakeClock clock;
   FakeBackend backend(clock);
   backend.allow_create = false;
   vkgl_fence::State state;
   vkgl_fence::submit(state, backend);
   CHECK(state.signaled);
   CHECK(state.sync == nullptr);
   CHECK(backend.create_calls == 1);
   CHECK(backend.flush_calls == 0);
   CHECK(backend.finish_calls == 1);
   CHECK_RESULT(vkgl_fence::observe(state, backend), vkgl_fence::Result::Signaled);
   CHECK(backend.wait_calls == 0);
}

static void test_reset_and_host_signal_cleanup() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State state;
   vkgl_fence::submit(state, backend);
   vkgl_fence::reset(state, backend);
   CHECK(!state.signaled);
   CHECK(state.sync == nullptr);
   CHECK(backend.destroy_calls == 1);
   vkgl_fence::State *one[] = {&state};
   CHECK_RESULT(vkgl_fence::wait(one, 1, true, 0, backend, clock),
                vkgl_fence::Result::Timeout);
   vkgl_fence::signal_host(state, backend);
   CHECK(state.signaled);
   CHECK(state.sync == nullptr);
   CHECK(backend.create_calls == 1);
   CHECK(backend.flush_calls == 1);
   CHECK(backend.finish_calls == 0);
   CHECK_RESULT(vkgl_fence::wait(one, 1, true, 0, backend, clock),
                vkgl_fence::Result::Signaled);
}

static void test_wait_all_success() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State a, b;
   vkgl_fence::submit(a, backend);
   vkgl_fence::submit(b, backend);
   backend.ready_after(a.sync, 3000000);
   backend.ready_after(b.sync, 5000000);
   vkgl_fence::State *both[] = {&a, &b};
   const uint64_t start = clock.now;
   CHECK_RESULT(vkgl_fence::wait(both, 2, true, 10000000, backend, clock),
                vkgl_fence::Result::Signaled);
   CHECK(clock.now == start + 5000000);
   CHECK(a.signaled && b.signaled);
   CHECK(a.sync == nullptr && b.sync == nullptr);
   CHECK(backend.destroy_calls == 2);
}

static void test_wait_all_uses_one_deadline() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State a, b;
   vkgl_fence::submit(a, backend);
   vkgl_fence::submit(b, backend);
   backend.ready_after(a.sync, 2000000);
   vkgl_fence::State *both[] = {&a, &b};
   const uint64_t start = clock.now;
   CHECK_RESULT(vkgl_fence::wait(both, 2, true, 5000000, backend, clock),
                vkgl_fence::Result::Timeout);
   CHECK(clock.now == start + 5000000);
   CHECK(a.signaled);
   CHECK(!b.signaled);
   CHECK(b.sync != nullptr);
   vkgl_fence::reset(a, backend);
   vkgl_fence::reset(b, backend);
   CHECK(backend.destroy_calls == 2);
}

static void test_wait_any_does_not_block_behind_first_fence() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State a, b;
   vkgl_fence::submit(a, backend);
   vkgl_fence::submit(b, backend);
   backend.ready_after(b.sync, 2000000);
   vkgl_fence::State *both[] = {&a, &b};
   const uint64_t start = clock.now;
   CHECK_RESULT(vkgl_fence::wait(both, 2, false, 10000000, backend, clock),
                vkgl_fence::Result::Signaled);
   CHECK(clock.now == start + 2000000);
   CHECK(!a.signaled);
   CHECK(b.signaled);
   CHECK(a.sync != nullptr && b.sync == nullptr);
   vkgl_fence::reset(a, backend);
   vkgl_fence::reset(b, backend);
}

static void test_finite_host_only_timeout() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State never;
   vkgl_fence::State *one[] = {&never};
   const uint64_t start = clock.now;
   CHECK_RESULT(vkgl_fence::wait(one, 1, true, 3000000, backend, clock),
                vkgl_fence::Result::Timeout);
   CHECK(clock.now == start + 3000000);
   CHECK(backend.sleeps.size() == 3);
   CHECK(!never.signaled);
   CHECK(backend.wait_calls == 0);
}

static void test_infinite_timeout_uses_forever() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State state;
   vkgl_fence::submit(state, backend);
   backend.ready_after(state.sync, 4000000);
   vkgl_fence::State *one[] = {&state};
   const uint64_t start = clock.now;
   CHECK_RESULT(vkgl_fence::wait(one, 1, true, vkgl_fence::infinite_timeout,
                                 backend, clock), vkgl_fence::Result::Signaled);
   CHECK(clock.now == start + 4000000);
   CHECK(backend.wait_timeouts.back() == vkgl_fence::infinite_timeout);
   CHECK(state.signaled && state.sync == nullptr);
}

static void test_wait_error_preserves_cleanup_handle() {
   FakeClock clock;
   FakeBackend backend(clock);
   vkgl_fence::State state;
   vkgl_fence::submit(state, backend);
   backend.fail_wait = true;
   CHECK_RESULT(vkgl_fence::observe(state, backend), vkgl_fence::Result::Error);
   CHECK(!state.signaled);
   CHECK(state.sync != nullptr);
   CHECK(backend.destroy_calls == 0);
   vkgl_fence::clear_sync(state, backend);
   CHECK(state.sync == nullptr);
   CHECK(backend.destroy_calls == 1);
}

int main() {
   test_never_submitted_zero_timeout();
   test_zero_timeout_wait_all_and_any();
   test_submission_tracks_sync_completion();
   test_conservative_finish_fallback();
   test_reset_and_host_signal_cleanup();
   test_wait_all_success();
   test_wait_all_uses_one_deadline();
   test_wait_any_does_not_block_behind_first_fence();
   test_finite_host_only_timeout();
   test_infinite_timeout_uses_forever();
   test_wait_error_preserves_cleanup_handle();
   std::puts("PASS vktest-submit-fence (11 deterministic cases; no EGL/GPU)");
   return 0;
}
