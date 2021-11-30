// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "owt/quic_transport/sdk/impl/quic_transport_owt_server_impl.h"

#include <string.h>

#include "base/location.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/socket/udp_server_socket.h"
#include "net/third_party/quiche/src/quic/core/crypto/crypto_handshake.h"
#include "net/third_party/quiche/src/quic/core/crypto/quic_random.h"
#include "net/third_party/quiche/src/quic/core/quic_crypto_stream.h"
#include "net/third_party/quiche/src/quic/core/quic_data_reader.h"
#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/tls_server_handshaker.h"
#include "net/tools/quic/quic_simple_server_packet_writer.h"
#include "net/tools/quic/quic_simple_server_session_helper.h"
#include "net/quic/address_utils.h"

namespace net {

namespace {

const char kSourceAddressTokenSecret[] = "secret";
const size_t kNumSessionsToCreatePerSocketEvent = 16;

// Allocate some extra space so we can send an error if the client goes over
// the limit.
const int kReadBufferSize = 16 * quic::kMaxIncomingPacketSize;

}  // namespace


QuicTransportOWTServerImpl::QuicTransportOWTServerImpl(
    int port,
    std::unique_ptr<quic::ProofSource> proof_source,
    const quic::QuicConfig& config,
    const quic::QuicCryptoServerConfig::ConfigOptions& crypto_config_options,
    const quic::ParsedQuicVersionVector& supported_versions,
    base::Thread* io_thread,
    base::Thread* event_thread)
    : port_(port),
      version_manager_(supported_versions),
      helper_(
          new QuicChromiumConnectionHelper(&clock_,
                                           quic::QuicRandom::GetInstance())),
      alarm_factory_(new QuicChromiumAlarmFactory(
          base::ThreadTaskRunnerHandle::Get().get(),
          &clock_)),
      config_(config),
      crypto_config_options_(crypto_config_options),
      crypto_config_(kSourceAddressTokenSecret,
                     quic::QuicRandom::GetInstance(),
                     std::move(proof_source),
                     quic::KeyExchangeSource::Default()),
      read_pending_(false),
      synchronous_read_count_(0),
      read_buffer_(base::MakeRefCounted<IOBufferWithSize>(kReadBufferSize)),
      task_runner_(io_thread->task_runner()),
      event_runner_(event_thread->task_runner()),
      weak_factory_(this) {
  Initialize();
}

void QuicTransportOWTServerImpl::Initialize() {
#if MMSG_MORE
  use_recvmmsg_ = true;
#endif

  // If an initial flow control window has not explicitly been set, then use a
  // sensible value for a server: 1 MB for session, 64 KB for each stream.
  const uint32_t kInitialSessionFlowControlWindow = 1 * 1024 * 1024;  // 1 MB
  const uint32_t kInitialStreamFlowControlWindow = 64 * 1024;         // 64 KB
  if (config_.GetInitialStreamFlowControlWindowToSend() ==
      quic::kMinimumFlowControlSendWindow) {
    config_.SetInitialStreamFlowControlWindowToSend(
        kInitialStreamFlowControlWindow);
  }
  if (config_.GetInitialSessionFlowControlWindowToSend() ==
      quic::kMinimumFlowControlSendWindow) {
    config_.SetInitialSessionFlowControlWindowToSend(
        kInitialSessionFlowControlWindow);
  }

  std::unique_ptr<quic::CryptoHandshakeMessage> scfg(
      crypto_config_.AddDefaultConfig(helper_->GetRandomGenerator(),
                                      helper_->GetClock(),
                                      crypto_config_options_));
}

QuicTransportOWTServerImpl::~QuicTransportOWTServerImpl() {

}

int QuicTransportOWTServerImpl::Start() {
      task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&QuicTransportOWTServerImpl::StartOnCurrentThread, weak_factory_.GetWeakPtr()));
      return true;
}

void QuicTransportOWTServerImpl::StartOnCurrentThread() {
// Determine IP address to connect to from supplied hostname.
  net::IPAddress ip = net::IPAddress::IPv6AllZeros();

  std::unique_ptr<UDPServerSocket> socket(
      new UDPServerSocket(/*net_log=*/nullptr, NetLogSource()));

  socket->AllowAddressReuse();

  int rc = socket->Listen(net::IPEndPoint(ip, port_));
  if (rc < 0) {
    LOG(ERROR) << "Listen() failed: " << ErrorToString(rc);
  }

  // These send and receive buffer sizes are sized for a single connection,
  // because the default usage of QuicTransportOWTServerImpl is as a test server with
  // one or two clients.  Adjust higher for use with many clients.
  rc = socket->SetReceiveBufferSize(
      static_cast<int32_t>(quic::kDefaultSocketReceiveBuffer));
  if (rc < 0) {
    LOG(ERROR) << "SetReceiveBufferSize() failed: " << ErrorToString(rc);
  }

  rc = socket->SetSendBufferSize(320 * quic::kMaxIncomingPacketSize);
  if (rc < 0) {
    LOG(ERROR) << "SetSendBufferSize() failed: " << ErrorToString(rc);
  }

  rc = socket->GetLocalAddress(&server_address_);
  if (rc < 0) {
    LOG(ERROR) << "GetLocalAddress() failed: " << ErrorToString(rc);
  }

  DVLOG(1) << "Listening on " << server_address_.ToString();

  socket_.swap(socket);

  dispatcher_.reset(new quic::QuicTransportOWTDispatcher(
      &config_, &crypto_config_, &version_manager_,
      std::unique_ptr<quic::QuicConnectionHelperInterface>(helper_),
      std::unique_ptr<quic::QuicCryptoServerStream::Helper>(
          new QuicSimpleServerSessionHelper(quic::QuicRandom::GetInstance())),
      std::unique_ptr<quic::QuicAlarmFactory>(alarm_factory_), quic::kQuicDefaultConnectionIdLength, task_runner_.get(), event_runner_.get()));
  QuicSimpleServerPacketWriter* writer =
      new QuicSimpleServerPacketWriter(socket_.get(), dispatcher_.get());
  dispatcher_->InitializeWithWriter(writer);

  StartReading();

}

void QuicTransportOWTServerImpl::Stop() {
  // Before we shut down the epoll server, give all active sessions a chance to
  // notify clients that they're closing.
  dispatcher_->Shutdown();

  if (!socket_) {
    return;
  }
  socket_->Close();
  socket_.reset();
}

void QuicTransportOWTServerImpl::SetVisitor(QuicTransportServerInterface::Visitor* visitor) { 
  visitor_ = visitor; 
}

void QuicTransportOWTServerImpl::OnSessionCreated(quic::QuicTransportOWTServerSession* session) {
  visitor_->OnSession(session);
}

void QuicTransportOWTServerImpl::OnSessionClosed(quic::QuicTransportOWTServerSession* session) {

}

void QuicTransportOWTServerImpl::StartReading() {
  if (synchronous_read_count_ == 0) {
    // Only process buffered packets once per message loop.
    dispatcher_->ProcessBufferedChlos(kNumSessionsToCreatePerSocketEvent);
  }

  if (read_pending_) {
    return;
  }
  read_pending_ = true;

  int result = socket_->RecvFrom(
      read_buffer_.get(), read_buffer_->size(), &client_address_,
      base::BindOnce(&QuicTransportOWTServerImpl::OnReadComplete, base::Unretained(this)));

  if (result == ERR_IO_PENDING) {
    synchronous_read_count_ = 0;
    if (dispatcher_->HasChlosBuffered()) {
      // No more packets to read, so yield before processing buffered packets.
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::BindOnce(&QuicTransportOWTServerImpl::StartReading,
                                weak_factory_.GetWeakPtr()));
    }
    return;
  }

  if (++synchronous_read_count_ > 32) {
    synchronous_read_count_ = 0;
    // Schedule the processing through the message loop to 1) prevent infinite
    // recursion and 2) avoid blocking the thread for too long.
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(&QuicTransportOWTServerImpl::OnReadComplete,
                              weak_factory_.GetWeakPtr(), result));
  } else {
    OnReadComplete(result);
  }
}

void QuicTransportOWTServerImpl::OnReadComplete(int result) {
  read_pending_ = false;
  if (result == 0)
    result = ERR_CONNECTION_CLOSED;

  if (result < 0) {
    LOG(ERROR) << "QuicRawServer read failed: " << ErrorToString(result);
    Stop();
    return;
  }

  quic::QuicReceivedPacket packet(read_buffer_->data(), result,
                                  helper_->GetClock()->Now(), false);
  dispatcher_->ProcessPacket(
      ToQuicSocketAddress(server_address_),
      ToQuicSocketAddress(client_address_),
      packet);

  StartReading();
}

}  // namespace quic
