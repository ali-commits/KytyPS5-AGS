#include "receiver.h"
#include "usbSink.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/tcp.h>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace ControllerAudio::Receiver {
namespace {
using Json                         = nlohmann::json;
volatile std::sig_atomic_t stopped = 0;
void                       Stop(int) {
	stopped = 1;
}
struct Fd {
	int value = -1;
	~Fd() {
		if (value >= 0) ::close(value);
	}
};
using Ctx        = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Ssl        = std::unique_ptr<SSL, decltype(&SSL_free)>;
uint64_t stop_at = UINT64_MAX;
bool     Running() {
	return !stopped && NowUs() < stop_at;
}
void Require(bool yes, const char* message) {
	if (!yes) throw std::runtime_error(message);
}
void Private(const std::string& path, bool directory = false) {
	struct stat s {};
	Require(lstat(path.c_str(), &s) == 0 && s.st_uid == getuid() && !(s.st_mode & 0077) &&
	            (directory ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode)),
	        "identity/config must be owner-only regular files (directory 0700)");
}
std::string Hex(std::span<const unsigned char> bytes) {
	std::string out;
	for (auto b: bytes) {
		out.push_back("0123456789abcdef"[b >> 4]);
		out.push_back("0123456789abcdef"[b & 15]);
	}
	return out;
}
Frame Control(Type type, uint64_t generation, uint64_t epoch, uint64_t sequence,
              const Json& json = Json()) {
	std::string text = json.is_null() ? "" : json.dump();
	return {{type, Kind::Control, 0, generation, epoch, 0, 0, 0, sequence, 0, 0},
	        {text.begin(), text.end()}};
}
bool Poll(int fd, short events, uint64_t deadline) {
	while (Running() && NowUs() < deadline) {
		pollfd p {fd, events, 0};
		auto timeout = static_cast<int>(std::min<uint64_t>(50, (deadline - NowUs() + 999) / 1000));
		int  result  = ::poll(&p, 1, std::max(0, timeout));
		if (result < 0 && errno == EINTR) continue;
		if (result < 0 || (result > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))))
			return false;
		if (result > 0 && (p.revents & events)) return true;
	}
	return false;
}
bool Want(SSL* ssl, int result, int fd, uint64_t deadline) {
	auto error = SSL_get_error(ssl, result);
	return error == SSL_ERROR_WANT_READ    ? Poll(fd, POLLIN, deadline)
	       : error == SSL_ERROR_WANT_WRITE ? Poll(fd, POLLOUT, deadline)
	                                       : false;
}
void Send(SSL* ssl, int fd, const Frame& frame) {
	auto wire = Encode(frame);
	Require(!wire.empty(), "outgoing frame invalid");
	size_t   offset   = 0;
	uint64_t deadline = NowUs() + 1000000;
	while (offset < wire.size() && Running()) {
		size_t bytes  = 0;
		int    result = SSL_write_ex(ssl, wire.data() + offset, wire.size() - offset, &bytes);
		if (result == 1) {
			offset += bytes;
			continue;
		}
		Require(Want(ssl, result, fd, deadline), "TLS write deadline or disconnect");
	}
	Require(offset == wire.size(), "TLS write interrupted");
}
void Connect(Fd& socket, const Config& config) {
	sockaddr_storage address {};
	socklen_t        size = 0;
	auto*            v4   = reinterpret_cast<sockaddr_in*>(&address);
	auto*            v6   = reinterpret_cast<sockaddr_in6*>(&address);
	if (inet_pton(AF_INET, config.host.c_str(), &v4->sin_addr) == 1) {
		v4->sin_family = AF_INET;
		v4->sin_port   = htons(config.port);
		size           = sizeof(*v4);
	} else if (inet_pton(AF_INET6, config.host.c_str(), &v6->sin6_addr) == 1) {
		v6->sin6_family = AF_INET6;
		v6->sin6_port   = htons(config.port);
		size            = sizeof(*v6);
	} else
		throw std::runtime_error("host must be a numeric IPv4/IPv6 address for bounded connect");
	socket.value = ::socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	Require(socket.value >= 0, "socket failed");
	int one = 1;
	setsockopt(socket.value, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	int result = ::connect(socket.value, reinterpret_cast<sockaddr*>(&address), size);
	Require(result == 0 || (errno == EINPROGRESS && Poll(socket.value, POLLOUT, NowUs() + 3000000)),
	        "connect deadline or refusal");
	int       error = 0;
	socklen_t n     = sizeof(error);
	Require(getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &n) == 0 && error == 0,
	        "connect failed");
}
void PeerPin(SSL* ssl, const std::string& pin) {
	auto* cert = SSL_get1_peer_certificate(ssl);
	Require(cert, "server certificate absent");
	std::array<unsigned char, 32> digest {};
	unsigned                      count = 0;
	bool valid = X509_digest(cert, EVP_sha256(), digest.data(), &count) == 1 && count == 32 &&
	             X509_cmp_current_time(X509_get0_notBefore(cert)) < 0 &&
	             X509_cmp_current_time(X509_get0_notAfter(cert)) > 0;
	X509_free(cert);
	Require(valid && Hex(digest) == pin, "server certificate pin/time mismatch");
}
void Connection(const Config& config, Session& session) {
	session.clock = {};
	struct Cleanup {
		Session& session;
		~Cleanup() { session.Disconnect(); }
	} cleanup {session};
	Ctx ctx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	Require(bool(ctx), "TLS context failed");
	Require(SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) == 1, "TLS minimum failed");
	SSL_CTX_set_options(ctx.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
	// Pinned self-signed identities are intentional; the full leaf SHA256 and validity
	// are checked after key possession is proven, before any application frame.
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });
	Require(SSL_CTX_use_certificate_chain_file(ctx.get(), config.cert.c_str()) == 1 &&
	            SSL_CTX_use_PrivateKey_file(ctx.get(), config.key.c_str(), SSL_FILETYPE_PEM) == 1 &&
	            SSL_CTX_check_private_key(ctx.get()) == 1,
	        "receiver identity invalid");
	Fd socket;
	Connect(socket, config);
	Ssl ssl(SSL_new(ctx.get()), SSL_free);
	Require(bool(ssl) && SSL_set_fd(ssl.get(), socket.value) == 1, "TLS socket binding failed");
	uint64_t deadline = NowUs() + 5000000;
	for (;;) {
		int result = SSL_connect(ssl.get());
		if (result == 1) break;
		Require(Want(ssl.get(), result, socket.value, deadline),
		        "TLS handshake deadline or authentication failure");
	}
	PeerPin(ssl.get(), config.pinned_host_sha256);
	uint64_t sequence = 0;
	Send(ssl.get(), socket.value,
	     Control(Type::Hello, 0, 0, sequence++,
	             {{"protocol_version", 1},
	              {"receiver_id", config.receiver_id},
	              {"controller", 0},
	              {"output", "usb-dualsense-4ch"}}));
	Framer               framer;
	std::optional<Frame> pending;
	uint64_t             next_ping = 0, last_pong = NowUs(), ping_sent = 0, pending_since = 0,
	                     last_read = NowUs();
	auto                 ready     = [&] {
		if (pending && session.clock.Fresh(NowUs())) {
			if (!session.Handle(*pending, NowUs())) throw std::runtime_error(session.Error());
			Send(ssl.get(), socket.value,
			     Control(Type::Ready, session.Generation(), session.Epoch(), sequence++));
			pending.reset();
		}
	};
	auto consume = [&](const Frame& frame) {
		auto& h = frame.header;
		if (h.type == Type::Pong) {
			auto j = Json::parse(frame.payload.begin(), frame.payload.end(), nullptr, false);
			Require(!j.is_discarded() && j.is_object(), "invalid Pong");
			for (auto key: {"client_send_us", "server_receive_us", "server_send_us"})
				Require(j.contains(key) && j[key].is_number_unsigned(),
				        "invalid Pong clock fields");
			auto sent = j["client_send_us"].get<uint64_t>();
			Require(sent == ping_sent && sent != 0, "unsolicited/replayed Pong");
			Require(
			    session.clock.Sample(sent, NowUs(), j["server_receive_us"], j["server_send_us"]),
			    "invalid clock map");
			last_pong = NowUs();
			ping_sent = 0;
			if (!session.clock.Fresh(last_pong)) next_ping = last_pong + 100000;
			ready();
			return true;
		}
		if (h.type == Type::State) {
			auto j = Json::parse(frame.payload.begin(), frame.payload.end(), nullptr, false);
			Require(!j.is_discarded() && j.is_object() && j.contains("state") &&
			            j["state"].is_string(),
			        "invalid State");
			if (j["state"] == "preparing") {
				Require(!pending, "duplicate preparing State");
				pending       = frame;
				pending_since = NowUs();
				ready();
				return true;
			}
			pending.reset();
		}
		if (h.type == Type::Ping) {
			// The receiver only originates Ping; accepting requests would permit reflection.
			throw std::runtime_error("message direction violation");
		}
		if (!session.Handle(frame, NowUs())) throw std::runtime_error(session.Error());
		return true;
	};
	std::array<uint8_t, 4096> buffer {};
	uint64_t                  partial_since = 0;
	uint64_t                  next_status   = NowUs() + 2000000;
	while (Running()) {
		uint64_t now = NowUs();
		if (!session.Tick(now)) throw std::runtime_error(session.Error());
		Require(now - last_pong <= 6000000 && now - last_read <= 10000000,
		        "server read/Pong deadline");
		if (pending) Require(now - pending_since <= 5000000, "readiness clock deadline");
		if (partial_since) Require(now - partial_since <= 2000000, "partial frame deadline");
		if (now >= next_ping && ping_sent == 0) {
			ping_sent = now;
			Send(ssl.get(), socket.value,
			     Control(Type::Ping, 0, 0, sequence++, {{"client_send_us", now}}));
			next_ping = now + 2000000;
		}
		if (session.Ready() && now >= next_status) {
			Send(ssl.get(), socket.value,
			     Control(Type::Status, session.Generation(), session.Epoch(), sequence++,
			             {{"state", "active"},
			              {"queued_us", session.QueuedUs()},
			              {"bytes", session.Bytes()}}));
			next_status = now + 2000000;
		}
		size_t count  = 0;
		int    result = SSL_read_ex(ssl.get(), buffer.data(), buffer.size(), &count);
		if (result == 1) {
			last_read = NowUs();
			Require(framer.Feed({buffer.data(), count}, consume),
			        "invalid or rejected incoming frame");
			if (framer.Buffered()) {
				if (!partial_since) partial_since = NowUs();
			} else
				partial_since = 0;
			continue;
		}
		int error = SSL_get_error(ssl.get(), result);
		Require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
		        "TLS peer disconnected");
		// Short polling services rumble expiry/device loss even when no PCM arrives.
		pollfd fd {socket.value, error == SSL_ERROR_WANT_READ ? short(POLLIN) : short(POLLOUT), 0};
		int    polled = ::poll(&fd, 1, 10);
		Require(polled >= 0 || errno == EINTR, "TLS read poll failed");
	}
}
} // namespace
void GenerateIdentity(const std::string& directory) {
	namespace fs = std::filesystem;
	Require(!fs::exists(directory), "identity directory already exists; refusing replacement");
	Require(::mkdir(directory.c_str(), 0700) == 0, "cannot create identity directory");
	Private(directory, true);
	auto* keyctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
	Require(keyctx, "identity key context failed");
	EVP_PKEY* key = nullptr;
	bool      ok  = EVP_PKEY_keygen_init(keyctx) == 1 &&
	                EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyctx, NID_X9_62_prime256v1) == 1 &&
	                EVP_PKEY_keygen(keyctx, &key) == 1;
	EVP_PKEY_CTX_free(keyctx);
	Require(ok, "identity key generation failed");
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> owned_key(key, EVP_PKEY_free);
	std::unique_ptr<X509, decltype(&X509_free)>         cert(X509_new(), X509_free);
	Require(bool(cert), "certificate allocation failed");
	Require(X509_set_version(cert.get(), 2) == 1, "certificate version failed");
	std::array<unsigned char, 16> serial {};
	Require(RAND_bytes(serial.data(), serial.size()) == 1, "certificate randomness failed");
	serial[0] &= 0x7f;
	auto* bn  = BN_bin2bn(serial.data(), serial.size(), nullptr);
	auto* asn = bn ? BN_to_ASN1_INTEGER(bn, nullptr) : nullptr;
	BN_free(bn);
	Require(asn, "certificate serial failed");
	ok = X509_set_serialNumber(cert.get(), asn) == 1;
	ASN1_INTEGER_free(asn);
	Require(ok, "certificate serial failed");
	X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
	X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 3600);
	Require(X509_set_pubkey(cert.get(), key) == 1, "certificate public key failed");
	auto* name = X509_get_subject_name(cert.get());
	Require(X509_NAME_add_entry_by_txt(
	            name, "CN", MBSTRING_ASC,
	            reinterpret_cast<const unsigned char*>("Kyty controller audio receiver"), -1, -1,
	            0) == 1 &&
	            X509_set_issuer_name(cert.get(), name) == 1,
	        "certificate subject failed");
	X509V3_CTX extctx;
	X509V3_set_ctx(&extctx, cert.get(), cert.get(), nullptr, nullptr, 0);
	for (auto extension:
	     std::array<std::pair<int, const char*>, 3> {{{NID_basic_constraints, "critical,CA:FALSE"},
	                                                  {NID_key_usage, "critical,digitalSignature"},
	                                                  {NID_ext_key_usage, "clientAuth"}}}) {
		auto* ext = X509V3_EXT_conf_nid(nullptr, &extctx, extension.first, extension.second);
		Require(ext, "certificate extension failed");
		ok = X509_add_ext(cert.get(), ext, -1) == 1;
		X509_EXTENSION_free(ext);
		Require(ok, "certificate extension failed");
	}
	Require(X509_sign(cert.get(), key, EVP_sha256()) > 0, "certificate signing failed");
	auto save = [&](const char* filename, bool is_key) {
		std::string path = directory + "/" + filename;
		int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		Require(fd >= 0, "cannot create private identity file");
		FILE* file = fdopen(fd, "w");
		if (!file) {
			::close(fd);
			throw std::runtime_error("identity file stream failed");
		}
		bool saved =
		    is_key ? PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1
		           : PEM_write_X509(file, cert.get()) == 1;
		bool flushed = fflush(file) == 0 && fsync(fd) == 0;
		int  closed  = fclose(file);
		Require(saved && flushed && closed == 0, "identity file write failed");
	};
	save("key.pem", true);
	save("cert.pem", false);
	std::array<unsigned char, 32> digest {};
	unsigned                      n = 0;
	Require(X509_digest(cert.get(), EVP_sha256(), digest.data(), &n) == 1, "fingerprint failed");
	std::printf("receiver certificate: %s/cert.pem\ncertificate_sha256: %s\n", directory.c_str(),
	            Hex(digest).c_str());
}
Config ReadConfig(const std::string& path) {
	Private(path);
	std::ifstream in(path, std::ios::binary);
	std::string   text(16385, '\0');
	in.read(text.data(), text.size());
	text.resize(in.gcount());
	Require(text.size() <= 16384, "config too large");
	auto j = Json::parse(text, nullptr, false);
	Require(!j.is_discarded() && j.is_object(), "invalid config JSON");
	Config c;
	auto   get = [&](const char* key, bool required = false) {
		Require(!required || j.contains(key), "missing config field");
		if (!j.contains(key)) return std::string {};
		Require(j[key].is_string(), "invalid string config field");
		auto v = j[key].get<std::string>();
		Require(!v.empty() && v.size() <= 4096 && v.find('\0') == std::string::npos,
		        "invalid string config bound");
		return v;
	};
	c.host               = get("host", true);
	c.pinned_host_sha256 = get("pinned_host_sha256", true);
	c.cert               = get("cert", true);
	c.key                = get("key", true);
	c.receiver_id        = get("receiver_id");
	{
		FILE* file = fopen(c.cert.c_str(), "r");
		Require(file, "receiver certificate unavailable");
		auto* certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
		fclose(file);
		Require(certificate, "receiver certificate invalid");
		std::array<unsigned char, 32> digest {};
		unsigned                      count = 0;
		bool ok = X509_digest(certificate, EVP_sha256(), digest.data(), &count) == 1 && count == 32;
		X509_free(certificate);
		Require(ok, "receiver fingerprint failed");
		Require(c.receiver_id.empty() || c.receiver_id == Hex(digest),
		        "receiver_id must equal certificate fingerprint");
		c.receiver_id = Hex(digest);
	}
	if (j.contains("sink")) c.sink = get("sink");
	c.usb_path   = get("usb_path");
	c.audio_name = get("audio_name");
	if (j.contains("port")) {
		Require(j["port"].is_number_unsigned() && j["port"].get<uint64_t>() > 0 &&
		            j["port"].get<uint64_t>() <= 65535,
		        "invalid port");
		c.port = j["port"].get<uint16_t>();
	}
	Require(c.receiver_id.size() == 64 && (c.sink == "null" || c.sink == "usb") &&
	            c.pinned_host_sha256.size() == 64 &&
	            std::all_of(
	                c.pinned_host_sha256.begin(), c.pinned_host_sha256.end(),
	                [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }),
	        "invalid receiver/pin/sink config");
	Private(c.cert);
	Private(c.key);
	Private(std::filesystem::path(c.key).parent_path(), true);
	return c;
}
int Run(const Config& config) {
	stopped = 0;
	stop_at = config.run_seconds ? NowUs() + uint64_t(config.run_seconds) * 1000000 : UINT64_MAX;
	std::signal(SIGINT, Stop);
	std::signal(SIGTERM, Stop);
	std::signal(SIGPIPE, SIG_IGN);
	std::unique_ptr<Sink> sink;
	NullSink*             null = nullptr;
	if (config.sink == "null") {
		auto owned = std::make_unique<NullSink>(config.raw_path, config.diagnostics);
		null       = owned.get();
		sink       = std::move(owned);
	} else
		sink = std::make_unique<UsbSink>(MakeSdlUsbOutput(config));
	Session  session(*sink);
	uint32_t backoff    = 250;
	uint64_t last_error = 0;
	while (Running()) {
		try {
			Connection(config, session);
			backoff = 250;
		} catch (const std::exception& error) {
			sink->Reset();
			if (NowUs() - last_error > 2000000) {
				std::fprintf(stderr, "receiver waiting: %.128s\n", error.what());
				last_error = NowUs();
			}
		}
		uint64_t retry = NowUs() + uint64_t(backoff) * 1000;
		while (Running() && NowUs() < retry)
			::poll(nullptr, 0, 20);
		backoff = std::min<uint32_t>(backoff * 2, 5000);
	}
	sink->Reset();
	if (config.diagnostics && null)
		for (auto kind: {Kind::Actuator, Kind::Speaker})
			std::printf(
			    "%s bytes=%llu sha256=%s\n", kind == Kind::Actuator ? "actuator" : "speaker",
			    static_cast<unsigned long long>(null->Bytes(kind)), null->Hash(kind).c_str());
	return 0;
}
} // namespace ControllerAudio::Receiver
