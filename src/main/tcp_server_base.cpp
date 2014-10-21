#include "../precompiled.hpp"
#include "tcp_server_base.hpp"
#include "tcp_session_base.hpp"
#define POSEIDON_TCP_SESSION_SSL_IMPL_
#include "tcp_session_ssl_impl.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include "singletons/epoll_daemon.hpp"
#include "log.hpp"
#include "exception.hpp"
#include "endian.hpp"
#include "tcp_session_base.hpp"
using namespace Poseidon;

class TcpServerBase::SslImplServer : boost::noncopyable {
private:
	SslCtxPtr m_sslCtx;

public:
	SslImplServer(const std::string &cert, const std::string &privateKey){
		requireSsl();

		m_sslCtx.reset(::SSL_CTX_new(::TLSv1_server_method()));
		if(!m_sslCtx){
			LOG_FATAL("Could not create server SSL context");
			std::abort();
		}
		::SSL_CTX_set_verify(m_sslCtx.get(), SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE, VAL_INIT);

		LOG_INFO("Loading server certificate: ", cert);
		if(::SSL_CTX_use_certificate_file(
			m_sslCtx.get(), cert.c_str(), SSL_FILETYPE_PEM) != 1)
		{
			DEBUG_THROW(Exception, "::SSL_CTX_use_certificate_file() failed");
		}

		LOG_INFO("Loading server private key: ", privateKey);
		if(::SSL_CTX_use_PrivateKey_file(
			m_sslCtx.get(), privateKey.c_str(), SSL_FILETYPE_PEM) != 1)
		{
			DEBUG_THROW(Exception, "::SSL_CTX_use_PrivateKey_file() failed");
		}

		LOG_INFO("Verifying private key...");
		if(::SSL_CTX_check_private_key(m_sslCtx.get()) != 1){
			DEBUG_THROW(Exception, "::SSL_CTX_check_private_key() failed");
		}
	}

public:
	void createSsl(SslPtr &ssl){
		ssl.reset(::SSL_new(m_sslCtx.get()));
	}
};

class TcpServerBase::SslImplClient : public TcpSessionBase::SslImpl {
public:
	SslImplClient(Move<SslPtr> ssl, int fd)
		: SslImpl(STD_MOVE(ssl), fd)
	{
	}

protected:
	bool establishConnection(){
		const int ret = ::SSL_accept(m_ssl.get());
		if(ret == 1){
			return true;
		}
		const int err = ::SSL_get_error(m_ssl.get(), ret);
		if((err == SSL_ERROR_WANT_READ) || (err == SSL_ERROR_WANT_WRITE)){
			return false;
		}
		LOG_ERROR("::SSL_accept() = ", ret, ", ::SSL_get_error() = ", err);
		DEBUG_THROW(Exception, "::SSL_accept() failed");
	}
};

TcpServerBase::TcpServerBase(const std::string &bindAddr, unsigned bindPort,
	const std::string &cert, const std::string &privateKey)
{
	char port[16];
	const unsigned len = std::sprintf(port, ":%hu", bindPort);
	m_bindAddr.reserve(bindAddr.size() + len);
	m_bindAddr.assign(bindAddr);
	m_bindAddr.append(port, len);

	LOG_INFO("Creating ", (cert.empty() ? "" : "SSL "), "socket server on ", m_bindAddr, "...");

	union {
		::sockaddr sa;
		::sockaddr_in sin;
		::sockaddr_in6 sin6;
	} u;
	::socklen_t salen = sizeof(u);

	if(::inet_pton(AF_INET, bindAddr.c_str(), &u.sin.sin_addr) == 1){
		u.sin.sin_family = AF_INET;
		storeBe(u.sin.sin_port, bindPort);
		salen = sizeof(::sockaddr_in);
	} else if(::inet_pton(AF_INET6, bindAddr.c_str(), &u.sin6.sin6_addr) == 1){
		u.sin6.sin6_family = AF_INET6;
		storeBe(u.sin6.sin6_port, bindPort);
		salen = sizeof(::sockaddr_in6);
	} else {
		LOG_ERROR("Unknown address format: ", bindAddr);
		DEBUG_THROW(Exception, "Unknown address format");
	}

	m_listen.reset(::socket(u.sa.sa_family, SOCK_STREAM, IPPROTO_TCP));
	if(!m_listen){
		DEBUG_THROW(SystemError, errno);
	}
	const int TRUE_VALUE = true;
	if(::setsockopt(m_listen.get(),
		SOL_SOCKET, SO_REUSEADDR, &TRUE_VALUE, sizeof(TRUE_VALUE)) != 0)
	{
		DEBUG_THROW(SystemError, errno);
	}
	const int flags = ::fcntl(m_listen.get(), F_GETFL);
	if(flags == -1){
		DEBUG_THROW(SystemError, errno);
	}
	if(::fcntl(m_listen.get(), F_SETFL, flags | O_NONBLOCK) != 0){
		DEBUG_THROW(SystemError, errno);
	}
	if(::bind(m_listen.get(), &u.sa, salen)){
		DEBUG_THROW(SystemError, errno);
	}
	if(::listen(m_listen.get(), SOMAXCONN)){
		DEBUG_THROW(SystemError, errno);
	}

	if(!cert.empty()){
		m_sslImplServer.reset(new SslImplServer(cert, privateKey));
	}
}
TcpServerBase::~TcpServerBase(){
	LOG_INFO("Destroyed socket server on ", m_bindAddr);
}

boost::shared_ptr<TcpSessionBase> TcpServerBase::tryAccept() const {
	ScopedFile client(::accept(m_listen.get(), VAL_INIT, VAL_INIT));
	if(!client){
		if(errno != EAGAIN){
			DEBUG_THROW(SystemError, errno);
		}
		return VAL_INIT;
	}
	AUTO(session, onClientConnect(STD_MOVE(client)));
	if(!session){
		LOG_WARNING("onClientConnect() returns a null pointer.");
		DEBUG_THROW(Exception, "Null client pointer");
	}
	if(m_sslImplServer){
		LOG_INFO("Waiting for SSL handshake...");

		SslPtr ssl;
		m_sslImplServer->createSsl(ssl);
		boost::scoped_ptr<TcpSessionBase::SslImpl>
			sslImpl(new SslImplClient(STD_MOVE(ssl), session->m_socket.get()));
		session->initSsl(STD_MOVE(sslImpl));
	}
	LOG_INFO("Client ", session->getRemoteIp(), " has connected.");
	return session;
}
