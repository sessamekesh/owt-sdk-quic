/*
 * Copyright (C) 2019 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "owt/quic/logging.h"
#include "owt/quic_transport/sdk/impl/quic_transport_factory_impl.h"
#include "base/at_exit.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "owt/quic_transport/sdk/impl/proof_source_owt.h"
#include "owt/quic_transport/sdk/impl/quic_transport_owt_client_impl.h"
#include "owt/quic_transport/sdk/impl/quic_transport_owt_server_impl.h"
#include "net/quic/crypto/proof_source_chromium.h"
#include "net/quic/platform/impl/quic_chromium_clock.h"
#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/third_party/quiche/src/quic/core/crypto/proof_source.h"
#include "net/third_party/quiche/src/quic/core/crypto/quic_crypto_server_config.h"
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "url/gurl.h"
#include "net/quic/address_utils.h"
#include "net/tools/quic/synchronous_host_resolver.h"
#include "net/base/privacy_mode.h"

namespace quic {

// FakeProofSource for server
class FakeProofSource : public quic::ProofSource {
 public:
  FakeProofSource() {}
  ~FakeProofSource() override {}

  void GetProof(const quic::QuicSocketAddress& server_address,
                const quic::QuicSocketAddress& client_address,
                const std::string& hostname,
                const std::string& server_config,
                quic::QuicTransportVersion transport_version,
                absl::string_view chlo_hash,
                std::unique_ptr<Callback> callback) override {
    quic::QuicReferenceCountedPointer<ProofSource::Chain> chain =
        GetCertChain(server_address, client_address, hostname);
    quic::QuicCryptoProof proof;
    proof.signature = "fake signature";
    proof.leaf_cert_scts = "fake timestamp";
    callback->Run(true, chain, proof, nullptr);
  }

  quic::QuicReferenceCountedPointer<Chain> GetCertChain(
      const quic::QuicSocketAddress& server_address,
      const ::quic::QuicSocketAddress& client_address,
      const std::string& hostname) override {
    std::vector<std::string> certs;
    certs.push_back("fake cert");
    return quic::QuicReferenceCountedPointer<ProofSource::Chain>(
        new ProofSource::Chain(certs));
  }

  void ComputeTlsSignature(
      const quic::QuicSocketAddress& server_address,
      const ::quic::QuicSocketAddress& client_address,
      const std::string& hostname,
      uint16_t signature_algorithm,
      absl::string_view in,
      std::unique_ptr<SignatureCallback> callback) override {
    callback->Run(true, "fake signature", nullptr);
  }

  ProofSource::TicketCrypter* GetTicketCrypter() override {
    return nullptr;
}
};

// FakeProofVerifier for client
class FakeProofVerifier : public quic::ProofVerifier {
 public:
  quic::QuicAsyncStatus VerifyProof(
      const string& hostname,
      const uint16_t port,
      const string& server_config,
      quic::QuicTransportVersion quic_version,
      absl::string_view  chlo_hash,
      const std::vector<string>& certs,
      const string& cert_sct,
      const string& signature,
      const quic::ProofVerifyContext* context,
      string* error_details,
      std::unique_ptr<quic::ProofVerifyDetails>* details,
      std::unique_ptr<quic::ProofVerifierCallback> callback) override {
    return quic::QUIC_SUCCESS;
  }

  quic::QuicAsyncStatus VerifyCertChain(
      const std::string& hostname,
      const uint16_t port,
      const std::vector<std::string>& certs,
      const std::string& ocsp_response,
      const std::string& cert_sct,
      const quic::ProofVerifyContext* verify_context,
      std::string* error_details,
      std::unique_ptr<quic::ProofVerifyDetails>* verify_details,
      uint8_t* out_alert,
      std::unique_ptr<quic::ProofVerifierCallback> callback) override {
    return quic::QUIC_SUCCESS;
  }

  std::unique_ptr<quic::ProofVerifyContext> CreateDefaultContext() override {
    return nullptr;
  }
};

QuicTransportFactory* QuicTransportFactory::Create() {
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("quic_transport_thread_pool");
  QuicTransportFactoryImpl* factory = new QuicTransportFactoryImpl();
  factory->InitializeAtExitManager();
  return factory;
}

QuicTransportFactoryImpl::QuicTransportFactoryImpl()
    : at_exit_manager_(nullptr),
      io_thread_(std::make_unique<base::Thread>("quic_transport_io_thread")),
      event_thread_(
          std::make_unique<base::Thread>("quic_transport_event_thread")) {
  io_thread_->StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0));
  event_thread_->StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0));
  Init();
}

QuicTransportFactoryImpl::~QuicTransportFactoryImpl() = default;

void QuicTransportFactoryImpl::InitializeAtExitManager() {
  at_exit_manager_ = std::make_unique<base::AtExitManager>();
}

QuicTransportServerInterface* QuicTransportFactoryImpl::CreateQuicTransportServer(
    int port,
    const char* cert_file,
    const char* key_file) {
  std::unique_ptr<net::ProofSourceChromium> proof_source(
          new net::ProofSourceChromium());
  CHECK(proof_source->Initialize(
      base::FilePath(cert_file),
      base::FilePath(key_file), base::FilePath()));

  net::IPAddress ip = net::IPAddress::IPv6AllZeros();
            quic::QuicConfig config;

  return new net::QuicTransportOWTServerImpl(
                port,
                std::move(proof_source), config, 
                quic::QuicCryptoServerConfig::ConfigOptions(),
                quic::AllSupportedVersions(), 
                io_thread_.get(), event_thread_.get());
}

void QuicTransportFactoryImpl::Init() {
  base::CommandLine::Init(0, nullptr);
  base::CommandLine* command_line(base::CommandLine::ForCurrentProcess());
  command_line->AppendSwitch("--quic_default_to_bbr");
  owt::quic::Logging::InitLogging();
}

QuicTransportClientInterface*
QuicTransportFactoryImpl::CreateQuicTransportClient(
    const char* host, 
    int port) {
  QuicTransportClientInterface* result(nullptr);
  base::WaitableEvent done(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                           base::WaitableEvent::InitialState::NOT_SIGNALED);
  io_thread_->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](const char* host, int port,
             base::Thread* io_thread, base::Thread* event_thread,
             QuicTransportClientInterface** result, base::WaitableEvent* event) {
            std::unique_ptr<quic::ProofVerifier> proof_verifier;
            proof_verifier.reset(new ProofVerifierOwt());
            quic::QuicIpAddress ip_addr;

            GURL url("https://www.example.org");
            
            if (!ip_addr.FromString(host)) {
              net::AddressList addresses;
              int rv = net::SynchronousHostResolver::Resolve(host, &addresses);
              if (rv != net::OK) {
                LOG(ERROR) << "Unable to resolve '" << host
                           << "' : " << net::ErrorToShortString(rv);
                std::cerr << "Unable to resolve '" << host
                           << "' : " << net::ErrorToShortString(rv);
                return;
              }
              ip_addr =
                  net::ToQuicIpAddress(addresses[0].address());
            }

            quic::QuicServerId server_id(url.host(), url.EffectiveIntPort(),
                                         net::PRIVACY_MODE_DISABLED);
            quic::ParsedQuicVersionVector versions = quic::CurrentSupportedVersions();

            *result = new net::QuicTransportOWTClientImpl(
                quic::QuicSocketAddress(ip_addr, port), server_id, versions, std::move(proof_verifier),
                io_thread, event_thread);
            event->Signal();
          },
          base::Unretained(host), port, base::Unretained(io_thread_.get()),
          base::Unretained(event_thread_.get()), base::Unretained(&result),
          base::Unretained(&done)));
  done.Wait();
  return result;
}

}  // namespace quic
