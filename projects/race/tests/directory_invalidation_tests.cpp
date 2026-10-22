#include "../src/platform/directory_invalidation.hpp"
#include "test.hpp"
#include <limits>

using namespace race;
namespace watch=platform::directory_watch;
int main(){
  watch::detail::Invalidation latch(7,8);
  auto initial=latch.snapshot();CHECK(initial.dirty&&initial.observation.generation==1&&initial.completed_batches==0);
  // Completion during initial scan and another while dirty: both advance.
  CHECK(latch.batch(false));auto first=latch.snapshot();CHECK(latch.batch(false));
  CHECK_ERROR(latch.acknowledge(initial.observation),ErrorCode::StaleHandle);
  CHECK_ERROR(latch.acknowledge(first.observation),ErrorCode::StaleHandle);
  auto current=latch.snapshot();CHECK(current.dirty&&current.coalesced_batches==2&&current.completed_batches==2);
  auto foreign=current.observation;++foreign.watcher;CHECK_ERROR(latch.acknowledge(foreign),ErrorCode::StaleHandle);
  CHECK(latch.acknowledge(current.observation));CHECK(!latch.snapshot().dirty);
  CHECK(latch.batch(true));CHECK(latch.snapshot().dirty&&latch.snapshot().overflow_batches==1);
  CHECK_ERROR(latch.acknowledge(current.observation),ErrorCode::StaleHandle);
  CHECK(latch.acknowledge(latch.snapshot().observation));
  // A ready native completion found by acknowledge's poll must stale the token.
  auto before_poll=latch.snapshot().observation;CHECK(latch.batch(false));
  CHECK_ERROR(latch.acknowledge(before_poll),ErrorCode::StaleHandle);
  auto rearm_before=latch.snapshot();latch.arm_failed(fail(ErrorCode::Overflow,"fixture immediate overflow"));
  CHECK(latch.snapshot().phase==watch::Phase::Watching&&latch.snapshot().overflow_batches==rearm_before.overflow_batches+1);
  CHECK_ERROR(latch.acknowledge(rearm_before.observation),ErrorCode::StaleHandle);
  latch.arm_failed(fail(ErrorCode::PlatformFailure,"fixture rearm failure"));
  CHECK(latch.snapshot().dirty);CHECK_ERROR(latch.acknowledge(latch.snapshot().observation),ErrorCode::Stopping);
  latch.stopped();CHECK(latch.snapshot().failure->code==ErrorCode::PlatformFailure);
  watch::detail::Invalidation bounded(8,2);CHECK(bounded.acknowledge(bounded.snapshot().observation));
  CHECK(bounded.batch(false));auto final=bounded.snapshot().observation;
  CHECK_ERROR(bounded.batch(true),ErrorCode::Exhausted);CHECK(bounded.snapshot().dirty&&bounded.snapshot().observation==final);
  CHECK_ERROR(bounded.acknowledge(final),ErrorCode::Stopping);CHECK_ERROR(bounded.batch(false),ErrorCode::Stopping);
  CHECK(bounded.snapshot().completed_batches==2&&bounded.snapshot().overflow_batches==1);
  watch::detail::Invalidation closing(9,std::numeric_limits<std::uint64_t>::max());
  closing.stop_requested();CHECK(closing.batch(false));closing.stopped();
  CHECK(closing.snapshot().phase==watch::Phase::Stopped&&closing.snapshot().completed_batches==1);
  CHECK_ERROR(closing.acknowledge(closing.snapshot().observation),ErrorCode::Stopping);
  std::puts("directory invalidation generations, coalescence, overflow and exhaustion passed");
}
