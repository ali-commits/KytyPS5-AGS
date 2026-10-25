#pragma once

#include <atomic>

namespace Common {

// Call after a release store. GCC libstdc++ PR127757 can otherwise skip the
// wake while a new waiter still sees the old value (store buffering).
// https://gcc.gnu.org/PR127757 — fixed upstream in a971455d8f2b.
template <typename T>
void NotifyAllAfterStore(std::atomic<T>& value) noexcept {
	std::atomic_thread_fence(std::memory_order_seq_cst);
	value.notify_all();
}

} // namespace Common
