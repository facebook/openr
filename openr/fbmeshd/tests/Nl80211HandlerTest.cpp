/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/FileUtil.h>
#include <folly/test/TestUtils.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/common/ErrorCodes.h>

using namespace openr::fbmeshd;

using ::testing::HasSubstr;

std::string
exec(const char* cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::shared_ptr<FILE> pipe{popen(cmd, "r"), pclose};
  if (!pipe)
    throw std::runtime_error("popen() failed!");
  while (!feof(pipe.get())) {
    if (fgets(buffer.data(), 128, pipe.get()) != nullptr)
      result += buffer.data();
  }
  return result;
}

// The fixture for testing class Nl80211Handler.
class Nl80211HandlerTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    if (getuid()) {
      SKIP() << "Test must be run as root";
      return;
    }
    auto hwsim_output =
        exec("modprobe -r mac80211-hwsim; modprobe mac80211-hwsim radios=6");
    VLOG(1) << "Reloaded mac80211-hwsim";

    // If we ever test anything with encryption or userspace peering, we will
    // crash because this global pointer is initialised; clear it between tests
    Nl80211Handler::globalNlHandler = nullptr;
    VLOG(1) << "Cleared globalNlHandler pointer";

    // Because the gflags session persists until the death of the executable
    // (past the end of a single test), configuration for a given test can leak
    // past the end of that test, if the following test doesn't specifically
    // re-set a flag.
    //
    // To avoid issues, pre-set command line flags to defaults (as in
    // run_fbmeshd.sh) before each test. This also allows us to confidently only
    // change individual flags that we're testing below, so we don't need to
    // provide e.g. a mesh ID on tests of other flags.
    std::array<const char*, 16> constDefaultArgs{
        "Nl80211HandlerTest",
        "--enable_encryption=false",
        "--encryption_password=",
        "--encryption_sae_groups=19",
        "--mesh_center_freq1=5805",
        "--mesh_channel_type=20",
        "--mesh_frequency=5805",
        "--mesh_id=mesh-soma",
        // Different from run_fbmeshd.sh in order to avoid any collisions with
        // the fbmeshd service automatically started, even on qemu
        "--mesh_ifname=meshtest",
        "--mesh_mac_address=",
        "--mesh_max_peer_links=32",
        "--mesh_rssi_threshold=-80",
        "--mesh_ttl=31",
        "--mesh_ttl_element=31",
        "--mesh_hwmp_active_path_timeout=30000",
        "--mesh_hwmp_rann_interval=3000"};
    int argCount = constDefaultArgs.size();
    char** args = const_cast<char**>(constDefaultArgs.data());

    gflags::ParseCommandLineFlags(&argCount, &args, false);
  }

  fbzmq::ZmqEventLoop zmqLoop_;

  Nl80211HandlerTest() {}
};

// Basic test that valid flags get set as expected
TEST_F(Nl80211HandlerTest, ConfigurationBasicTest) {
  std::array<const char*, 12> constArgs{"Nl80211HandlerTest",
                                        "--mesh_id=bazooka",
                                        "--mesh_ifname=mesh2",
                                        "--mesh_frequency=5825",
                                        "--mesh_channel_type=20",
                                        "--mesh_rssi_threshold=-75",
                                        "--mesh_mac_address=00:22:44:66:88:aa",
                                        "--mesh_ttl=12",
                                        "--mesh_ttl_element=13",
                                        "--mesh_hwmp_active_path_timeout=30000",
                                        "--mesh_hwmp_rann_interval=3000",
                                        "--mesh_max_peer_links=10"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_NO_THROW(({ Nl80211Handler nlHandler{zmqLoop_, false}; }));

  ASSERT_EQ("bazooka", FLAGS_mesh_id);
  ASSERT_EQ("mesh2", FLAGS_mesh_ifname);
  ASSERT_EQ(5825, FLAGS_mesh_frequency);
  ASSERT_EQ("20", FLAGS_mesh_channel_type);
  ASSERT_EQ(-75, FLAGS_mesh_rssi_threshold);
  ASSERT_EQ("00:22:44:66:88:aa", FLAGS_mesh_mac_address);
  ASSERT_EQ(12, FLAGS_mesh_ttl);
  ASSERT_EQ(13, FLAGS_mesh_ttl_element);
  ASSERT_EQ(30000, FLAGS_mesh_hwmp_active_path_timeout);
  ASSERT_EQ(3000, FLAGS_mesh_hwmp_rann_interval);
  ASSERT_EQ(10, FLAGS_mesh_max_peer_links);
}

// Check that invalid channel type isn't accepted
TEST_F(Nl80211HandlerTest, ConfigurationInvalidChannelType) {
  std::array<const char*, 2> constArgs{"Nl80211HandlerTest",
                                       "--mesh_channel_type=HT40"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW(
      ({
        Nl80211Handler nlHandler{zmqLoop_, false};
      }),
      std::invalid_argument);
}

// Test that we throws an exception on too long of a mesh_id
TEST_F(Nl80211HandlerTest, ConfigurationTooLongMeshId) {
  std::array<const char*, 2> constArgs{
      "Nl80211HandlerTest",
      "--mesh_id=bazookaTESTlonglonglongIwantthistobelongbutevenlongeristhislongenoughIsurehopeso"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW(
      ({
        Nl80211Handler nlHandler{zmqLoop_, false};
      }),
      std::invalid_argument);
}

// Test that we throws an exception on too long of an ifname
TEST_F(Nl80211HandlerTest, ConfigurationTooLongIfName) {
  std::array<const char*, 2> constArgs{
      "Nl80211HandlerTest",
      "--mesh_ifname=blahblahblahblahblahblahblahblahblahblahblahblahblahblahblahblahblahblah"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW(
      ({
        Nl80211Handler nlHandler{zmqLoop_, false};
      }),
      std::invalid_argument);
}

// Test that we throws an exception on too long of an encryption_password
TEST_F(Nl80211HandlerTest, ConfigurationTooLongPassword) {
  std::array<const char*, 3> constArgs{
      "Nl80211HandlerTest",
      "--enable_encryption=true",
      "--encryption_password=this is really quite secret, is it not? Yeah, I think it is. But we need too long of a password, because this should fail!"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW(
      ({
        Nl80211Handler nlHandler{zmqLoop_, false};
      }),
      std::invalid_argument);
}

// Test that we truncate too many encryption_sae_groups
TEST_F(Nl80211HandlerTest, ConfigurationTooManySaeGroups) {
  std::array<const char*, 4> constArgs{
      "Nl80211HandlerTest",
      "--enable_encryption=true",
      "--encryption_password=password",
      "--encryption_sae_groups=19,26,21,25,20,1,2,3,4,5,6,7,8,9"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW(
      ({
        Nl80211Handler nlHandler{zmqLoop_, false};
      }),
      std::invalid_argument);
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerFindsNetInterfaces) {
  Nl80211Handler nlHandler{zmqLoop_, false};

  // Check if iw found the same number of mesh-capable interfaces
  auto iw_output = exec("iw phy");
  int occurrences = 0;
  std::string::size_type start = 0;
  std::string join_mesh_substr = "join_mesh";

  while ((start = iw_output.find(join_mesh_substr, start)) !=
         std::string::npos) {
    ++occurrences;
    start += join_mesh_substr.length();
  }
  ASSERT_EQ(occurrences, nlHandler.getNumberOfMeshPhys());
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerObtainsFrequencies) {
  Nl80211Handler nlHandler{zmqLoop_, false};

  // Check if we found all the frequencies supported by a phy
  const NetInterface& netIf = nlHandler.lookupNetifFromPhy(0);
  const int CHANNEL_1_FREQ{2412};
  const int CHANNEL_165_FREQ{5825};
  const int INVALID_FREQ1{1000};
  const int INVALID_FREQ2{2000};
  ASSERT_TRUE(netIf.isFrequencySupported(CHANNEL_1_FREQ));
  ASSERT_TRUE(netIf.isFrequencySupported(CHANNEL_165_FREQ));
  ASSERT_FALSE(netIf.isFrequencySupported(INVALID_FREQ1));
  ASSERT_FALSE(netIf.isFrequencySupported(INVALID_FREQ2));
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerWithInvalidFrequency) {
  // Confirm that we throw if we get an invalid frequency
  std::array<const char*, 2> constArgs{"Nl80211HandlerTest",
                                       "--mesh_frequency=3000"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  ASSERT_THROW({ Nl80211Handler(zmqLoop_, false); }, std::runtime_error);
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerAppliesMeshParams) {
  // TODO: T40934912: Use 2.4 GHz because 5 GHz doesn't work on qemu
  std::array<const char*, 9> constArgs{"Nl80211HandlerTest",
                                       "--mesh_frequency=2412",
                                       "--mesh_center_freq1=2412",
                                       "--mesh_channel_type=20",
                                       "--mesh_rssi_threshold=-75",
                                       "--mesh_mac_address=00:22:44:66:88:aa",
                                       "--mesh_ttl=12",
                                       "--mesh_ttl_element=13",
                                       "--mesh_max_peer_links=10"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  Nl80211Handler nlHandler{zmqLoop_, false};

  ASSERT_EQ(nlHandler.joinMeshes(), R_SUCCESS);

  auto test_output = exec("iw dev meshtest get mesh_param mesh_rssi_threshold");
  ASSERT_NE(std::string::npos, test_output.find("-75", 0));

  test_output = exec("ip addr show dev meshtest");
  ASSERT_NE(std::string::npos, test_output.find("00:22:44:66:88:aa", 0));

  test_output = exec("iw dev meshtest get mesh_param mesh_ttl");
  ASSERT_NE(std::string::npos, test_output.find("12", 0));

  test_output = exec("iw dev meshtest get mesh_param mesh_element_ttl");
  ASSERT_NE(std::string::npos, test_output.find("13", 0));

  test_output = exec("iw dev meshtest get mesh_param mesh_max_peer_links");
  ASSERT_NE(std::string::npos, test_output.find("10", 0));
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerLookUpMeshNetInterface) {
  // Confirm that we correctly look up the single mesh NetInterface
  Nl80211Handler nlHandler{zmqLoop_, false};

  ASSERT_TRUE(nlHandler.lookupMeshNetif().maybeIfName);
  ASSERT_EQ(nlHandler.lookupMeshNetif().maybeIfName.value(), "meshtest");
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerJoinLeaveMesh) {
  // Confirm that we can join and leave a mesh successfully
  // TODO: T40934912: Use 2.4 GHz because 5 GHz doesn't work on qemu
  std::array<const char*, 4> constArgs{"Nl80211HandlerTest",
                                       "--mesh_frequency=2412",
                                       "--mesh_center_freq1=2412",
                                       "--mesh_channel_type=20"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  Nl80211Handler nlHandler{zmqLoop_, false};

  ASSERT_EQ(nlHandler.joinMeshes(), R_SUCCESS);
  ASSERT_EQ(nlHandler.leaveMeshes(), R_SUCCESS);
}

TEST_F(Nl80211HandlerTest, Nl80211HandlerGetMesh) {
  // TODO: T40934912: Use 2.4 GHz because 5 GHz doesn't work on qemu
  std::array<const char*, 4> constArgs{"Nl80211HandlerTest",
                                       "--mesh_frequency=2412",
                                       "--mesh_center_freq1=2412",
                                       "--mesh_channel_type=20"};
  int argCount = constArgs.size();
  char** args = const_cast<char**>(constArgs.data());

  gflags::ParseCommandLineFlags(&argCount, &args, false);
  Nl80211Handler nlHandler{zmqLoop_, false};

  nlHandler.joinMeshes();
  thrift::Mesh mesh = nlHandler.getMesh();
  ASSERT_EQ(mesh.frequency, 2412);
  ASSERT_EQ(mesh.centerFreq1, 2412);
  ASSERT_EQ(mesh.channelWidth, 1);
}

int
main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
