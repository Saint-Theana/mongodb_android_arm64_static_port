/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include <boost/optional.hpp>
#include <memory>

#include "mongo/s/chunk_writes_tracker.h"


namespace mongo {

/**
 * Drives state transitions and the status of a ChunkWritesTracker for a chunk
 * while it is splitting. Supports cancelability so that we don't lose
 * information from the writes tracker if the split errors. Should be created
 * when it is decided that a split should be performed and then passed along to
 * the ChunkSplitter which will drive these state changes.
 */
class ChunkSplitStateDriver final {
    ChunkSplitStateDriver(const ChunkSplitStateDriver&) = delete;
    ChunkSplitStateDriver& operator=(const ChunkSplitStateDriver&) = delete;

public:
    /**
     * Tries to lock the writesTracker for splitting, and if it succeeds,
     * constructs and returns a ChunkSplitStateDriver object. If it fails due to the
     * writesTracker already being locked, returns boost::none.
     */
    static std::shared_ptr<ChunkSplitStateDriver> tryInitiateSplit(
        std::shared_ptr<ChunkWritesTracker> writesTracker);

    /**
     * Sets the other pointer back to default initialized state so that it
     * doesn't try to cancel any ongoing split in its destructor. This
     * constructor is required for boost::optional.
     */
    ChunkSplitStateDriver(ChunkSplitStateDriver&& other) noexcept;

    /**
     * Not needed.
     */
    ChunkSplitStateDriver& operator=(ChunkSplitStateDriver&&) = delete;

    /**
     * If there's an ongoing split, cancels it. Otherwise does nothing.
     */
    ~ChunkSplitStateDriver();

    /**
     * Clears the current bytes written, but stashes them in a variable in case
     * the split is later canceled.
     */
    void prepareSplit();

    /**
     * In the case that we trigger a split but decide not to split due to the
     * actual size of a chunk on disk being too small, we update our estimate
     * by abandoning the stashed bytes we had written prior to prepare. That
     * way we won't continue to trigger splits on a chunk that is smaller than
     * we currently estimate it to be.
     */
    void abandonPrepare();

    /**
     * Marks the split as committed, which means that shouldSplit will
     * never again return true.
     */
    void commitSplit();

private:
    /**
     * Should only be used by tryInitiateSplit
     */
    ChunkSplitStateDriver(std::shared_ptr<ChunkWritesTracker> writesTracker)
        : _writesTracker(writesTracker), _splitState(SplitState::kSplitInProgress) {}

    /**
     * Pointer to the writes tracker object for which we're driving the split
     */
    std::weak_ptr<ChunkWritesTracker> _writesTracker;

    /**
     * Carries over result from prepare into cancelSplit.
     */
    uint64_t _stashedBytesWritten{0};

    /**
     * The current state of the chunk with respect to its progress being split.
     */
    enum class SplitState {
        kNotSplitting,
        kSplitInProgress,
        kSplitPrepared,
        kSplitCommitted,
    } _splitState{SplitState::kNotSplitting};
};

}  // namespace mongo
