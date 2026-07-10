/*
*  This file is part of aasdk library project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  aasdk is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  aasdk is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with aasdk. If not, see <http://www.gnu.org/licenses/>.
*/

#include <f1x/aasdk/Messenger/MessageInStream.hpp>
#include <f1x/aasdk/Error/Error.hpp>

namespace f1x
{
namespace aasdk
{
namespace messenger
{

MessageInStream::MessageInStream(boost::asio::io_service& ioService, transport::ITransport::Pointer transport, ICryptor::Pointer cryptor)
    : strand_(ioService)
    , transport_(std::move(transport))
    , cryptor_(std::move(cryptor))
{

}

void MessageInStream::startReceive(ReceivePromise::Pointer promise)
{
    strand_.dispatch([this, self = this->shared_from_this(), promise = std::move(promise)]() mutable {
        if(promise_ == nullptr)
        {
            promise_ = std::move(promise);

            auto transportPromise = transport::ITransport::ReceivePromise::defer(strand_);
            transportPromise->then(
                [this, self = this->shared_from_this()](common::Data data) mutable {
                    this->receiveFrameHeaderHandler(common::DataConstBuffer(data));
                },
                [this, self = this->shared_from_this()](const error::Error& e) mutable {
                    promise_->reject(e);
                    promise_.reset();
                });

            transport_->receive(FrameHeader::getSizeOf(), std::move(transportPromise));
        }
        else
        {
            promise->reject(error::Error(error::ErrorCode::OPERATION_IN_PROGRESS));
        }
    });
}

void MessageInStream::receiveFrameHeaderHandler(const common::DataConstBuffer& buffer)
{
    FrameHeader frameHeader(buffer);
    recentFrameType_ = frameHeader.getType();

    // Frames of different channels legally interleave on the wire: over
    // wireless TCP, GearHead inserts other channels' frames (e.g. audio)
    // between a split video message's FIRST and LAST frames. The original
    // single-buffer logic rejected that as MESSENGER_INTERTWINED_CHANNELS,
    // which killed the whole messenger -- observed live 2026-07-11 as every
    // wireless session freezing ~20s in, the moment video (split 16KB
    // frames) and audio streamed simultaneously. USB rarely interleaves,
    // which is why the wired path never tripped this. Reassemble per
    // channel instead of rejecting.
    const auto channelId = frameHeader.getChannelId();
    if(recentFrameType_ == FrameType::FIRST || recentFrameType_ == FrameType::BULK)
    {
        // Start of a new message; implicitly discards any half-assembled
        // predecessor left on this channel by a lost LAST frame.
        currentMessage_ = std::make_shared<Message>(channelId, frameHeader.getEncryptionType(), frameHeader.getMessageType());
        pendingMessages_[channelId] = currentMessage_;
    }
    else
    {
        const auto it = pendingMessages_.find(channelId);
        if(it != pendingMessages_.end())
        {
            currentMessage_ = it->second;
        }
        else
        {
            // MIDDLE/LAST with no FIRST in progress -- salvage what's
            // decodable rather than tearing every channel down.
            currentMessage_ = std::make_shared<Message>(channelId, frameHeader.getEncryptionType(), frameHeader.getMessageType());
            pendingMessages_[channelId] = currentMessage_;
        }
    }

    const size_t frameSize = FrameSize::getSizeOf(frameHeader.getType() == FrameType::FIRST ? FrameSizeType::EXTENDED : FrameSizeType::SHORT);

    auto transportPromise = transport::ITransport::ReceivePromise::defer(strand_);
    transportPromise->then(
        [this, self = this->shared_from_this()](common::Data data) mutable {
            this->receiveFrameSizeHandler(common::DataConstBuffer(data));
        },
        [this, self = this->shared_from_this()](const error::Error& e) mutable {
            this->resetReceiveState();
            promise_->reject(e);
            promise_.reset();
        });

    transport_->receive(frameSize, std::move(transportPromise));
}

void MessageInStream::receiveFrameSizeHandler(const common::DataConstBuffer& buffer)
{
    auto transportPromise = transport::ITransport::ReceivePromise::defer(strand_);
    transportPromise->then(
        [this, self = this->shared_from_this()](common::Data data) mutable {
            this->receiveFramePayloadHandler(common::DataConstBuffer(data));
        },
        [this, self = this->shared_from_this()](const error::Error& e) mutable {
            this->resetReceiveState();
            promise_->reject(e);
            promise_.reset();
        });

    FrameSize frameSize(buffer);
    transport_->receive(frameSize.getSize(), std::move(transportPromise));
}

void MessageInStream::receiveFramePayloadHandler(const common::DataConstBuffer& buffer)
{
    if(currentMessage_->getEncryptionType() == EncryptionType::ENCRYPTED)
    {
        try
        {
            // TLS records must be decrypted in wire order, and they are:
            // interleaving happens only at frame boundaries, and each
            // frame's payload is decrypted (appended into its own
            // channel's message) as soon as it arrives.
            cryptor_->decrypt(currentMessage_->getPayload(), buffer);
        }
        catch(const error::Error& e)
        {
            this->resetReceiveState();
            promise_->reject(e);
            promise_.reset();
            return;
        }
    }
    else
    {
        currentMessage_->insertPayload(buffer);
    }

    if(recentFrameType_ == FrameType::BULK || recentFrameType_ == FrameType::LAST)
    {
        pendingMessages_.erase(currentMessage_->getChannelId());
        promise_->resolve(std::move(currentMessage_));
        promise_.reset();
    }
    else
    {
        auto transportPromise = transport::ITransport::ReceivePromise::defer(strand_);
        transportPromise->then(
            [this, self = this->shared_from_this()](common::Data data) mutable {
                this->receiveFrameHeaderHandler(common::DataConstBuffer(data));
            },
            [this, self = this->shared_from_this()](const error::Error& e) mutable {
                this->resetReceiveState();
                promise_->reject(e);
                promise_.reset();
            });

        transport_->receive(FrameHeader::getSizeOf(), std::move(transportPromise));
    }
}

// A transport/SSL error ends the whole stream (the promise rejection fans
// out to every channel), so partially-assembled messages can never be
// completed -- drop them all.
void MessageInStream::resetReceiveState()
{
    currentMessage_.reset();
    pendingMessages_.clear();
}

}
}
}
