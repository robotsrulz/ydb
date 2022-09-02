#pragma once

#include "intrusive_ptr.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

// Atomic ptr based on https://github.com/facebook/folly/blob/main/folly/concurrency/AtomicSharedPtr.h

// Operators * and -> for TAtomicIntrusivePtr are useless because it is not safe to work with atomic ptr such way
// Safe usage is to convert to TIntrusivePtr.

// Max TAtomicIntrusivePtr count per object is (2**16 = 2**32 / 2**16).

template <class T>
class TAtomicIntrusivePtr
{
public:
    TAtomicIntrusivePtr() = default;
    TAtomicIntrusivePtr(std::nullptr_t);

    explicit TAtomicIntrusivePtr(TIntrusivePtr<T> other);
    TAtomicIntrusivePtr(TAtomicIntrusivePtr&& other);

    ~TAtomicIntrusivePtr();

    TAtomicIntrusivePtr& operator=(TIntrusivePtr<T> other);
    TAtomicIntrusivePtr& operator=(std::nullptr_t);

    TIntrusivePtr<T> Acquire();

    TIntrusivePtr<T> Exchange(TIntrusivePtr<T> other);

    void Reset();
    bool CompareAndSwap(void*& comparePtr, T* target);
    bool CompareAndSwap(void*& comparePtr, TIntrusivePtr<T> target);

    // Result is suitable only for comparison. Not dereference.
    void* Get() const;

    explicit operator bool() const;

private:
    template <class U>
    friend bool operator==(const TAtomicIntrusivePtr<U>& lhs, const TIntrusivePtr<U>& rhs);

    template <class U>
    friend bool operator==(const TIntrusivePtr<U>& lhs, const TAtomicIntrusivePtr<U>& rhs);

    template <class U>
    friend bool operator!=(const TAtomicIntrusivePtr<U>& lhs, const TIntrusivePtr<U>& rhs);

    template <class U>
    friend bool operator!=(const TIntrusivePtr<U>& lhs, const TAtomicIntrusivePtr<U>& rhs);

    // Keeps packed pointer (localRefCount, objectPtr).
    // Atomic ptr holds N references, where N = ReservedRefCount - localRefCount.
    // LocalRefCount is incremented in Acquire method.
    // When localRefCount exceeds ReservedRefCount / 2 a new portion of refs are required globally.
    std::atomic<char*> Ptr_ = nullptr;

    constexpr static int CounterBits = 64 - PtrBits;
    constexpr static int ReservedRefCount = (1 << CounterBits) - 1;

    // Consume ref if ownership is transferred.
    // AcquireObject(ptr.Release(), true)
    // AcquireObject(ptr.Get(), false)
    static char* AcquireObject(T* obj, bool consumeRef = false);
    static void ReleaseObject(void* packedPtr);
    static void DoRelease(T* obj, int refs);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define ATOMIC_INTRUSIVE_PTR_INL_H_
#include "atomic_intrusive_ptr-inl.h"
#undef ATOMIC_INTRUSIVE_PTR_INL_H_
