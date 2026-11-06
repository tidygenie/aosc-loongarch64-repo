#ifndef VKGL_SUBMIT_FENCE_HPP
#define VKGL_SUBMIT_FENCE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vkgl_fence {

constexpr uint64_t infinite_timeout = std::numeric_limits<uint64_t>::max();
constexpr uint64_t wait_slice_ns = 1000000; // Keep multi-fence waits responsive.

struct State {
   bool signaled = false;
   void *sync = nullptr;
};

enum class Result { Signaled, Timeout, Error };

// Backend wait_sync() returns Signaled, Timeout, or Error and accepts
// infinite_timeout. Clock now_ns() is monotonic. Neither owns State.
template <typename Backend>
void clear_sync(State &state, Backend &backend) {
   if (state.sync) backend.destroy_sync(state.sync);
   state.sync = nullptr;
}

template <typename Backend>
void reset(State &state, Backend &backend) {
   clear_sync(state, backend);
   state.signaled = false;
}

template <typename Backend>
void signal_host(State &state, Backend &backend) {
   // Swapchain acquire completion is a host event, never an EGL submission.
   clear_sync(state, backend);
   state.signaled = true;
}

template <typename Backend>
void submit(State &state, Backend &backend) {
   reset(state, backend);
   state.sync = backend.create_sync();
   if (state.sync) {
      // Ensure that the new fence reaches the GPU without blocking the caller.
      backend.flush_commands();
   } else {
      // No usable EGL_KHR_fence_sync: preserve completion semantics conservatively.
      backend.finish_commands();
      state.signaled = true;
   }
}

template <typename Backend>
Result observe(State &state, Backend &backend, uint64_t timeout_ns = 0) {
   if (state.signaled) return Result::Signaled;
   if (!state.sync) return Result::Timeout;
   Result result = backend.wait_sync(state.sync, timeout_ns);
   if (result == Result::Signaled) {
      clear_sync(state, backend);
      state.signaled = true;
   }
   return result;
}

template <typename Backend, typename Clock>
Result wait(State *const *states, size_t count, bool wait_all, uint64_t timeout_ns,
            Backend &backend, Clock &clock) {
   if (count == 0) return Result::Signaled;

   const bool infinite = timeout_ns == infinite_timeout;
   const uint64_t start = clock.now_ns();
   const uint64_t deadline = infinite || timeout_ns > infinite_timeout - start
      ? infinite_timeout : start + timeout_ns;

   for (;;) {
      State *candidate = nullptr;
      size_t pending = 0;
      bool all_signaled = true;

      // Poll every relevant fence once before deciding whether to sleep. In
      // particular, a zero timeout must not manufacture a host signal.
      for (size_t i = 0; i < count; ++i) {
         State &state = *states[i];
         Result result = observe(state, backend, 0);
         if (result == Result::Error) return result;
         if (result == Result::Signaled) {
            if (!wait_all) return Result::Signaled;
         } else {
            all_signaled = false;
            ++pending;
            if (!candidate && state.sync) candidate = &state;
         }
      }
      if (wait_all && all_signaled) return Result::Signaled;
      if (timeout_ns == 0) return Result::Timeout;

      uint64_t remaining = infinite_timeout;
      if (!infinite) {
         const uint64_t now = clock.now_ns();
         if (now >= deadline) return Result::Timeout;
         remaining = deadline - now;
      }

      // A sole pending sync can use the full remaining timeout (including
      // EGL_FOREVER). Otherwise bound each wait and re-poll all fences so
      // waitAny cannot strand an earlier-completing fence behind another one.
      const bool sole_pending_sync = pending == 1 && candidate != nullptr;
      const uint64_t interval = sole_pending_sync
         ? remaining : std::min(remaining, wait_slice_ns);

      if (candidate) {
         Result result = observe(*candidate, backend, interval);
         if (result == Result::Error) return result;
         if (result == Result::Signaled && !wait_all) return result;
      } else {
         // Host-only unsignaled fences have no OS object on which to block.
         backend.sleep_for(interval);
      }
   }
}

} // namespace vkgl_fence

#endif
