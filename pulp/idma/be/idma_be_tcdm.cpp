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

#include <algorithm>
#include <vp/vp.hpp>
#include "idma_be_tcdm.hpp"



IDmaBeTcdm::IDmaBeTcdm(vp::Component *idma, std::string itf_name, IdmaBeProducer *be)
:   Block(idma, itf_name),
    fsm_event(this, &IDmaBeTcdm::fsm_handler)
{
    // Backend will be used later for interaction
    this->be = be;

    // Declare master port to TCDM interface
    this->traces.new_trace("trace", &this->trace, vp::DEBUG);

    // Declare our own trace so that we can individually activate traces
    idma->new_master_port(itf_name, &this->ico_itf);

    // Get the width of the TCDM interconnect. This is used to constrain the size of the
    // requests which are sent to the TCDM
    this->width = idma->get_js_config()->get_int("tcdm_width");

    // Max number of pending bursts
    this->burst_queue_maxsize = idma->get_js_config()->get_int("burst_queue_size");

    // A component which does not declare the property gets 0, keep at least one request
    this->tcdm_outstanding = idma->get_js_config()->get_int("tcdm_outstanding");
    if (this->tcdm_outstanding <= 0)
    {
        this->tcdm_outstanding = 1;
    }
    this->read_reqs.resize(this->tcdm_outstanding);

    // Local memory base
    this->loc_base = idma->get_js_config()->get_int("loc_base");
}



void IDmaBeTcdm::activate_burst()
{
    // Delivery side: it tracks the burst whose lines are coming back, always the front one.
    // The issue side runs ahead of it, see advance_read_issue.
    if (this->current_burst_size == 0 && this->burst_queue_size.size() > 0)
    {
        this->current_burst_base = this->burst_queue_base.front();
        this->current_burst_size = this->burst_queue_size.front();
    }
}



void IDmaBeTcdm::advance_read_issue()
{
    // Once a burst has been fully sent, roll on to the next one without waiting for it to come
    // back, otherwise the pipeline drains at every burst boundary. read_issue_idx is relative to
    // the front of the queue, so it shifts down whenever the front burst is retired.
    if (this->read_issue_size == 0
        && this->read_issue_idx < this->burst_queue_base.size()
        && !this->burst_queue_is_write[this->read_issue_idx])
    {
        this->read_issue_base = this->burst_queue_base[this->read_issue_idx];
        this->read_issue_size = this->burst_queue_size[this->read_issue_idx];
        this->read_issue_transfer = this->burst_queue_transfer[this->read_issue_idx];
        this->read_issue_idx++;
    }
}



void IDmaBeTcdm::enqueue_burst(uint64_t base, uint64_t size, bool is_write, IdmaTransfer *transfer)
{
    // Just enqueue the burst and trigger the FSM, the FSM will take care of sending the requests
    this->burst_queue_base.push_back(base);
    this->burst_queue_size.push_back(size);
    this->burst_queue_is_write.push_back(is_write);
    this->burst_queue_transfer.push_back(transfer);

    // We may need to activate the first burst
    this->activate_burst();

    // Trigger the FSM since we may need to start processing a burst
    this->fsm_event.enqueue();
}



// Called by the backend to enqueue a read burst
void IDmaBeTcdm::read_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size)
{
    this->enqueue_burst(base, size, false, transfer);
}



// Called by the backend to enqueue a write burst
void IDmaBeTcdm::write_burst(IdmaTransfer *transfer, uint64_t base, uint64_t size)
{
    this->enqueue_burst(base, size, true, transfer);
}



uint64_t IDmaBeTcdm::get_burst_size(uint64_t base, uint64_t size)
{
    // There is no constraint on burst size, this backend will anyway cut the burst into lines
    return size;
}


bool IDmaBeTcdm::can_accept_burst()
{
    // Accept a burst if we have room in the queue of pending bursts
    return (int)this->burst_queue_base.size() < this->burst_queue_maxsize;
}

bool IDmaBeTcdm::can_accept_data()
{
    // Take several chunks at once, so that the source back-end can keep its own requests in
    // flight instead of waiting for each chunk to land in the memory
    return this->write_chunks.size() < (uint32_t)this->tcdm_outstanding;
}



// Called by backend to push data for the current burst
void IDmaBeTcdm::write_data(IdmaTransfer *transfer, uint8_t *data, uint64_t size)
{
    this->trace.msg(vp::Trace::LEVEL_TRACE, "Writing data (size: 0x%lx)\n", size);

    // Allocate the destination address of this chunk now. The allocation cursor is independent
    // from the retire cursor, so a chunk of the next burst can be taken while the current burst
    // is still in flight.
    if (this->write_alloc_remaining == 0)
    {
        this->write_alloc_base = this->burst_queue_base[this->write_alloc_burst];
        this->write_alloc_remaining = this->burst_queue_size[this->write_alloc_burst];
    }

    this->write_chunks.push_back({transfer, data, size, this->write_alloc_base, 0, 0});

    this->write_alloc_base += size;
    this->write_alloc_remaining -= size;
    if (this->write_alloc_remaining == 0)
    {
        this->write_alloc_burst++;
    }

    // The FSM will take care of splitting the chunk into lines
    this->fsm_event.enqueue();
}



void IDmaBeTcdm::reset(bool active)
{
    if (active)
    {
        this->current_burst_size = 0;

        this->read_issue_base = 0;
        this->read_issue_size = 0;
        this->read_issue_transfer = NULL;
        this->read_issue_idx = 0;
        while (this->read_inflight.size() > 0)
        {
            this->read_inflight.pop();
        }
        while (this->free_read_reqs.size() > 0)
        {
            this->free_read_reqs.pop();
        }
        for (int i = 0; i < this->tcdm_outstanding; i++)
        {
            this->free_read_reqs.push(i);
        }

        this->write_chunks.clear();
        this->write_issue_chunk = 0;
        this->write_alloc_base = 0;
        this->write_alloc_remaining = 0;
        this->write_alloc_burst = 0;
        while (this->write_inflight.size() > 0)
        {
            this->write_inflight.pop();
        }

        this->last_line_timestamp = -1;
    }
}



void IDmaBeTcdm::write_line()
{
    WriteChunk &chunk = this->write_chunks[this->write_issue_chunk];

    // Extract one line from the chunk being issued
    uint64_t base = chunk.base + chunk.issued;
    uint64_t size = this->get_line_size(base, chunk.size - chunk.issued);

    this->trace.msg(vp::Trace::LEVEL_TRACE, "Writing line to TCDM (base: 0x%lx, size: 0x%lx)\n",
        base, size);

    // Prepare the line request
    vp::IoReq *req = &this->req;

    req->prepare();
    req->set_is_write(true);
    req->set_addr(base - this->loc_base);
    req->set_size(size);
    // The data belongs to the source back-end, it stays valid until we acknowledge the chunk
    req->set_data(chunk.data + chunk.issued);

    // Move the issue position on straight away, so that the next line can be sent while this
    // one is still in flight
    chunk.issued += size;
    if (chunk.issued == chunk.size)
    {
        this->write_issue_chunk++;
    }

    // Send request to TCDM
    vp::IoReqStatus status = this->ico_itf.req(req);
    if (status == vp::IoReqStatus::IO_REQ_INVALID)
    {
        trace.force_warning("Invalid access during TCDM write line (base: 0x%lx, size: 0x%lx)\n",
            base, size);
    }
    else if (status != vp::IoReqStatus::IO_REQ_OK)
    {
        // For now aynchronous replies are not supported since dma is always passing.
        // This could be needed if we want to model a more dynamic priority
        trace.fatal("Asynchronous response is not supported on TCDM backend\n");
    }

    // The reply is synchronous, so we already know when the line will have landed
    int64_t latency = (int64_t)req->get_latency();
    this->write_inflight.push({size, (int64_t)this->clock.get_cycles() + latency});

    this->fsm_event.enqueue(std::max((int64_t)1, latency));
}



uint64_t IDmaBeTcdm::get_line_size(uint64_t base, uint64_t size)
{
    // Make sure we don't go over interface width
    size = std::min(size, (uint64_t)this->width);

    // And that we don't cross the line
    uint64_t next_page = (base + this->width - 1) & ~(this->width - 1);
    if (next_page > base)
    {
        size = std::min(next_page - base, size);
    }

    return size;
}



void IDmaBeTcdm::remove_chunk_from_current_burst(uint64_t size)
{
    // Update current burst
    this->current_burst_base += size;
    this->current_burst_size -= size;

    if (this->current_burst_size == 0)
    {
        // In case it is the last chunk, remove the current burst
        this->burst_queue_base.pop_front();
        this->burst_queue_size.pop_front();
        this->burst_queue_is_write.pop_front();
        this->burst_queue_transfer.pop_front();

        // The issue and allocation cursors are relative to the front of the queue, so they
        // shift down with it
        if (this->read_issue_idx > 0)
        {
            this->read_issue_idx--;
        }
        if (this->write_alloc_burst > 0)
        {
            this->write_alloc_burst--;
        }

        // And take the next one
        this->activate_burst();

        this->be->update();
        this->update();
    }
}


void IDmaBeTcdm::read_line()
{
    // Take a free request from the pool. The caller checked that one is available.
    int req_id = this->free_read_reqs.front();
    this->free_read_reqs.pop();
    vp::IoReq *req = &this->read_reqs[req_id];

    // Extract the line from the position we have reached in the issue of the current burst
    uint64_t base = this->read_issue_base;
    uint64_t size = this->get_line_size(base, this->read_issue_size);
    IdmaTransfer *transfer = this->read_issue_transfer;

    this->trace.msg(vp::Trace::LEVEL_TRACE, "Reading line from TCDM (base: 0x%lx, size: 0x%lx)\n",
        base, size);

    // Prepare the IO request to TCDM
    req->prepare();
    req->set_is_write(false);
    req->set_addr(base - this->loc_base);
    req->set_size(size);
    // Several lines can be in flight and the destination keeps the data until the write is
    // done, so each line owns its buffer. Freed when the ack is received.
    req->set_data(new uint8_t[size]);

    // Send to TCDM
    vp::IoReqStatus status = this->ico_itf.req(req);
    if (status == vp::IoReqStatus::IO_REQ_INVALID)
    {
        trace.force_warning("Invalid access during TCDM read line (base: 0x%lx, size: 0x%lx)\n",
            base, size);
    }
    else if (status != vp::IoReqStatus::IO_REQ_OK)
    {
        // For now aynchronous replies are not supported since dma is always passing.
        // This could be needed if we want to model a more dynamic priority
        trace.fatal("Asynchronous response is not supported on TCDM backend\n");
    }

    // Move the issue position on straight away, so that the next line can be sent while this
    // one is still in flight
    this->read_issue_base += size;
    this->read_issue_size -= size;

    // The reply is synchronous, so we already know when the data can be handed over
    int64_t latency = (int64_t)req->get_latency();
    this->read_inflight.push({req_id, req->get_data(), size, transfer,
        (int64_t)this->clock.get_cycles() + latency});

    this->fsm_event.enqueue(std::max((int64_t)1, latency));
}



// Called by destination backend to ack the data we sent for writing
void IDmaBeTcdm::write_data_ack(uint8_t *data)
{
    // Release the data since we are now sure it won't be used anymore
    delete[] data;
    // And check if there is any action to take since backend may became ready
    this->update();
}



// This is called everytime we should check if any action should be taken
void IDmaBeTcdm::fsm_handler(vp::Block *__this, vp::ClockEvent *event)
{
    IDmaBeTcdm *_this = (IDmaBeTcdm *)__this;

    // Retire the write lines which have landed, in issue order, so that the chunks are
    // acknowledged in order, as the source back-end expects
    while (_this->write_inflight.size() > 0
        && _this->write_inflight.front().ready_timestamp <= _this->clock.get_cycles())
    {
        uint64_t size = _this->write_inflight.front().size;
        _this->write_inflight.pop();

        // Lines land in order, so this is always the first chunk
        WriteChunk &chunk = _this->write_chunks.front();
        chunk.acked += size;
        _this->remove_chunk_from_current_burst(size);

        if (chunk.acked == chunk.size)
        {
            // Acking may terminate the transfer and free it, so take a copy of what we need and
            // make our state consistent first
            IdmaTransfer *transfer = chunk.transfer;
            uint8_t *data = chunk.data;
            uint64_t chunk_size = chunk.size;

            _this->write_chunks.pop_front();
            _this->write_issue_chunk--;

            _this->be->update();
            // Keep last, it re-enters the source back-end and may terminate the transfer
            _this->be->ack_data(transfer, data, chunk_size);
        }
    }

    if (_this->write_inflight.size() > 0)
    {
        // Come back when the oldest line in flight has landed
        _this->fsm_event.enqueue(
            _this->write_inflight.front().ready_timestamp - _this->clock.get_cycles());
    }

    // Send the lines of the pending chunks, one per cycle, as long as the port still has room
    // for another outstanding access
    if (_this->write_issue_chunk < _this->write_chunks.size()
        && (int)_this->write_inflight.size() < _this->tcdm_outstanding
        && (_this->last_line_timestamp == -1
            || _this->last_line_timestamp < _this->clock.get_cycles()))
    {
        _this->last_line_timestamp = _this->clock.get_cycles();
        _this->write_line();
        _this->fsm_event.enqueue();
    }

    if (_this->burst_queue_is_write.size() > 0 && !_this->burst_queue_is_write.front())
    {
        _this->advance_read_issue();

        // At most one line per cycle, to respect the width of the TCDM port, and only while the
        // port still has a slot for another outstanding access
        if (_this->read_issue_size > 0 && _this->free_read_reqs.size() > 0
            && (_this->last_line_timestamp == -1
                || _this->last_line_timestamp < _this->clock.get_cycles()))
        {
            _this->last_line_timestamp = _this->clock.get_cycles();
            _this->read_line();
        }

        // Hand over at most one line per cycle, and always in issue order, since the
        // destination backend consumes the data sequentially.
        if (_this->read_inflight.size() > 0)
        {
            IDmaBeTcdm::PendingRead line = _this->read_inflight.front();

            if (_this->be->is_ready_to_accept_data(line.transfer))
            {
                if (line.ready_timestamp <= _this->clock.get_cycles())
                {
                    _this->read_inflight.pop();
                    _this->free_read_reqs.push(line.req_id);

                    // State must be consistent before write_data: it re-enters us synchronously
                    // through write_data_ack
                    _this->remove_chunk_from_current_burst(line.size);
                    _this->be->write_data(line.transfer, line.data, line.size);
                }
                else
                {
                    // Come back when the head of the queue has its data available
                    _this->fsm_event.enqueue(line.ready_timestamp - _this->clock.get_cycles());
                }
            }
        }

        bool more_to_send = _this->read_issue_size > 0
            || _this->read_issue_idx < _this->burst_queue_base.size();

        if ((more_to_send && _this->free_read_reqs.size() > 0)
            || _this->read_inflight.size() > 0)
        {
            _this->fsm_event.enqueue();
        }
    }
}



void IDmaBeTcdm::update()
{
    // All the checks are centralized in a new cycle in the FSM
    this->fsm_event.enqueue();
}



bool IDmaBeTcdm::is_empty()
{
    return this->burst_queue_base.empty();
}
