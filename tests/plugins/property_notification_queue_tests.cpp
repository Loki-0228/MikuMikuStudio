#include "../../src/plugins/manager.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace { bool failAllocation=false; }
void* operator new(std::size_t size) {
    if (failAllocation) throw std::bad_alloc();
    if (auto* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
using mikumikustudio::plugins::PropertyNotificationQueue;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while (false)
void Run() {
    PropertyNotificationQueue queue;
    CHECK(queue.Empty());
    PropertyNotificationQueue::Reservation failed;
    const std::string id="test.property";
    failAllocation=true;
    bool threw=false;
    try { (void)queue.Prepare(id,failed); } catch (const std::bad_alloc&) { threw=true; }
    failAllocation=false;
    CHECK(threw && queue.Empty());
    CHECK(queue.Prepare(id,failed)==MMS_OK);
    failAllocation=true;
    // A committed edit can publish despite allocator failure because the node
    // and its string were staged beforehand.
    queue.Publish(failed,42);
    failAllocation=false;
    CHECK(!queue.Empty() && queue.Front().first==id && queue.Front().second==42);
    queue.Pop(); CHECK(queue.Empty());
    {
        PropertyNotificationQueue::Reservation abandoned;
        CHECK(queue.Prepare(id,abandoned)==MMS_OK);
    }
    std::array<PropertyNotificationQueue::Reservation,64> held;
    for (auto& reservation:held) CHECK(queue.Prepare(id,reservation)==MMS_OK);
    PropertyNotificationQueue::Reservation overflow;
    CHECK(queue.Prepare(id,overflow)==MMS_E_BUSY);
    for (unsigned i=0;i<held.size();++i) queue.Publish(held[i],i+1);
    CHECK(queue.Prepare(id,overflow)==MMS_E_BUSY);
    for (unsigned i=0;i<held.size();++i) {
        CHECK(!queue.Empty() && queue.Front().second==i+1);
        queue.Pop();
    }
    CHECK(queue.Empty() && queue.Prepare(id,overflow)==MMS_OK);
    queue.Publish(overflow,100);CHECK(queue.Front().second==100);queue.Pop();
}
}
int main() {Run();std::printf("property_notification_queue_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;}
