// Copyright Steinwurf ApS 2014.
// All Rights Reserved
//
// Distributed under the "BSD License". See the accompanying LICENSE.rst file.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <type_traits>
#include <utility>

#include "no_locking_policy.hpp"

namespace recycle
{

const std::size_t DEF_CAP = 10; //value used to set default capacity of the shared pool

/// @brief The shared pool stores value objects and recycles them.
///
/// The shared pool is a useful construct if you have some
/// expensive to create objects where you would like to create a
/// factory capable of recycling the objects.
///
///
template <class Value, class LockingPolicy = no_locking_policy>
class shared_pool
{
public:
    /// The type managed
    using value_type = Value;

    /// The pointer to the resource
    using value_ptr = std::shared_ptr<value_type>;

    /// The allocate function type
    /// Should take no arguments and return an std::shared_ptr to the Value
    using allocate_function = std::function<value_ptr()>;

    /// The recycle function type
    /// If specified the recycle function will be called every time a
    /// resource gets recycled into the pool. This allows temporary
    /// resources, e.g., file handles to be closed when an object is longer
    /// used.
    using recycle_function = std::function<void(value_ptr)>;

    /// The locking policy mutex type
    using mutex_type = typename LockingPolicy::mutex_type;

    /// The locking policy lock type
    using lock_type = typename LockingPolicy::lock_type;

public:
    /// Default constructor, we only want this to be available
    /// i.e. the shared_pool to be default constructible if the
    /// value_type we build is default constructible.
    ///
    /// This means that we only want
    /// std::is_default_constructible<shared_pool<T>>::value to
    /// be true if the type T is default constructible.
    ///
    /// Unfortunately this does not work if we don't do the
    /// template magic seen below. What we do there is to use
    /// SFINAE to disable the default constructor for non default
    /// constructible types.
    ///
    /// It looks quite ugly and if somebody can fix in a simpler way
    /// please do :)
    /// @param cap maximum number of recycled resources in the pool
    template <class T = Value,
              typename std::enable_if<std::is_default_constructible<T>::value,
                                      uint8_t>::type = 0>
    shared_pool(std::size_t cap = DEF_CAP) :
        m_pool(std::make_shared<impl>(
            allocate_function(std::make_shared<value_type>), cap))
    {
    }

    /// Create a shared pool using a specific allocate function.
    /// @param allocate Allocation function
    /// @param cap maximum number of recycled resources in the pool
    shared_pool(allocate_function allocate, std::size_t cap = DEF_CAP) :
        m_pool(std::make_shared<impl>(std::move(allocate), cap))
    {
    }

    /// Create a shared pool using a specific allocate function and
    /// recycle function.
    /// @param allocate Allocation function
    /// @param recycle Recycle function
    /// @param cap maximum number of recycled resources in the pool
    shared_pool(allocate_function allocate, recycle_function recycle, std::size_t cap = DEF_CAP) :
        m_pool(std::make_shared<impl>(std::move(allocate), std::move(recycle), cap))
    {
    }

    /// Copy constructor
    shared_pool(const shared_pool& other) :
        m_pool(std::make_shared<impl>(*other.m_pool))
    {
    }

    /// Move constructor
    shared_pool(shared_pool&& other) : m_pool(std::move(other.m_pool))
    {
        assert(m_pool);
    }

    /// Copy assignment
    shared_pool& operator=(const shared_pool& other)
    {
        shared_pool tmp(other);
        std::swap(*this, tmp);
        return *this;
    }

    /// Move assignment
    shared_pool& operator=(shared_pool&& other)
    {
        m_pool = std::move(other.m_pool);
        return *this;
    }

    /// @returns the number of unused resources
    std::size_t unused_resources() const
    {
        assert(m_pool);
        return m_pool->unused_resources();
    }

    /// Frees all unused resources
    void free_unused()
    {
        assert(m_pool);
        m_pool->free_unused();
    }

    /// @return A resource from the pool.
    value_ptr allocate()
    {
        assert(m_pool);
        return m_pool->allocate();
    }

    /// @return A resource from the pool.
    /// @param params list of parameters sent to constructor of the type of the shared pool inside
    template<typename ...T>
    value_ptr allocate_with(T&& ...params)
    {
        assert(m_pool);
        return m_pool->allocate_with(std::forward<T>(params)...);
    }

    /// @return maximum number of resources that can be unused
    std::size_t capacity() const
    {
        assert(m_pool);
        return m_pool->capacity();
    }

    /// @param num maximum amount of unused resources of the pool that we want to set 
    void setCapacity(std::size_t num)
    {
        assert(m_pool);
        m_pool->setCapacity(num);
    }

    /// @return number of resources currently in use
    std::size_t used_resources() const
    {
        assert(m_pool);
        return m_pool->used_resources();
    }

private:
    /// The actual pool implementation. We use the
    /// enable_shared_from_this helper to make sure we can pass a
    /// "back-pointer" to the pooled objects. The idea behind this
    /// is that we need objects to be able to add themselves back
    /// into the pool once they go out of scope.
    struct impl : public std::enable_shared_from_this<impl>
    {
        /// @copydoc shared_pool::shared_pool(allocate_function, std::size_t)
        impl(allocate_function allocate, std::size_t cap) : m_allocate(std::move(allocate)), m_capacity(cap)
        {
            assert(m_allocate);
        }

        /// @copydoc shared_pool::shared_pool(allocate_function,
        ///                                       recycle_function, std::size_t)
        impl(allocate_function allocate, recycle_function recycle, std::size_t cap) :
            m_allocate(std::move(allocate)), m_recycle(std::move(recycle)), m_capacity(cap)
        {
            assert(m_allocate);
            assert(m_recycle);
        }

        /// Copy constructor
        impl(const impl& other) :
            std::enable_shared_from_this<impl>(other),
            m_allocate(other.m_allocate), m_recycle(other.m_recycle)
        {
            std::size_t size = other.unused_resources();
            for (std::size_t i = 0; i < size; ++i)
            {
                m_free_list.push_back(m_allocate());
            }
            m_capacity = other.capacity();
            m_used_resources = other.used_resources();
        }

        /// Move constructor
        impl(impl&& other) :
            std::enable_shared_from_this<impl>(other),
            m_allocate(std::move(other.m_allocate)),
            m_recycle(std::move(other.m_recycle)),
            m_free_list(std::move(other.m_free_list))
        {
            m_capacity = other.capacity();
            m_used_resources = other.used_resources();
        }

        /// Copy assignment
        impl& operator=(const impl& other)
        {
            impl tmp(other);
            std::swap(*this, tmp);
            return *this;
        }

        /// Move assignment
        impl& operator=(impl&& other)
        {
            m_allocate = std::move(other.m_allocate);
            m_recycle = std::move(other.m_recycle);
            m_free_list = std::move(other.m_free_list);
            m_capacity = other.capacity();
            m_used_resources = other.used_resources();
            return *this;
        }

        /// get maximum capacity of pool
        std::size_t capacity() const 
        {
            lock_type lock(m_mutex);
            return this->m_capacity;
        }

        /// set the maximum pool capacity
        void setCapacity(std::size_t num) 
        {   
            lock_type lock(m_mutex);
            // if the recycler has more resources than the new capacity, drop excess resources out
            if (m_free_list.size() > num) m_free_list.resize(num);
            m_capacity = num;
        }

        /// number of resources in use currently
        std::size_t used_resources() 
        {
            lock_type lock(m_mutex);
            return this->m_used_resource;
        }

        /// Allocate a new value from the pool
        value_ptr allocate()
        {
            value_ptr resource;

            {
                lock_type lock(m_mutex);

                if (m_free_list.size() > 0)
                {
                    resource = m_free_list.back();
                    m_free_list.pop_back();
                }
            }

            if (!resource)
            {
                assert(m_allocate);
                resource = m_allocate();
            }

            {
                lock_type lock(m_mutex);
                // if the number of used_resources, summed with currently recycled ones, exceeded capacity, we will not recycle this resource. Rather we will use regular shared_pointer
                if (m_used_resources + m_free_list.size() > m_capacity) return value_ptr(resource.get());
                ++m_used_resources; // we increment used_resources to keep track of resources that can be returned to the pool
            }

            auto pool = impl::shared_from_this();

            // Here we create a std::shared_ptr<T> with a naked
            // pointer to the resource and a custom deleter
            // object. The custom deleter object stores two
            // things:
            //
            //   1. A std::weak_ptr<T> to the pool (used when we
            //      need to put the resource back in the pool). If
            //      the pool dies before the resource then we can
            //      detect this with the weak_ptr and no try to
            //      access it.
            //
            //   2. A std::shared_ptr<T> that points to the actual
            //      resource and is the one actually keeping it alive.

            return value_ptr(resource.get(), deleter(pool, resource));
        }
       
        // Allocates a new value from the pool constructed with parameters accepted by the inner type
        template<typename ...T>
        value_ptr allocate_with(T&& ...params)
        {
            value_ptr resource;

            {
                lock_type lock(m_mutex);

                if (m_free_list.size() > 0)
                {
                    resource = m_free_list.back();
                    m_free_list.pop_back();
                    // we call the constructor of the inner type to initialize the content of the resource.
                    // this approach allows us to create the memory footprint of value_type on the stack and then
                    // copy construct it to the resource. This way we dont have to allocate the class on the heap.
                    // pitfall is that if class constructor is doing any expensive allocations inside, we won't be saving any
                    // time or space on that. So, the class itself is on stack but its elements are potentially dinamically allocated.
                    // Tradeoff we acquire is that we can initiliaze the type any way it's parameters allow, while still
                    // having capability to use regular allocate function that handles expensive objects that are default
                    // constructed.
                    *resource = value_type(std::forward<T>(params)...);
                }
            }

            if (!resource)
            {
                assert(m_allocate);
                // if this is first allocation of the object, use make shared with forwarded parameters to initialize object to what we want
                resource = std::make_shared<value_type>(std::forward<T>(params)...);
            }

            {
                lock_type lock(m_mutex);
                // if the number of used_resources, summed with currently recycled ones, exceeded capacity, we will not recycle this resource. Rather we will use regular shared_pointer
                if (m_used_resources + m_free_list.size() > m_capacity) return value_ptr(resource.get());
                ++m_used_resources; // we increment used_resources to keep track of resources that can be returned to the pool
            }

            auto pool = impl::shared_from_this();

            // Here we create a std::shared_ptr<T> with a naked
            // pointer to the resource and a custom deleter
            // object. The custom deleter object stores two
            // things:
            //
            //   1. A std::weak_ptr<T> to the pool (used when we
            //      need to put the resource back in the pool). If
            //      the pool dies before the resource then we can
            //      detect this with the weak_ptr and no try to
            //      access it.
            //
            //   2. A std::shared_ptr<T> that points to the actual
            //      resource and is the one actually keeping it alive.

            return value_ptr(resource.get(), deleter(pool, resource));   
        }


        /// @copydoc shared_pool::free_unused()
        void free_unused()
        {
            lock_type lock(m_mutex);
            m_free_list.clear();
        }

        /// @copydoc shared_pool::unused_resources()
        std::size_t unused_resources() const
        {
            lock_type lock(m_mutex);
            return m_free_list.size();
        }

        /// This function called when a resource should be added
        /// back into the pool
        void recycle(const value_ptr& resource)
        {
            if (m_recycle)
            {
                m_recycle(resource);
            }

            lock_type lock(m_mutex);
            // if recycler is full, disregard the resource, otherwise recycle
            if (m_free_list.size() < m_capacity) m_free_list.push_back(resource);
            --m_used_resources; // we decrement used_resources counter, since this resource is not in use anymore
        }

    private:
        /// The allocator to use
        allocate_function m_allocate;

        /// The recycle function
        recycle_function m_recycle;

        /// Stores all the free resources
        std::list<value_ptr> m_free_list;

        /// variable storing capacity of the pool
        size_t m_capacity;

        /// variable storing number of resources in use
        size_t m_used_resources = 0;

        /// Mutex used to coordinate access to the pool. We had to
        /// make it mutable as we have to lock in the
        /// unused_resources() function. Otherwise we can have a
        /// race condition on the size it returns. I.e. if one
        /// threads releases a resource into the free list while
        /// another tries to read its size.
        mutable mutex_type m_mutex;
    };

    /// The custom deleter object used by the std::shared_ptr<T>
    /// to de-allocate the object if the pool goes out of
    /// scope. When a std::shared_ptr wants to de-allocate the
    /// object contained it will call the operator() define here.
    struct deleter
    {
        /// @param pool. A weak_ptr to the pool
        deleter(const std::weak_ptr<impl>& pool, const value_ptr& resource) :
            m_pool(pool), m_resource(resource)
        {
            assert(!m_pool.expired());
            assert(m_resource);
        }

        /// Call operator called by std::shared_ptr<T> when
        /// de-allocating the object.
        void operator()(value_type*)
        {
            // Place the resource in the free list
            auto pool = m_pool.lock();

            if (pool)
            {
                pool->recycle(m_resource);
            }

            // This reset() is needed because otherwise a circular
            // dependency can arise here in special situations.
            //
            // One example of such a situation is when the value_type
            // derives from std::enable_shared_from_this in that case,
            // the following will happen:
            //
            // The std::enable_shared_from_this implementation works by
            // storing a std::weak_ptr to itself. This std::weak_ptr
            // internally points to an "counted" object keeping track
            // of the reference count managing the raw pointer's release
            // policy (e.g. storing the custom deleter etc.) for all
            // the shared_ptr's. The "counted" object is both kept
            // alive by all std::shared_ptr and std::weak_ptr objects.
            //
            // In this specific case of std::enable_shared_from_this,
            // the custom deleter is not destroyed because the internal
            // std::weak_ptr still points to the "counted" object and
            // inside the custom deleter we are keeping the managed
            // object alive because we have a std::shared_ptr to it.
            //
            // The following diagram show the circular dependency where
            // the arrows indicate what is keeping what alive:
            //
            //  +----------------+                   +--------------+
            //  | custom deleter +--------------+    | real deleter |
            //  +----------------+              |    +--------------+
            //         ^                        |            ^
            //         |                        |            |
            //         |                        |            |
            //   +-----+--------+               |    +-------+------+
            //   | shared_count |               |    | shared_count |
            //   +--------------+               |    +--------------+
            //      ^    ^                      |            ^
            //      |    |                      |            |
            //      |    |                      |            |
            //      |    |                      v            |
            //      |    |  +------------+    +------------+ |
            //      |    +--+ shared_ptr |    | shared_ptr +-+
            //      |       +------------+    +----+-------+
            //      |                              |
            //      |                              |
            // +----+-----+            +--------+  |
            // | weak_ptr |<-----------+ object |<-+
            // +----------+            +--------+
            //
            // The std::shared_ptr on the right is the one managed by the
            // shared pool, it is the one actually deleting the
            // object when it goes out of scope. The shared_ptr on the
            // left is the one which contains the custom
            // deleter that will return the object into the resource
            // pool when it goes out of scope.
            //
            // By calling reset on the shared_ptr in the custom deleter
            // we break the cyclic dependency.

            // if we didnt recycle, reset call will delete the resource
            m_resource.reset();
        }

        // Pointer to the pool needed for recycling
        std::weak_ptr<impl> m_pool;

        // The resource object
        value_ptr m_resource;
    };

private:
    // The pool impl
    std::shared_ptr<impl> m_pool;
};
} // namespace recycle
