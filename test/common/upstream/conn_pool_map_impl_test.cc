#include <memory>
#include <vector>

#include "envoy/http/conn_pool.h"

#include "common/upstream/conn_pool_map_impl.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/conn_pool.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Invoke;
using testing::InvokeArgument;
using testing::NiceMock;
using testing::Return;
using testing::SaveArg;

namespace Envoy {
namespace Upstream {
namespace {

class ConnPoolMapImplTest : public testing::Test {
public:
  // Note, we could test with Http::ConnectionPool::MockInstance here, which would simplify the
  // test. However, it's nice to test against an actual interface we'll be using.
  using TestMap = ConnPoolMap<int, Http::ConnectionPool::Instance>;
  using TestMapPtr = std::unique_ptr<TestMap>;

  TestMapPtr makeTestMap() { return std::make_unique<TestMap>(dispatcher_, absl::nullopt); }

  TestMapPtr makeTestMapWithLimit(uint64_t limit) {
    return std::make_unique<TestMap>(dispatcher_, absl::make_optional(limit));
  }

  TestMap::PoolFactory getBasicFactory() {
    return [&]() {
      auto pool = std::make_unique<NiceMock<Http::ConnectionPool::MockInstance>>();
      ON_CALL(*pool, hasActiveConnections).WillByDefault(Return(false));
      mock_pools_.push_back(pool.get());
      return pool;
    };
  }

  // Returns a pool which claims it has active connections.
  TestMap::PoolFactory getActivePoolFactory() {
    return [&]() {
      auto pool = std::make_unique<NiceMock<Http::ConnectionPool::MockInstance>>();
      ON_CALL(*pool, hasActiveConnections).WillByDefault(Return(true));
      mock_pools_.push_back(pool.get());
      return pool;
    };
  }
  TestMap::PoolFactory getNeverCalledFactory() {
    return []() {
      EXPECT_TRUE(false);
      return nullptr;
    };
  }

  TestMap::PoolFactory getFactoryExpectDrainedCb(Http::ConnectionPool::Instance::DrainedCb* cb) {
    return [this, cb]() {
      auto pool = std::make_unique<NiceMock<Http::ConnectionPool::MockInstance>>();
      EXPECT_CALL(*pool, addDrainedCallback(_)).WillOnce(SaveArg<0>(cb));
      mock_pools_.push_back(pool.get());
      return pool;
    };
  }

protected:
  NiceMock<Event::MockDispatcher> dispatcher_;
  std::vector<NiceMock<Http::ConnectionPool::MockInstance>*> mock_pools_;
};

TEST_F(ConnPoolMapImplTest, TestMapIsEmptyOnConstruction) {
  TestMapPtr test_map = makeTestMap();

  EXPECT_EQ(test_map->size(), 0);
}

TEST_F(ConnPoolMapImplTest, TestAddingAConnPoolIncreasesSize) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  EXPECT_EQ(test_map->size(), 1);
}

TEST_F(ConnPoolMapImplTest, TestAddingTwoConnPoolsIncreasesSize) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());
  EXPECT_EQ(test_map->size(), 2);
}

TEST_F(ConnPoolMapImplTest, TestConnPoolReturnedMatchesCreated) {
  TestMapPtr test_map = makeTestMap();

  TestMap::OptPoolRef pool = test_map->getPool(1, getBasicFactory());
  EXPECT_EQ(&(pool.value().get()), mock_pools_[0]);
}

TEST_F(ConnPoolMapImplTest, TestConnSecondPoolReturnedMatchesCreated) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  TestMap::OptPoolRef pool = test_map->getPool(2, getBasicFactory());
  EXPECT_EQ(&(pool.value().get()), mock_pools_[1]);
}

TEST_F(ConnPoolMapImplTest, TestMultipleOfSameKeyReturnsOriginal) {
  TestMapPtr test_map = makeTestMap();

  TestMap::OptPoolRef pool1 = test_map->getPool(1, getBasicFactory());
  TestMap::OptPoolRef pool2 = test_map->getPool(2, getBasicFactory());

  EXPECT_EQ(&(pool1.value().get()), &(test_map->getPool(1, getBasicFactory()).value().get()));
  EXPECT_EQ(&(pool2.value().get()), &(test_map->getPool(2, getBasicFactory()).value().get()));
  EXPECT_EQ(test_map->size(), 2);
}

TEST_F(ConnPoolMapImplTest, TestEmptyClerWorks) {
  TestMapPtr test_map = makeTestMap();

  test_map->clear();
  EXPECT_EQ(test_map->size(), 0);
}

TEST_F(ConnPoolMapImplTest, TestClearEmptiesOutMap) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());

  test_map->clear();
  EXPECT_EQ(test_map->size(), 0);
}

TEST_F(ConnPoolMapImplTest, CallbacksPassedToPools) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());
  Http::ConnectionPool::Instance::DrainedCb cb1;
  EXPECT_CALL(*mock_pools_[0], addDrainedCallback(_)).WillOnce(SaveArg<0>(&cb1));
  Http::ConnectionPool::Instance::DrainedCb cb2;
  EXPECT_CALL(*mock_pools_[1], addDrainedCallback(_)).WillOnce(SaveArg<0>(&cb2));

  ReadyWatcher watcher;
  test_map->addDrainedCallback([&watcher] { watcher.ready(); });

  EXPECT_CALL(watcher, ready()).Times(2);
  cb1();
  cb2();
}

// Tests that if we add the callback first, it is passed along when pools are created later.
TEST_F(ConnPoolMapImplTest, CallbacksCachedAndPassedOnCreation) {
  TestMapPtr test_map = makeTestMap();

  ReadyWatcher watcher;
  test_map->addDrainedCallback([&watcher] { watcher.ready(); });

  Http::ConnectionPool::Instance::DrainedCb cb1;
  test_map->getPool(1, getFactoryExpectDrainedCb(&cb1));

  Http::ConnectionPool::Instance::DrainedCb cb2;
  test_map->getPool(2, getFactoryExpectDrainedCb(&cb2));

  EXPECT_CALL(watcher, ready()).Times(2);
  cb1();
  cb2();
}

// Tests that if we drain connections on an empty map, nothing happens.
TEST_F(ConnPoolMapImplTest, EmptyMapDrainConnectionsNop) {
  TestMapPtr test_map = makeTestMap();
  test_map->drainConnections();
}

// Tests that we forward drainConnections to the pools.
TEST_F(ConnPoolMapImplTest, DrainConnectionsForwarded) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());
  EXPECT_CALL(*mock_pools_[0], drainConnections());
  EXPECT_CALL(*mock_pools_[1], drainConnections());

  test_map->drainConnections();
}

TEST_F(ConnPoolMapImplTest, ClearDefersDelete) {
  TestMapPtr test_map = makeTestMap();

  Http::ConnectionPool::Instance::DrainedCb cb1;
  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());
  test_map->clear();

  EXPECT_EQ(dispatcher_.to_delete_.size(), 2);
}

TEST_F(ConnPoolMapImplTest, GetPoolHittingLimitFails) {
  TestMapPtr test_map = makeTestMapWithLimit(1);

  test_map->getPool(1, getBasicFactory());
  ON_CALL(*mock_pools_[0], hasActiveConnections()).WillByDefault(Return(true));
  auto opt_pool = test_map->getPool(2, getNeverCalledFactory());

  EXPECT_FALSE(opt_pool.has_value());
  EXPECT_EQ(test_map->size(), 1);
}

TEST_F(ConnPoolMapImplTest, GetPoolHittingLimitGreaterThan1Fails) {
  TestMapPtr test_map = makeTestMapWithLimit(2);

  test_map->getPool(1, getActivePoolFactory());
  test_map->getPool(2, getActivePoolFactory());
  auto opt_pool = test_map->getPool(3, getNeverCalledFactory());

  EXPECT_FALSE(opt_pool.has_value());
  EXPECT_EQ(test_map->size(), 2);
}

TEST_F(ConnPoolMapImplTest, GetPoolLimitHitThenOneFreesUpNextCallSucceeds) {
  TestMapPtr test_map = makeTestMapWithLimit(1);

  test_map->getPool(1, getActivePoolFactory());
  test_map->getPool(2, getNeverCalledFactory());

  ON_CALL(*mock_pools_[0], hasActiveConnections()).WillByDefault(Return(false));

  auto opt_pool = test_map->getPool(2, getBasicFactory());

  EXPECT_TRUE(opt_pool.has_value());
  EXPECT_EQ(test_map->size(), 1);
}

// Test that only the pool which are idle are actually cleared
TEST_F(ConnPoolMapImplTest, GetOnePoolIdleOnlyClearsThatOne) {
  TestMapPtr test_map = makeTestMapWithLimit(2);

  // Get a pool which says it's not active.
  test_map->getPool(1, getBasicFactory());

  // Get one that *is* active.
  auto opt_pool = test_map->getPool(2, getActivePoolFactory());

  // this should force out #1
  auto new_pool = test_map->getPool(3, getBasicFactory());

  // Get 2 again. It should succeed, but not invoke the factory.
  auto opt_pool2 = test_map->getPool(2, getNeverCalledFactory());

  EXPECT_TRUE(opt_pool.has_value());
  EXPECT_TRUE(new_pool.has_value());
  EXPECT_EQ(&(opt_pool.value().get()), &(opt_pool2.value().get()));
  EXPECT_EQ(test_map->size(), 2);
}

// Show that even if all pools are idle, we only free up one as necessary
TEST_F(ConnPoolMapImplTest, GetPoolLimitHitManyIdleOnlyOneFreed) {
  TestMapPtr test_map = makeTestMapWithLimit(3);

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getBasicFactory());
  test_map->getPool(3, getBasicFactory());
  auto opt_pool = test_map->getPool(4, getBasicFactory());

  ASSERT_TRUE(opt_pool.has_value());
  EXPECT_EQ(test_map->size(), 3);
}

// Show that if we hit the limit once, then again with the same keys, we don't clean out the
// previously cleaned entries. Essentially, ensure we clean up any state related to being full.
TEST_F(ConnPoolMapImplTest, GetPoolFailStateIsCleared) {
  TestMapPtr test_map = makeTestMapWithLimit(2);

  test_map->getPool(1, getBasicFactory());
  test_map->getPool(2, getActivePoolFactory());
  test_map->getPool(3, getBasicFactory());

  // At this point, 1 should be cleared out. Let's get it again, then trigger a full condition.
  auto opt_pool = test_map->getPool(1, getActivePoolFactory());
  EXPECT_TRUE(opt_pool.has_value());

  // We're full. Because pool 1  and 2 are busy, the next call should fail.
  auto opt_pool_failed = test_map->getPool(4, getNeverCalledFactory());
  EXPECT_FALSE(opt_pool_failed.has_value());

  EXPECT_EQ(test_map->size(), 2);
}

// The following tests only die in debug builds, so don't run them if this isn't one.
#if !defined(NDEBUG)
class ConnPoolMapImplDeathTest : public ConnPoolMapImplTest {};

TEST_F(ConnPoolMapImplDeathTest, ReentryClearTripsAssert) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  ON_CALL(*mock_pools_[0], addDrainedCallback(_))
      .WillByDefault(Invoke([](Http::ConnectionPool::Instance::DrainedCb cb) { cb(); }));

  EXPECT_DEATH_LOG_TO_STDERR(test_map->addDrainedCallback([&test_map] { test_map->clear(); }),
                             ".*Details: A resource should only be entered once");
}

TEST_F(ConnPoolMapImplDeathTest, ReentryGetPoolTripsAssert) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  ON_CALL(*mock_pools_[0], addDrainedCallback(_))
      .WillByDefault(Invoke([](Http::ConnectionPool::Instance::DrainedCb cb) { cb(); }));

  EXPECT_DEATH_LOG_TO_STDERR(
      test_map->addDrainedCallback([&test_map, this] { test_map->getPool(2, getBasicFactory()); }),
      ".*Details: A resource should only be entered once");
}

TEST_F(ConnPoolMapImplDeathTest, ReentryDrainConnectionsTripsAssert) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  ON_CALL(*mock_pools_[0], addDrainedCallback(_))
      .WillByDefault(Invoke([](Http::ConnectionPool::Instance::DrainedCb cb) { cb(); }));

  EXPECT_DEATH_LOG_TO_STDERR(
      test_map->addDrainedCallback([&test_map] { test_map->drainConnections(); }),
      ".*Details: A resource should only be entered once");
}

TEST_F(ConnPoolMapImplDeathTest, ReentryAddDrainedCallbackTripsAssert) {
  TestMapPtr test_map = makeTestMap();

  test_map->getPool(1, getBasicFactory());
  ON_CALL(*mock_pools_[0], addDrainedCallback(_))
      .WillByDefault(Invoke([](Http::ConnectionPool::Instance::DrainedCb cb) { cb(); }));

  EXPECT_DEATH_LOG_TO_STDERR(
      test_map->addDrainedCallback([&test_map] { test_map->addDrainedCallback([]() {}); }),
      ".*Details: A resource should only be entered once");
}
#endif // !defined(NDEBUG)

} // namespace
} // namespace Upstream
} // namespace Envoy