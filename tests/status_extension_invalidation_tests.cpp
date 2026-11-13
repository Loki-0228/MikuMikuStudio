#include "mikumikustudio/status_extension_invalidation.hpp"
#include <cstdio>
#include <stdexcept>
using mikumikustudio::StatusExtensionInvalidation;
namespace { unsigned checks = 0; void Check(bool ok) { if (!ok) throw ++checks; ++checks; } }
int main() {
    try {
        StatusExtensionInvalidation s; int hides = 0, posts = 0, prepares = 0, publishes = 0;
        auto hide = [&] { ++hides; return true; };
        auto post = [&] { ++posts; return true; };
        auto prep = [&] { ++prepares; return true; };
        auto pub = [&] { ++publishes; return true; };
        Check(s.Invalidate(hide) && s.dirty() && s.hidden() && s.revision()==1);
        Check(!s.TryPost([] { return false; }) && !s.queued());
        Check(s.TryPost(post) && s.TryPost(post) && posts==1 && s.queued());
        s.MessageDelivered(); Check(!s.queued());
        Check(!s.Recalculate(false,prep,pub,hide) && !prepares && s.dirty());
        Check(s.Recalculate(true,prep,pub,hide) && !s.dirty() && !s.hidden() && publishes==1);
        Check(!s.Invalidate([] { return false; }) && s.dirty() && !s.hidden());
        Check(!s.Recalculate(true,prep,pub,hide));
        Check(s.Invalidate(hide) && s.hidden());
        Check(!s.Recalculate(true,[&] { Check(!s.Invalidate(hide)); return true; },pub,hide));
        Check(s.dirty() && s.hidden() && publishes==1);
        Check(!s.Recalculate(true,prep,[&] { Check(!s.Invalidate(hide)); return true; },hide));
        Check(s.dirty() && s.hidden());
        Check(!s.Recalculate(true,[]() -> bool { throw std::runtime_error("prepare"); },pub,hide));
        Check(s.dirty() && s.hidden());
        Check(s.TryPost(post)); s.MessageDelivered();
        Check(s.Recalculate(true,prep,pub,hide));
        Check(!s.Close()); Check(s.Invalidate(hide) && s.Close() && s.closing());
        Check(!s.Invalidate(hide) && !s.TryPost(post)); s.MessageDelivered();
        std::printf("status_extension_invalidation_tests: %u checks passed\n", checks); return 0;
    } catch (unsigned n) { std::fprintf(stderr,"invalidation check %u failed\n",n); return 1; }
}
