#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "LibunwindstackUnwinder.h"
#include "UprobesReturnAddressManager.h"

namespace LinuxTracing {

namespace {

class TestStack {
 public:
  explicit TestStack(uint64_t sp) : sp_{sp} {}

  TestStack(const TestStack&) = default;
  TestStack& operator=(const TestStack&) = default;
  TestStack(TestStack&&) = default;
  TestStack& operator=(TestStack&&) = default;

  bool operator==(const TestStack& o) const {
    if (sp_ != o.sp_) {
      return false;
    }
    if (data_.size() != o.data_.size()) {
      return false;
    }
    for (size_t i = 0; i < data_.size(); ++i) {
      if (data_[i] != o.data_[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const TestStack& o) const { return !(*this == o); }

  void Push(uint64_t value) {
    sp_ -= sizeof(uint64_t);
    data_.insert(data_.begin(), value);
  }

  void Pop() {
    sp_ += sizeof(uint64_t);
    data_.erase(data_.begin());
  }

  void HijackTop(uint64_t new_value) { data_[0] = new_value; }

  uint64_t GetSp() const { return sp_; }

  uint64_t GetTop() const { return data_[0]; }

  char* GetData() { return reinterpret_cast<char*>(data_.data()); }

  const char* GetData() const {
    return reinterpret_cast<const char*>(data_.data());
  }

  uint64_t GetSize() const { return data_.size() * sizeof(uint64_t); }

 private:
  uint64_t sp_;
  std::vector<uint64_t> data_{};
};

class TestHandler {
 public:
  explicit TestHandler(pid_t tid)
      : tid_{tid}, expected_stack_{256}, hijacked_stack_{expected_stack_} {}

  void OnNonUprobesCall() {
    // Fake pushing the return address.
    expected_stack_.Push(next_push_);
    hijacked_stack_.Push(next_push_);
    ++next_push_;

    // Fake pushing other data.
    expected_stack_.Push(next_push_);
    hijacked_stack_.Push(next_push_);
    ++next_push_;
  }

  void OnNonUretprobesReturn() {
    // Fake popping other data.
    expected_stack_.Pop();
    hijacked_stack_.Pop();

    // Fake popping the return address.
    expected_stack_.Pop();
    hijacked_stack_.Pop();
  }

  void OnUprobesCall(UprobesReturnAddressManager* return_address_manager) {
    // Fake pushing the return address.
    expected_stack_.Push(next_push_);
    hijacked_stack_.Push(next_push_);
    ++next_push_;

    return_address_manager->ProcessUprobes(tid_, hijacked_stack_.GetSp(),
                                           hijacked_stack_.GetTop());

    // Fake uretprobes hijacking the return address.
    hijacked_stack_.HijackTop(next_hijack_);
    ++next_hijack_;

    // Fake pushing other data.
    expected_stack_.Push(next_push_);
    hijacked_stack_.Push(next_push_);
    ++next_push_;
  }

  void OnUretprobesReturn(UprobesReturnAddressManager* return_address_manager) {
    // Fake popping other data.
    expected_stack_.Pop();
    hijacked_stack_.Pop();

    // Fake popping the return address.
    expected_stack_.Pop();
    hijacked_stack_.Pop();

    return_address_manager->ProcessUretprobes(tid_);
  }

  void OnUprobesOptimizedTailCall(
      UprobesReturnAddressManager* return_address_manager) {
    // Fake popping other data to clear the frame for the tail call.
    expected_stack_.Pop();
    hijacked_stack_.Pop();

    // Do not fake pushing the return address as this is an optimized tail call.

    return_address_manager->ProcessUprobes(tid_, hijacked_stack_.GetSp(),
                                           hijacked_stack_.GetTop());

    // Fake uretprobes hijacking the return address.
    hijacked_stack_.HijackTop(next_hijack_);
    ++next_hijack_;

    // Fake pushing other data.
    expected_stack_.Push(next_push_);
    hijacked_stack_.Push(next_push_);
    ++next_push_;
  }

  void OnUretprobesAfterTailCallReturn(
      UprobesReturnAddressManager* return_address_manager) {
    // Do not fake popping other data as this function had ended with a tail
    // call, its frame was clear.

    // Do not fake popping the return address as this function had ended with a
    // tail call, only the uretprobe is hit.

    return_address_manager->ProcessUretprobes(tid_);
  }

  TestStack PatchStackOnSample(
      UprobesReturnAddressManager* return_address_manager) {
    TestStack patched_stack{hijacked_stack_};
    return_address_manager->PatchSample(tid_, patched_stack.GetSp(),
                                        patched_stack.GetData(),
                                        patched_stack.GetSize());
    return patched_stack;
  }

  pid_t GetTid() const { return tid_; }

  const TestStack& GetExpectedStack() const { return expected_stack_; }

  const TestStack& GetHijackedStack() const { return hijacked_stack_; }

 private:
  pid_t tid_;
  TestStack expected_stack_;
  TestStack hijacked_stack_;

  uint64_t next_push_ = 42;
  uint64_t next_hijack_ = 1000;
};

}  // namespace

TEST(UprobesReturnAddressManager, NoUprobes) {
  UprobesReturnAddressManager return_address_manager;
  TestHandler test_handler{42};

  // Fake sample.
  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // Fake call to function A.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // Fake return of function B.
  test_handler.OnNonUretprobesReturn();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // A returns.
  test_handler.OnNonUretprobesReturn();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());
}

TEST(UprobesReturnAddressManager, OneUprobe) {
  UprobesReturnAddressManager return_address_manager;
  TestHandler test_handler{42};

  // A is called.
  test_handler.OnNonUprobesCall();

  // B is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C returns.
  test_handler.OnNonUretprobesReturn();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // A returns.
  test_handler.OnNonUretprobesReturn();
}

TEST(UprobesReturnAddressManager, DifferentThread) {
  UprobesReturnAddressManager return_address_manager;
  TestHandler test_handler{42};
  TestHandler other_test_handler{111};

  // A is called.
  test_handler.OnNonUprobesCall();

  // B is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  // C is called on the other thread.
  other_test_handler.OnNonUprobesCall();

  // Sample on the other thread.
  EXPECT_EQ(other_test_handler.PatchStackOnSample(&return_address_manager),
            other_test_handler.GetExpectedStack());

  // B returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  // Sample on the other thread.
  EXPECT_EQ(other_test_handler.PatchStackOnSample(&return_address_manager),
            other_test_handler.GetExpectedStack());

  // C returns (on the other thread).
  other_test_handler.OnNonUprobesCall();

  // A returns.
  test_handler.OnNonUretprobesReturn();
}

TEST(UprobesReturnAddressManager, TwoNestedUprobesAndAnotherUprobe) {
  UprobesReturnAddressManager return_address_manager;
  TestHandler test_handler{42};

  // A is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // D is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // D returns.
  test_handler.OnNonUretprobesReturn();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // E is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // E returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // A returns.
  test_handler.OnNonUretprobesReturn();
}

TEST(UprobesReturnAddressManager, TailCallOptimization) {
  UprobesReturnAddressManager return_address_manager;
  TestHandler test_handler{42};

  // A is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B is called and hits a uprobe.
  test_handler.OnUprobesCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C is called with tail-call optimization and hits a uprobe.
  test_handler.OnUprobesOptimizedTailCall(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // D is called.
  test_handler.OnNonUprobesCall();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // D returns.
  test_handler.OnNonUretprobesReturn();

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // C returns and hits a uretprobe.
  test_handler.OnUretprobesReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // B is not on the stack anymore because it had ended with a tail-call, but
  // its uretprobe is still hit.
  test_handler.OnUretprobesAfterTailCallReturn(&return_address_manager);

  EXPECT_EQ(test_handler.PatchStackOnSample(&return_address_manager),
            test_handler.GetExpectedStack());

  // A returns.
  test_handler.OnNonUretprobesReturn();
}

//==============================================================================
// Tests for frame pointer based callchains start here:
//==============================================================================

const std::string maps_string =
    "55d0f260c000-55d0f260d000 r--p 00000000 fe:00 3415204                    "
    "/usr/local/uprobes_target\n"
    "55d0f260d000-55d0f260f000 r-xp 00001000 fe:00 3415204                    "
    "/usr/local/uprobes_target\n"
    "55d0f260f000-55d0f2610000 r--p 00003000 fe:00 3415204                    "
    "/usr/local/uprobes_target\n"
    "55d0f2611000-55d0f2612000 r--p 00004000 fe:00 3415204                    "
    "/usr/local/uprobes_target\n"
    "55d0f2612000-55d0f2613000 rw-p 00005000 fe:00 3415204                    "
    "/usr/local/uprobes_target\n"
    "55d0f3ce1000-55d0f3d14000 rw-p 00000000 00:00 0                          "
    "[heap]\n"
    "7f075b495000-7f075b4d6000 rw-p 00000000 00:00 0 \n"
    "7f075b4f7000-7f075b4fb000 rw-p 00000000 00:00 0 \n"
    "7f075b4fb000-7f075b50a000 r--p 00000000 fe:00 2131083                    "
    "/usr/lib/x86_64-linux-gnu/libm-2.29.so\n"
    "7f075b50a000-7f075b5a5000 r-xp 0000f000 fe:00 2131083                    "
    "/usr/lib/x86_64-linux-gnu/libm-2.29.so\n"
    "7f075b5a5000-7f075b63e000 r--p 000aa000 fe:00 2131083                    "
    "/usr/lib/x86_64-linux-gnu/libm-2.29.so\n"
    "7f075b63e000-7f075b63f000 r--p 00142000 fe:00 2131083                    "
    "/usr/lib/x86_64-linux-gnu/libm-2.29.so\n"
    "7f075b63f000-7f075b640000 rw-p 00143000 fe:00 2131083                    "
    "/usr/lib/x86_64-linux-gnu/libm-2.29.so\n"
    "7f075b640000-7f075b665000 r--p 00000000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b665000-7f075b7ac000 r-xp 00025000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b7ac000-7f075b7f5000 r--p 0016c000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b7f5000-7f075b7f6000 ---p 001b5000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b7f6000-7f075b7f9000 r--p 001b5000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b7f9000-7f075b7fc000 rw-p 001b8000 fe:00 2131081                    "
    "/usr/lib/x86_64-linux-gnu/libc-2.29.so\n"
    "7f075b7fc000-7f075b800000 rw-p 00000000 00:00 0 \n"
    "7f075b800000-7f075b803000 r--p 00000000 fe:00 2112042                    "
    "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1\n"
    "7f075b803000-7f075b814000 r-xp 00003000 fe:00 2112042                    "
    "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1\n"
    "7f075b814000-7f075b818000 r--p 00014000 fe:00 2112042                    "
    "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1\n"
    "7f075b818000-7f075b819000 r--p 00017000 fe:00 2112042                    "
    "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1\n"
    "7f075b819000-7f075b81a000 rw-p 00018000 fe:00 2112042                    "
    "/usr/lib/x86_64-linux-gnu/libgcc_s.so.1\n"
    "7f075b81a000-7f075b8b0000 r--p 00000000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b8b0000-7f075b98b000 r-xp 00096000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b98b000-7f075b9d4000 r--p 00171000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b9d4000-7f075b9d5000 ---p 001ba000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b9d5000-7f075b9e0000 r--p 001ba000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b9e0000-7f075b9e3000 rw-p 001c5000 fe:00 2112089                    "
    "/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.28\n"
    "7f075b9e3000-7f075b9e6000 rw-p 00000000 00:00 0 \n"
    "7f075ba00000-7f075ba02000 rw-p 00000000 00:00 0 \n"
    "7f075ba02000-7f075ba03000 r--p 00000000 fe:00 2131077                    "
    "/usr/lib/x86_64-linux-gnu/ld-2.29.so\n"
    "7f075ba03000-7f075ba22000 r-xp 00001000 fe:00 2131077                    "
    "/usr/lib/x86_64-linux-gnu/ld-2.29.so\n"
    "7f075ba22000-7f075ba2a000 r--p 00020000 fe:00 2131077                    "
    "/usr/lib/x86_64-linux-gnu/ld-2.29.so\n"
    "7f075ba2a000-7f075ba2b000 r--p 00027000 fe:00 2131077                    "
    "/usr/lib/x86_64-linux-gnu/ld-2.29.so\n"
    "7f075ba2b000-7f075ba2c000 rw-p 00028000 fe:00 2131077                    "
    "/usr/lib/x86_64-linux-gnu/ld-2.29.so\n"
    "7f075ba2c000-7f075ba2d000 rw-p 00000000 00:00 0 \n"
    "7ffcae624000-7ffcae646000 rw-p 00000000 00:00 0                          "
    "[stack]\n"
    "7ffcae7f0000-7ffcae7f3000 r--p 00000000 00:00 0                          "
    "[vvar]\n"
    "7ffcae7f3000-7ffcae7f4000 r-xp 00000000 00:00 0                          "
    "[vdso]\n"
    "7fffffffe000-7ffffffff000 --xp 00000000 00:00 0                          "
    "[uprobes]";

auto maps = LibunwindstackUnwinder::ParseMaps(maps_string);

TEST(UprobesReturnAddressManager, CallchainNoUprobes) {
  UprobesReturnAddressManager return_address_manager;

  std::vector<uint64_t> expected_callchain{
      18446744073709551104lu, 94355907990078lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990078lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  EXPECT_TRUE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
  EXPECT_THAT(callchain_sample, testing::ElementsAreArray(expected_callchain));
}

TEST(UprobesReturnAddressManager, CallchainOneUprobe) {
  UprobesReturnAddressManager return_address_manager;

  std::vector<uint64_t> expected_callchain{
      18446744073709551104lu, 94355907990078lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  return_address_manager.ProcessUprobes(1, 140723234287848lu, 94355907990270lu);

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990078lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 140737488347136lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  EXPECT_TRUE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
  EXPECT_THAT(callchain_sample, testing::ElementsAreArray(expected_callchain));
}

TEST(UprobesReturnAddressManager, CallchainTwoUprobes) {
  UprobesReturnAddressManager return_address_manager;

  std::vector<uint64_t> expected_callchain{
      18446744073709551104lu, 94355907990063lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};
  return_address_manager.ProcessUprobes(1, 140723234287944lu, 94355907990423lu);
  return_address_manager.ProcessUprobes(1, 140723234287816lu, 94355907990220lu);

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990063lu,  94355907990120lu,
      94355907990170lu,       140737488347136lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu,  140737488347136lu,
      94355907990459lu,       94355907990731lu,  139669574937531lu,
      6143427251839320320lu};

  EXPECT_TRUE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
  EXPECT_THAT(callchain_sample, testing::ElementsAreArray(expected_callchain));
}

TEST(UprobesReturnAddressManager, CallchainTwoUprobesMissingOne) {
  UprobesReturnAddressManager return_address_manager;

  return_address_manager.ProcessUprobes(1, 140723234287816lu, 94355907990220lu);

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990063lu,  94355907990120lu,
      94355907990170lu,       140737488347136lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu,  140737488347136lu,
      94355907990459lu,       94355907990731lu,  139669574937531lu,
      6143427251839320320lu};

  EXPECT_FALSE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
}

TEST(UprobesReturnAddressManager, CallchainTwoConsecutiveUprobes) {
  UprobesReturnAddressManager return_address_manager;

  std::vector<uint64_t> expected_callchain{
      18446744073709551104lu, 94355907990063lu, 94355907990120lu,
      94355907990170lu,       94355907990220lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  return_address_manager.ProcessUprobes(1, 140723234287944lu, 94355907990423lu);
  return_address_manager.ProcessUprobes(1, 140723234287848lu, 94355907990270lu);
  return_address_manager.ProcessUprobes(1, 140723234287816lu, 94355907990220lu);

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990063lu,  94355907990120lu,
      94355907990170lu,       140737488347136lu, 140737488347136lu,
      94355907990320lu,       94355907990370lu,  140737488347136lu,
      94355907990459lu,       94355907990731lu,  139669574937531lu,
      6143427251839320320lu};

  EXPECT_TRUE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
  EXPECT_THAT(callchain_sample, testing::ElementsAreArray(expected_callchain));
}

TEST(UprobesReturnAddressManager, CallchainBeforeInjectionByUprobe) {
  UprobesReturnAddressManager return_address_manager;

  std::vector<uint64_t> expected_callchain{
      18446744073709551104lu, 94355907990137lu, 94355907990270lu,
      94355907990320lu,       94355907990370lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu, 139669574937531lu,
      6143427251839320320lu};

  return_address_manager.ProcessUprobes(1, 140723234287912lu, 94355907990370lu);
  return_address_manager.ProcessUprobes(1, 140723234287880lu, 94355907990320lu);
  return_address_manager.ProcessUprobes(1, 140723234287816lu, 94355907990220lu);

  std::vector<uint64_t> callchain_sample{
      18446744073709551104lu, 94355907990137lu,  94355907990270lu,
      140737488347136lu,      140737488347136lu, 94355907990423lu,
      94355907990459lu,       94355907990731lu,  139669574937531lu,
      6143427251839320320lu};

  EXPECT_TRUE(return_address_manager.PatchCallchain(
      1, callchain_sample.data(), callchain_sample.size(), maps.get()));
  EXPECT_THAT(callchain_sample, testing::ElementsAreArray(expected_callchain));
}

}  // namespace LinuxTracing
