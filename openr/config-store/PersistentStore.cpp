/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PersistentStore.h"

#include <chrono>

#include <folly/FileUtil.h>
#include <folly/io/IOBuf.h>

#include <openr/common/Util.h>

using std::exception;

namespace {

static const long kDbFlushRatio = 10000;

} // anonymous namespace

namespace openr {

PersistentStore::PersistentStore(
    const std::string& /* nodeName */, // TODO remove nodeName argument
    const std::string& storageFilePath,
    fbzmq::Context& /* context */, // TODO remove context argument
    bool dryrun,
    bool periodicallySaveToDisk)
    : storageFilePath_(storageFilePath), dryrun_(dryrun) {
  if (periodicallySaveToDisk) {
    // Create timer and backoff mechanism only if backoff is requested
    saveDbTimerBackoff_ =
        std::make_unique<ExponentialBackoff<std::chrono::milliseconds>>(
            Constants::kPersistentStoreInitialBackoff,
            Constants::kPersistentStoreMaxBackoff);

    saveDbTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
      if (savePersistentObjectToDisk()) {
        saveDbTimerBackoff_->reportSuccess();
      } else {
        // Report error and schedule next-try
        saveDbTimerBackoff_->reportError();
        saveDbTimer_->scheduleTimeout(
            saveDbTimerBackoff_->getTimeRemainingUntilRetry());
      }
    });
  }

  // Load initial database. On failure we will just report error and continue
  // with empty database
  if (not loadDatabaseFromDisk()) {
    LOG(ERROR) << "Failed to load config-database from file: "
               << storageFilePath_;
  }
}

PersistentStore::~PersistentStore() {
  saveDatabaseToDisk();
}

folly::SemiFuture<folly::Unit>
PersistentStore::store(std::string key, std::string value) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    key = std::move(key),
    value = std::move(value)
  ]() mutable noexcept {
    SYSLOG(INFO) << "Store key: " << key << ", value: " << value
                 << " to config-store";
    // Override previous value if any
    database_.keyVals[key] = value;
    auto pObject = toPersistentObject(ActionType::ADD, key, value);
    pObjects_.emplace_back(std::move(pObject));
    maybeSaveObjectToDisk();
    p.setValue();
  });
  return sf;
}

folly::SemiFuture<bool>
PersistentStore::erase(std::string key) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), key = std::move(key)]() mutable noexcept {
        SYSLOG(INFO) << "Erase key: " << key << " from config-store";
        if (database_.keyVals.erase(key) > 0) {
          auto pObject = toPersistentObject(ActionType::DEL, key, "");
          pObjects_.emplace_back(std::move(pObject));
          maybeSaveObjectToDisk();
          p.setValue(true);
        } else {
          LOG(WARNING) << "Key: " << key << " doesn't exist";
          p.setValue(false);
        }
      });
  return sf;
}

folly::SemiFuture<std::optional<std::string>>
PersistentStore::load(std::string key) {
  folly::Promise<std::optional<std::string>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), key = std::move(key)]() mutable {
        auto it = database_.keyVals.find(key);
        if (it != database_.keyVals.end()) {
          p.setValue(it->second);
        } else {
          p.setValue(std::nullopt);
        }
      });
  return sf;
}

void
PersistentStore::maybeSaveObjectToDisk() noexcept {
  if (not saveDbTimerBackoff_) {
    // This is primarily used for unit testing to save DB immediately
    // Block the response till file is saved
    savePersistentObjectToDisk();
  } else if (not saveDbTimer_->isScheduled()) {
    saveDbTimer_->scheduleTimeout(
        saveDbTimerBackoff_->getTimeRemainingUntilRetry());
  }
}

bool
PersistentStore::savePersistentObjectToDisk() noexcept {
  if (not dryrun_) {
    // Write PersistentObject to ioBuf
    std::vector<PersistentObject> newObjects;
    newObjects = std::move(pObjects_);

    auto queue = folly::IOBufQueue(folly::IOBufQueue::cacheChainLength());

    for (auto& pObject : newObjects) {
      auto buf = encodePersistentObject(pObject);
      if (buf.hasError()) {
        LOG(ERROR) << "Failed to encode PersistentObject to ioBuf. Error: "
                   << folly::exceptionStr(buf.error());
        return false;
      }
      queue.append(std::move(**buf));
    }

    // Append IoBuf to disk
    auto ioBuf = queue.move();
    auto success = writeIoBufToDisk(ioBuf, WriteType::APPEND);
    if (success.hasError()) {
      LOG(ERROR) << "Failed to write PersistentObject to file '"
                 << storageFilePath_
                 << "'. Error: " << folly::exceptionStr(success.error());
      return false;
    }

    numOfNewWritesToDisk_++;

    // Write the whole database to disk periodically
    if (numOfNewWritesToDisk_ >= kDbFlushRatio) {
      numOfNewWritesToDisk_ = 0;
      const auto startTs = std::chrono::steady_clock::now();
      if (not saveDatabaseToDisk()) {
        return false;
      }
      LOG(INFO) << "Updated database on disk. Took "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startTs)
                       .count()
                << "ms";
    }
  } else {
    VLOG(1) << "Skipping writing to disk in dryrun mode";
  }
  numOfWritesToDisk_++;

  return true;
}

bool
PersistentStore::saveDatabaseToDisk() noexcept {
  std::unique_ptr<folly::IOBuf> ioBuf;
  // If database is empty, write 'kTlvFormatMarker' to disk and return
  if (database_.keyVals.size() == 0) {
    ioBuf = folly::IOBuf::copyBuffer(
        kTlvFormatMarker.data(), kTlvFormatMarker.size());
  } else {
    // Append kTlvFormatMarker to queue
    auto queue = folly::IOBufQueue(folly::IOBufQueue::cacheChainLength());
    queue.append(kTlvFormatMarker.data(), kTlvFormatMarker.size());

    // Encode database_ and append to queue
    for (auto& keyPair : database_.keyVals) {
      PersistentObject pObject;
      pObject =
          toPersistentObject(ActionType::ADD, keyPair.first, keyPair.second);

      auto buf = encodePersistentObject(pObject);
      if (buf.hasError()) {
        LOG(ERROR) << "Failed to encode PersistentObject to ioBuf. Error:  "
                   << folly::exceptionStr(buf.error());
        return false;
      }
      queue.append(std::move(*buf));
    }
    // Write queue to disk
    ioBuf = queue.move();
  }

  auto success = writeIoBufToDisk(ioBuf, WriteType::WRITE);
  if (success.hasError()) {
    LOG(ERROR) << "Failed to write database to file '" << storageFilePath_
               << "'. Error: " << folly::exceptionStr(success.error());
    return false;
  }
  return true;
}

bool
PersistentStore::loadDatabaseFromDisk() noexcept {
  // Check if file exists
  if (not fs::exists(storageFilePath_)) {
    LOG(INFO) << "Storage file " << storageFilePath_ << " doesn't exists. "
              << "Starting with empty database";
    return true;
  }

  // Read data from file
  std::string fileData{""};
  if (not folly::readFile(storageFilePath_.c_str(), fileData)) {
    LOG(ERROR) << "Failed to read file contents from '" << storageFilePath_
               << "'. Error (" << errno << "): " << folly::errnoStr(errno);
    return false;
  }

  // Create IoBuf and cursor for loading data from disk
  auto ioBuf = folly::IOBuf::wrapBuffer(fileData.c_str(), fileData.size());
  folly::io::Cursor cursor(ioBuf.get());

  // Read 'kTlvFormatMarker' from ioBuf
  if (not cursor.canAdvance(kTlvFormatMarker.size()) or
      cursor.readFixedString(kTlvFormatMarker.size()) != kTlvFormatMarker) {
    // Load old Format and write TlvFormat
    auto oldSuccess = loadDatabaseOldFormat(ioBuf);
    if (oldSuccess.hasError()) {
      LOG(ERROR) << "Failed to read old-format file contents from '"
                 << storageFilePath_
                 << "'. Error: " << folly::exceptionStr(oldSuccess.error());
      return false;
    }
    return true;
  }
  // Load TlvFormat
  auto tlvSuccess = loadDatabaseTlvFormat(ioBuf);
  if (tlvSuccess.hasError()) {
    LOG(ERROR) << "Failed to read Tlv-format file contents from '"
               << storageFilePath_
               << "'. Error: " << folly::exceptionStr(tlvSuccess.error());
    return false;
  }
  return true;
}

folly::Expected<folly::Unit, std::string>
PersistentStore::loadDatabaseOldFormat(
    const std::unique_ptr<folly::IOBuf>& ioBuf) noexcept {
  // Parse ioBuf into `database_`
  try {
    thrift::StoreDatabase newDatabase;
    serializer_.deserialize(ioBuf.get(), newDatabase);
    database_ = std::move(newDatabase);
    // Write Tlv format to disk
    saveDatabaseToDisk();
  } catch (std::exception const& e) {
    return folly::makeUnexpected<std::string>(
        folly::exceptionStr(e).toStdString());
  }
  return folly::Unit();
}

folly::Expected<folly::Unit, std::string>
PersistentStore::loadDatabaseTlvFormat(
    const std::unique_ptr<folly::IOBuf>& ioBuf) noexcept {
  // Parse ioBuf to persistentObject and then to `database_`
  folly::io::Cursor cursor(ioBuf.get());
  thrift::StoreDatabase newDatabase;
  // Read 'kTlvFormatMarker'
  try {
    cursor.readFixedString(kTlvFormatMarker.size());
  } catch (std::out_of_range& e) {
    return folly::makeUnexpected<std::string>(
        folly::exceptionStr(e).toStdString());
  }
  // Iteratively read persistentObject from disk
  while (true) {
    // Read and decode into persistentObject
    auto optionalObject = decodePersistentObject(cursor);
    if (optionalObject.hasError()) {
      return folly::makeUnexpected(optionalObject.error());
    }

    // Read finish
    if (not optionalObject->has_value()) {
      break;
    }
    auto pObject = std::move(optionalObject->value());

    // Add/Delete persistentObject to/from 'newDatabase'
    if (pObject.type == ActionType::ADD) {
      newDatabase.keyVals[pObject.key] =
          pObject.data.has_value() ? pObject.data.value() : "";
    } else if (pObject.type == ActionType::DEL) {
      newDatabase.keyVals.erase(pObject.key);
    }
  }
  database_ = std::move(newDatabase);
  return folly::Unit();
}

// Write over or append IoBuf to disk atomically
folly::Expected<folly::Unit, std::string>
PersistentStore::writeIoBufToDisk(
    const std::unique_ptr<folly::IOBuf>& ioBuf, WriteType writeType) noexcept {
  std::string fileData("");
  try {
    ioBuf->coalesce();
    fileData = ioBuf->moveToFbString().toStdString();

    if (writeType == WriteType::WRITE) {
      // Write over
      folly::writeFileAtomic(storageFilePath_.c_str(), fileData, 0666);
    } else {
      // Append to file
      folly::writeFile(
          fileData,
          storageFilePath_.c_str(),
          O_WRONLY | O_APPEND | O_CREAT,
          0666);
    }
  } catch (std::exception const& e) {
    return folly::makeUnexpected<std::string>(
        folly::exceptionStr(e).toStdString());
  }
  return folly::Unit();
}

// A made up encoding of a PersistentObject.
folly::Expected<std::unique_ptr<folly::IOBuf>, std::string>
PersistentStore::encodePersistentObject(
    const PersistentObject& pObject) noexcept {
  // Create buf with reserved size
  auto buf = folly::IOBuf::create(
      sizeof(uint8_t) + sizeof(uint32_t) + pObject.key.size() +
      sizeof(uint32_t) +
      (pObject.data.has_value() ? pObject.data.value().size() : 0));

  folly::io::Appender appender(buf.get(), 0);
  try {
    // Append 'pObject.type' to buf
    appender.writeBE(static_cast<uint8_t>(pObject.type));
    // Append key length and key to buf
    appender.writeBE<uint32_t>(pObject.key.size());
    appender.push(folly::StringPiece(pObject.key));

    // If 'pObject.data' has value, append the length and the data to buf
    // Otherwise, append 0 to buf
    if (pObject.data.has_value()) {
      appender.writeBE<uint32_t>(pObject.data.value().size());
      appender.push(folly::StringPiece(pObject.data.value()));
    } else {
      appender.writeBE<uint32_t>(0);
    }
    return buf;
  } catch (const exception& e) {
    return folly::makeUnexpected<std::string>(
        folly::exceptionStr(e).toStdString());
  }
}

// A made up decoding of a PersistentObject.
folly::Expected<std::optional<PersistentObject>, std::string>
PersistentStore::decodePersistentObject(folly::io::Cursor& cursor) noexcept {
  // If nothing can be read, return
  if (not cursor.canAdvance(1)) {
    return std::nullopt;
  }

  PersistentObject pObject;
  try {
    // Read 'type'
    pObject.type = ActionType(cursor.readBE<uint8_t>());
    // Read key length and key
    auto length = cursor.readBE<uint32_t>();
    pObject.key = cursor.readFixedString(length);

    // Read data length and data
    length = cursor.readBE<uint32_t>();
    if (length == 0) {
      return pObject;
    }
    pObject.data = cursor.readFixedString(length);
    return pObject;
  } catch (std::out_of_range& e) {
    return folly::makeUnexpected<std::string>(
        folly::exceptionStr(e).toStdString());
  }
}

// Create a PersistentObject and assign value to it.
PersistentObject
PersistentStore::toPersistentObject(
    const ActionType type, const std::string& key, const std::string& data) {
  PersistentObject pObject;
  pObject.type = type;
  pObject.key = key;
  if (type == ActionType::ADD) {
    pObject.data = data;
  }
  return pObject;
}

} // namespace openr
