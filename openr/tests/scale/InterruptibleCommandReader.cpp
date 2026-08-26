/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/InterruptibleCommandReader.h>

#include <poll.h>
#include <unistd.h>
#include <cerrno>

#include <utility>

namespace openr {
namespace {

void
emitCompleteCommands(
    std::string& input, const std::function<void(std::string)>& onCommand) {
  size_t offset{0};
  auto newline = input.find('\n', offset);
  while (newline != std::string::npos) {
    auto command = input.substr(offset, newline - offset);
    if (!command.empty() && command.back() == '\r') {
      command.pop_back();
    }
    onCommand(std::move(command));
    offset = newline + 1;
    newline = input.find('\n', offset);
  }
  input.erase(0, offset);
}

} // namespace

void
readCommandsUntilStopped(
    int inputFd,
    int stopFd,
    const std::function<void(std::string)>& onCommand) {
  std::string input;
  char buffer[4096];
  pollfd fds[] = {
      {.fd = inputFd, .events = POLLIN, .revents = 0},
      {.fd = stopFd, .events = POLLIN, .revents = 0},
  };

  while (true) {
    const int result = ::poll(fds, 2, -1);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }

    if (fds[1].revents != 0) {
      return;
    }
    if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0) {
      return;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP)) == 0) {
      continue;
    }

    const auto bytesRead = ::read(inputFd, buffer, sizeof(buffer));
    if (bytesRead > 0) {
      input.append(buffer, static_cast<size_t>(bytesRead));
      emitCompleteCommands(input, onCommand);
      continue;
    }
    if (bytesRead < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (!input.empty()) {
      onCommand(std::move(input));
    }
    return;
  }
}

} // namespace openr
