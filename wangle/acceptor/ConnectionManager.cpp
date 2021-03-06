/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <wangle/acceptor/ConnectionManager.h>

#include <glog/logging.h>
#include <folly/io/async/EventBase.h>

using folly::HHWheelTimer;
using std::chrono::milliseconds;

namespace folly { namespace wangle {

ConnectionManager::ConnectionManager(EventBase* eventBase,
    milliseconds timeout, Callback* callback)
  : connTimeouts_(new HHWheelTimer(eventBase)),
    callback_(callback),
    eventBase_(eventBase),
    idleIterator_(conns_.end()),
    idleLoopCallback_(this),
    timeout_(timeout),
    idleConnEarlyDropThreshold_(timeout_ / 2) {

}

void
ConnectionManager::addConnection(ManagedConnection* connection,
    bool timeout) {
  CHECK_NOTNULL(connection);
  ConnectionManager* oldMgr = connection->getConnectionManager();
  if (oldMgr != this) {
    if (oldMgr) {
      // 'connection' was being previously managed in a different thread.
      // We must remove it from that manager before adding it to this one.
      oldMgr->removeConnection(connection);
    }

    // put the connection into busy part first.  This should not matter at all
    // because the last callback for an idle connection must be onDeactivated(),
    // so the connection must be moved to idle part then.
    conns_.push_front(*connection);

    connection->setConnectionManager(this);
    if (callback_) {
      callback_->onConnectionAdded(*this);
    }
  }
  if (timeout) {
    scheduleTimeout(connection, timeout_);
  }
}

void
ConnectionManager::scheduleTimeout(ManagedConnection* const connection,
    std::chrono::milliseconds timeout) {
  if (timeout > std::chrono::milliseconds(0)) {
    connTimeouts_->scheduleTimeout(connection, timeout);
  }
}

void ConnectionManager::scheduleTimeout(
  folly::HHWheelTimer::Callback* callback,
  std::chrono::milliseconds timeout) {
  connTimeouts_->scheduleTimeout(callback, timeout);
}

void
ConnectionManager::removeConnection(ManagedConnection* connection) {
  if (connection->getConnectionManager() == this) {
    connection->cancelTimeout();
    connection->setConnectionManager(nullptr);

    // Un-link the connection from our list, being careful to keep the iterator
    // that we're using for idle shedding valid
    auto it = conns_.iterator_to(*connection);
    if (it == idleIterator_) {
      ++idleIterator_;
    }
    conns_.erase(it);

    if (callback_) {
      callback_->onConnectionRemoved(*this);
      if (getNumConnections() == 0) {
        callback_->onEmpty(*this);
      }
    }
  }
}

void
ConnectionManager::initiateGracefulShutdown(
  std::chrono::milliseconds idleGrace) {
  if (idleGrace.count() > 0) {
    idleLoopCallback_.scheduleTimeout(idleGrace);
    VLOG(3) << "Scheduling idle grace period of " << idleGrace.count() << "ms";
  } else {
    action_ = ShutdownAction::DRAIN2;
    VLOG(3) << "proceeding directly to closing idle connections";
  }
  drainAllConnections();
}

void
ConnectionManager::drainAllConnections() {
  DestructorGuard g(this);
  size_t numCleared = 0;
  size_t numKept = 0;

  auto it = idleIterator_ == conns_.end() ?
    conns_.begin() : idleIterator_;

  while (it != conns_.end() && (numKept + numCleared) < 64) {
    ManagedConnection& conn = *it++;
    if (action_ == ShutdownAction::DRAIN1) {
      conn.notifyPendingShutdown();
    } else {
      // Second time around: close idle sessions. If they aren't idle yet,
      // have them close when they are idle
      if (conn.isBusy()) {
        numKept++;
      } else {
        numCleared++;
      }
      conn.closeWhenIdle();
    }
  }

  if (action_ == ShutdownAction::DRAIN2) {
    VLOG(2) << "Idle connections cleared: " << numCleared <<
      ", busy conns kept: " << numKept;
  }
  if (it != conns_.end()) {
    idleIterator_ = it;
    eventBase_->runInLoop(&idleLoopCallback_);
  } else {
    action_ = ShutdownAction::DRAIN2;
  }
}

void
ConnectionManager::dropAllConnections() {
  DestructorGuard g(this);

  // Iterate through our connection list, and drop each connection.
  VLOG(3) << "connections to drop: " << conns_.size();
  idleLoopCallback_.cancelTimeout();
  unsigned i = 0;
  while (!conns_.empty()) {
    ManagedConnection& conn = conns_.front();
    conns_.pop_front();
    conn.cancelTimeout();
    conn.setConnectionManager(nullptr);
    // For debugging purposes, dump information about the first few
    // connections.
    static const unsigned MAX_CONNS_TO_DUMP = 2;
    if (++i <= MAX_CONNS_TO_DUMP) {
      conn.dumpConnectionState(3);
    }
    conn.dropConnection();
  }
  idleIterator_ = conns_.end();
  idleLoopCallback_.cancelLoopCallback();

  if (callback_) {
    callback_->onEmpty(*this);
  }
}

void
ConnectionManager::onActivated(ManagedConnection& conn) {
  auto it = conns_.iterator_to(conn);
  if (it == idleIterator_) {
    idleIterator_++;
  }
  conns_.erase(it);
  conns_.push_front(conn);
}

void
ConnectionManager::onDeactivated(ManagedConnection& conn) {
  auto it = conns_.iterator_to(conn);
  conns_.erase(it);
  conns_.push_back(conn);
  if (idleIterator_ == conns_.end()) {
    idleIterator_--;
  }
}

size_t
ConnectionManager::dropIdleConnections(size_t num) {
  VLOG(4) << "attempt to drop " << num << " idle connections";
  if (idleConnEarlyDropThreshold_ >= timeout_) {
    return 0;
  }

  size_t count = 0;
  while(count < num) {
    auto it = idleIterator_;
    if (it == conns_.end()) {
      return count; // no more idle session
    }
    auto idleTime = it->getIdleTime();
    if (idleTime == std::chrono::milliseconds(0) ||
          idleTime <= idleConnEarlyDropThreshold_) {
      VLOG(4) << "conn's idletime: " << idleTime.count()
              << ", earlyDropThreshold: " << idleConnEarlyDropThreshold_.count()
              << ", attempt to drop " << count << "/" << num;
      return count; // idleTime cannot be further reduced
    }
    ManagedConnection& conn = *it;
    idleIterator_++;
    conn.timeoutExpired();
    count++;
  }

  return count;
}


}} // folly::wangle
