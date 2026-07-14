/*
 * Copyright (C) 2024 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Germain Haugou, ETH Zurich (germain.haugou@iis.ee.ethz.ch)
 */

#pragma once

#include <deque>
#include <queue>
#include <vector>
#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include "../idma.hpp"
#include "idma_be.hpp"

/**
 * @brief TCDM back-end
 *
 * This back-end can be used to interface directly with a local memory.
 *
 * Lines are pipelined: up to tcdm_outstanding accesses can be in flight at the same time, so
 * that the latency reported by the interconnect is overlapped instead of being paid once per
 * line. With tcdm_outstanding set to 1, it sends a single request and is blocked until it is
 * done, which is the original behaviour.
 */
class IDmaBeTcdm : public vp::Block, public IdmaBeConsumer
{
public:
    /**
     * @brief Construct a new TCDM back-end
     *
     * @param idma The top iDMA block.
     * @param itf_name Name of the TCDM interface where the back-end should send requests.
     * @param be The top back-end.
     */
    IDmaBeTcdm(vp::Component *idma, std::string itf_name, IdmaBeProducer *be);

    void reset(bool active) override;

    void update();
    void read_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size) override;
    void write_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size) override;
    void write_data(IdmaTransfer *transfer, uint8_t *data, uint64_t size) override;
    void write_data_ack(uint8_t *data) override;
    uint64_t get_burst_size(uint64_t base, uint64_t size) override;
    bool can_accept_burst() override;
    bool can_accept_data() override;
    bool is_empty() override;

private:
    // FSM handler, called to check if any action should be taken after something was updated
    static void fsm_handler(vp::Block *__this, vp::ClockEvent *event);
    // Get the size of the line which can be accessed on TCDM side, to respect the size of
    // the interconnect
    uint64_t get_line_size(uint64_t base, uint64_t size);
    // Write a line to TCDM
    void write_line();
    // Read a line from TCDM
    void read_line();
    // Remove a chunk of data from current burst. This is used to track when a burst is done
    void remove_chunk_from_current_burst(uint64_t size);
    // Extract first pending information to let FSM start writing and reading lines from it
    void activate_burst();
    // Move the read issue cursor on to the next queued burst once the current one has been fully
    // sent, so that lines keep being issued while earlier ones are still in flight
    void advance_read_issue();
    // Enqueue a new burst to the queue of pending bursts
    void enqueue_burst(uint64_t base, uint64_t size, bool is_write, IdmaTransfer *transfer);

    // Pointer to back-end, used for data synchronization
    IdmaBeProducer *be;
    // TCDM interface where requests can be posted
    vp::IoMaster ico_itf;
    // Trace for this block, messages will be displayed with this block's name
    vp::Trace trace;
    // Block FSM event, used to trigger all checks after something has been updated
    vp::ClockEvent fsm_event;
    // Width in bytes of the TCDM interface. Requests to the TCDM will be split to fit
    // this width
    int width;
    // Top property giving the size of the queue of pending bursts
    int burst_queue_maxsize;
    // Number of accesses which can be in flight at the same time
    int tcdm_outstanding;
    // Top parameter giving base address of local memory
    uint64_t loc_base;

    // Request used for TCDM writes, only one at the same time is possible
    vp::IoReq req;

    // A read line which has been sent to the TCDM and is waiting for its latency to expire
    struct PendingRead
    {
        // Index in read_reqs of the request used for this line
        int req_id;
        // Buffer holding the data read from the TCDM, freed when the destination acks it
        uint8_t *data;
        uint64_t size;
        // Captured at issue time: the issue cursor can be ahead of the burst being retired, so
        // the front of the burst queue is not necessarily this line's transfer
        IdmaTransfer *transfer;
        // Cycle at which the data can be handed over to the destination back-end
        int64_t ready_timestamp;
    };

    // Pool of requests used for read lines, so that several can be in flight at once
    std::vector<vp::IoReq> read_reqs;
    // Indexes of the requests in read_reqs which are currently free
    std::queue<int> free_read_reqs;
    // Read lines in flight, in issue order. They are delivered to the destination in that same
    // order, since the destination consumes the data sequentially
    std::queue<PendingRead> read_inflight;

    // Pending bursts. Deques, since the read side issues lines ahead of delivery and so has to
    // look past the front burst, which is still being retired
    std::deque<uint64_t> burst_queue_base;
    std::deque<uint64_t> burst_queue_size;
    std::deque<bool> burst_queue_is_write;
    std::deque<IdmaTransfer *> burst_queue_transfer;

    // Base address of currently active burst, the one which is being retired
    uint64_t current_burst_base;
    // Size of currently active burst, the one which is being retired
    uint64_t current_burst_size;

    // A chunk of data pushed by the source back-end. Several can be taken at once, so that the
    // source can keep its own requests in flight instead of waiting for each chunk to land
    struct WriteChunk
    {
        // Transfer, data pointer and size, kept verbatim: they are given back as is when acking
        IdmaTransfer *transfer;
        uint8_t *data;
        uint64_t size;
        // TCDM address of the first byte, allocated when the chunk is received
        uint64_t base;
        // Bytes already issued as lines
        uint64_t issued;
        // Bytes whose lines have already landed
        uint64_t acked;
    };

    // A write line which has been sent to the TCDM and is waiting for its latency to expire
    struct PendingWrite
    {
        uint64_t size;
        // Cycle at which the line has landed in the memory
        int64_t ready_timestamp;
    };

    // Chunks taken from the source back-end, popped once fully acknowledged
    std::deque<WriteChunk> write_chunks;
    // Index in the chunk queue of the chunk whose lines are being issued
    uint32_t write_issue_chunk;
    // Write lines in flight, in issue order
    std::queue<PendingWrite> write_inflight;
    // Address to be given to the next incoming chunk
    uint64_t write_alloc_base;
    // Remaining size of the burst from which chunk addresses are allocated
    uint64_t write_alloc_remaining;
    // Index in the burst queue of the burst from which chunk addresses are allocated
    uint32_t write_alloc_burst;

    // Address of the next read line to be issued. Tracked separately from current_burst_base,
    // which only advances on delivery, so lines can be issued ahead of delivery
    uint64_t read_issue_base;
    // Size of the active read burst which still has to be issued
    uint64_t read_issue_size;
    // Transfer of the burst whose lines are currently being issued
    IdmaTransfer *read_issue_transfer;
    // Index in the burst queue of the next burst to pick up for issuing
    uint32_t read_issue_idx;
    // Timestamp in cycles of the last time a line was read or written. Used to make sure we send
    // only one line per cycle
    int64_t last_line_timestamp;
};
