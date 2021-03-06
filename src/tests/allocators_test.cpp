#if 0
#######################################################################################
# The MIT License

# Copyright (c) 2013       Benedikt Waldvogel, University of Bonn <mail@bwaldvogel.de>
# Copyright (c) 2012-2014  Hannes Schulz, University of Bonn  <schulz@ais.uni-bonn.de>

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#######################################################################################
#endif
#define BOOST_TEST_MODULE example

#include <boost/format.hpp>
#include <boost/test/included/unit_test.hpp>
#include <boost/thread/mutex.hpp>
#include <cuv/allocators.hpp>
#include <cuv/reference.hpp>
#include <tbb/parallel_for_each.h>

using namespace cuv;

BOOST_AUTO_TEST_SUITE(allocators_test)

template<class memory_space>
static void test_pooled_allocator() {
    memory_space m;
    pooled_cuda_allocator allocator;
    int* ptr1 = 0;
    int* ptr2 = 0;

    const int NUM_ELEMENTS = 10000;

    allocator.alloc(reinterpret_cast<void**>(&ptr1), NUM_ELEMENTS, sizeof(int), m);
    allocator.alloc(reinterpret_cast<void**>(&ptr2), NUM_ELEMENTS, sizeof(int), m);
    BOOST_CHECK(ptr1);
    BOOST_CHECK(ptr2);
    BOOST_CHECK_NE(ptr1, ptr2);
    BOOST_CHECK_EQUAL(allocator.pool_count(m), 2);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 0);
    BOOST_CHECK_EQUAL(allocator.pool_size(m), 2 * NUM_ELEMENTS * sizeof(int));

    for (size_t i = 0; i < 10000; i++) {
        reference<int, memory_space> ref(ptr1 + i);
        ref = i;
        BOOST_CHECK_EQUAL(static_cast<int>(ref), i);
    }

    allocator.dealloc(reinterpret_cast<void**>(&ptr1), m);
    BOOST_CHECK(ptr1 == 0);

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 2);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 1);
    BOOST_CHECK_EQUAL(allocator.pool_size(m), 2 * NUM_ELEMENTS * sizeof(int));

    for (size_t i = 0; i < 10000; i++) {
        reference<int, memory_space> ref(ptr2 + i);
        ref = i + 100;
        BOOST_CHECK_EQUAL(static_cast<int>(ref), i + 100);
    }

    allocator.dealloc(reinterpret_cast<void**>(&ptr2), m);

    BOOST_CHECK_EQUAL(allocator.pool_free_count(), allocator.pool_count());
}

template<class memory_space>
class allocate {
    private:
        pooled_cuda_allocator& allocator;
        int allocSize;
        boost::mutex& mutex;


    public:
        allocate(pooled_cuda_allocator& allocator, int allocSize, boost::mutex& mutex)
            : allocator(allocator), allocSize(allocSize), mutex(mutex) {}

        void operator()(void*& ptr) const {
            memory_space m;
            size_t pool_size = allocator.pool_size(m);
            void* ptr1 = NULL;
            void* ptr2 = NULL;
            allocator.alloc(&ptr1, allocSize, 1, m);
            allocator.alloc(&ptr2, 1, 1, m);
            allocator.alloc(&ptr, allocSize, 1, m);

            {
                boost::mutex::scoped_lock lock(mutex);
                BOOST_REQUIRE(ptr1);
                BOOST_REQUIRE(ptr2);
                BOOST_REQUIRE(ptr);

                BOOST_REQUIRE_NE(ptr1, ptr2);
                BOOST_REQUIRE_NE(ptr2, ptr);
                BOOST_REQUIRE_NE(ptr1, ptr);

                BOOST_REQUIRE_GE(allocator.pool_count(m), 2lu);
            }

            allocator.dealloc(&ptr1, m);
            allocator.dealloc(&ptr2, m);

            {
                boost::mutex::scoped_lock lock(mutex);
                BOOST_REQUIRE_GE(allocator.pool_size(m), pool_size);
                BOOST_REQUIRE_GE(allocator.pool_free_count(m), 0lu);
            }
        }
};

template<class memory_space>
class deallocate {
    private:
        pooled_cuda_allocator& allocator;
        boost::mutex& mutex;

    public:
      deallocate(pooled_cuda_allocator &allocator, boost::mutex &mutex)
          : allocator(allocator), mutex(mutex) {}

        void operator()(void*& ptr) const {
            allocator.dealloc(&ptr, memory_space());

            {
                boost::mutex::scoped_lock lock(mutex);
                BOOST_CHECK(!ptr);
            }
        }
};

template<class memory_space>
static void test_pooled_allocator_multi_threaded() {
    memory_space m;
    pooled_cuda_allocator allocator("allocator_multi_threaded");

    const int allocSize = pooled_cuda_allocator::MIN_SIZE_HOST;

    // boost-test is not thread-safe
    boost::mutex boost_mutex;

    std::vector<void*> pointers(1000, NULL);
    tbb::parallel_for_each(pointers.begin(), pointers.end(),
            allocate<memory_space>(allocator, allocSize, boost_mutex));

    for (size_t i = 0; i < pointers.size(); i++) {
        BOOST_REQUIRE(pointers[i]);
    }

    BOOST_CHECK_GE(allocator.pool_size(m), pointers.size() * allocSize);
    BOOST_CHECK_LE(allocator.pool_count(m), 10 * pointers.size());

    size_t count = allocator.pool_count(m);
    BOOST_CHECK_GE(count, pointers.size());

    tbb::parallel_for_each(pointers.begin(), pointers.end(),
            deallocate<memory_space>(allocator, boost_mutex));

    BOOST_CHECK_EQUAL(allocator.pool_free_count(), allocator.pool_count());
}

template<class memory_space>
static void test_pooled_allocator_garbage_collection() {
    memory_space m;
    pooled_cuda_allocator allocator;
    int* ptr1 = 0;
    int* ptr2 = 0;
    allocator.alloc(reinterpret_cast<void**>(&ptr1), 10000, sizeof(int), m);
    allocator.alloc(reinterpret_cast<void**>(&ptr2), 10000, sizeof(int), m);

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 2);

    allocator.dealloc(reinterpret_cast<void**>(&ptr1), m);

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 2);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 1);

    allocator.garbage_collection();

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 1);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 0);

    allocator.dealloc(reinterpret_cast<void**>(&ptr2), m);

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 1);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 1);

    allocator.garbage_collection();

    BOOST_CHECK_EQUAL(allocator.pool_count(m), 0);
    BOOST_CHECK_EQUAL(allocator.pool_free_count(m), 0);
}

BOOST_AUTO_TEST_CASE( pooled_cuda_allocator_test_simple ) {
    test_pooled_allocator<dev_memory_space>();
    test_pooled_allocator<host_memory_space>();
}

BOOST_AUTO_TEST_CASE( pooled_cuda_allocator_test_multithreaded ) {
    test_pooled_allocator_multi_threaded<dev_memory_space>();
    test_pooled_allocator_multi_threaded<host_memory_space>();
}

BOOST_AUTO_TEST_CASE( pooled_cuda_allocator_test_garbage_collection ) {
    test_pooled_allocator_garbage_collection<dev_memory_space>();
    test_pooled_allocator_garbage_collection<host_memory_space>();
}
BOOST_AUTO_TEST_SUITE_END()
