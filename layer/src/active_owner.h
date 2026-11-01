// Tracks "which backend instance is the live one" for process-wide hooks.
//
// Bug this prevents (found in the v0.2.0 review): when a second session replaces the first,
// the new backend is created BEFORE the old one is destroyed. If the old destructor simply
// cleared the pointer, the new backend would lose its hooks. release() only clears the
// pointer if the caller is still the owner.
#pragma once
#include <atomic>

namespace ovalffr {

template <class T>
class ActiveOwner {
public:
    void set(T* p) { cur_.store(p); }
    void release(T* p) {
        T* expected = p;
        cur_.compare_exchange_strong(expected, nullptr);
    }
    T* get() const { return cur_.load(); }

private:
    std::atomic<T*> cur_{nullptr};
};

} // namespace ovalffr
