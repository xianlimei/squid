/*
 * Copyright (C) 1996-2015 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 33    Client-side Routines */

#ifndef SQUID_CLIENTSIDE_H
#define SQUID_CLIENTSIDE_H

#include "base/RunnersRegistry.h"
#include "clientStreamForward.h"
#include "comm.h"
#include "helper/forward.h"
#include "http/forward.h"
#include "HttpControlMsg.h"
#include "ipc/FdNotes.h"
#include "SBuf.h"
#include "servers/Server.h"
#if USE_AUTH
#include "auth/UserRequest.h"
#endif
#if USE_OPENSSL
#include "ssl/support.h"
#endif

class ConnStateData;
class ClientHttpRequest;
class clientStreamNode;
namespace AnyP
{
class PortCfg;
} // namespace Anyp

/**
 * Badly named.
 * This is in fact the processing context for a single HTTP transaction.
 *
 * A context lifetime extends from directly after a request has been parsed
 * off the client connection buffer, until the last byte of both request
 * and reply payload (if any) have been written.
 *
 * (NOTE: it is not certain yet if an early reply to a POST/PUT is sent by
 * the server whether the context will remain in the pipeline until its
 * request payload has finished being read. It is supposed to, but may not)
 *
 * Contexts self-register with the Pipeline being managed by the Server
 * for the connection on which the request was received.
 *
 * When HTTP/1 pipeline is operating there may be multiple transactions using
 * the clientConnection. Only the back() context may read from the connection,
 * and only the front() context may write to it. A context which needs to read
 * or write to the connection but does not meet those criteria must be shifted
 * to the deferred state.
 *
 * When a context is completed the finished() method needs to be called which
 * will perform all cleanup and deregistration operations. If the reason for
 * finishing is an error, then notifyIoError() needs to be called prior to
 * the finished() method.
 * The caller should follow finished() with a call to ConnStateData::kick()
 * to resume processing of other transactions or I/O on the connection.
 *
 * Alternatively the initiateClose() method can be called to terminate the
 * whole client connection and all other pending contexts.
 *
 * The socket level management is done by a Server which owns us.
 * The scope of this objects control over a socket consists of the data
 * buffer received from the Server with an initially unknown length.
 * When that length is known it sets the end boundary of our access to the
 * buffer.
 *
 * The individual processing actions are done by other Jobs which we
 * kick off as needed.
 *
 * XXX: If an async call ends the ClientHttpRequest job, ClientSocketContext
 * (and ConnStateData) may not know about it, leading to segfaults and
 * assertions. This is difficult to fix
 * because ClientHttpRequest lacks a good way to communicate its ongoing
 * destruction back to the ClientSocketContext which pretends to "own" *http.
 */
class ClientSocketContext : public RefCountable
{
    MEMPROXY_CLASS(ClientSocketContext);

public:
    typedef RefCount<ClientSocketContext> Pointer;
    ClientSocketContext(const Comm::ConnectionPointer &aConn, ClientHttpRequest *aReq);
    ~ClientSocketContext();
    bool startOfOutput() const;
    void writeComplete(size_t size);

    Comm::ConnectionPointer clientConnection; /// details about the client connection socket.
    ClientHttpRequest *http;    /* we pretend to own that job */
    HttpReply *reply;
    char reqbuf[HTTP_REQBUF_SZ];

    struct {

        unsigned deferred:1; /* This is a pipelined request waiting for the current object to complete */

        unsigned parsed_ok:1; /* Was this parsed correctly? */
    } flags;
    bool mayUseConnection() const {return mayUseConnection_;}

    void mayUseConnection(bool aBool) {
        mayUseConnection_ = aBool;
        debugs(33,3, HERE << "This " << this << " marked " << aBool);
    }

    class DeferredParams
    {

    public:
        clientStreamNode *node;
        HttpReply *rep;
        StoreIOBuffer queuedBuffer;
    };

    DeferredParams deferredparams;
    int64_t writtenToSocket;
    void pullData();
    int64_t getNextRangeOffset() const;
    bool canPackMoreRanges() const;
    clientStream_status_t socketState();
    void sendBody(HttpReply * rep, StoreIOBuffer bodyData);
    void sendStartOfMessage(HttpReply * rep, StoreIOBuffer bodyData);
    size_t lengthToSend(Range<int64_t> const &available);
    void noteSentBodyBytes(size_t);
    void buildRangeHeader(HttpReply * rep);
    clientStreamNode * getTail() const;
    clientStreamNode * getClientReplyContext() const;
    ConnStateData *getConn() const;
    void finished(); ///< cleanup when the transaction has finished. may destroy 'this'
    void deferRecipientForLater(clientStreamNode * node, HttpReply * rep, StoreIOBuffer receivedData);
    bool multipartRangeRequest() const;
    void registerWithConn();
    void noteIoError(const int xerrno); ///< update state to reflect I/O error
    void initiateClose(const char *reason); ///< terminate due to a send/write error (may continue reading)

private:
    void prepareReply(HttpReply * rep);
    void packChunk(const StoreIOBuffer &bodyData, MemBuf &mb);
    void packRange(StoreIOBuffer const &, MemBuf * mb);
    void doClose();

    bool mayUseConnection_; /* This request may use the connection. Don't read anymore requests for now */
    bool connRegistered_;
};

class ConnectionDetail;
#if USE_OPENSSL
namespace Ssl
{
class ServerBump;
}
#endif

/**
 * Legacy Server code managing a connection to a client.
 *
 * NP: presents AsyncJob API but does not operate autonomously as a Job.
 *     So Must() is not safe to use.
 *
 * Multiple requests (up to pipeline_prefetch) can be pipelined.
 * This object is responsible for managing which one is currently being
 * fulfilled and what happens to the queue if the current one causes the client
 * connection to be closed early.
 *
 * Act as a manager for the client connection and passes data in buffer to a
 * Parser relevant to the state (message headers vs body) that is being
 * processed.
 *
 * Performs HTTP message processing to kick off the actual HTTP request
 * handling objects (ClientSocketContext, ClientHttpRequest, HttpRequest).
 *
 * Performs SSL-Bump processing for switching between HTTP and HTTPS protocols.
 *
 * To terminate a ConnStateData close() the client Comm::Connection it is
 * managing, or for graceful half-close use the stopReceiving() or
 * stopSending() methods.
 */
class ConnStateData : public Server, public HttpControlMsgSink, public RegisteredRunner
{

public:
    explicit ConnStateData(const MasterXaction::Pointer &xact);
    virtual ~ConnStateData();

    /* ::Server API */
    virtual void receivedFirstByte();
    virtual bool handleReadData();
    virtual void afterClientRead();
    virtual void afterClientWrite(size_t);

    /* HttpControlMsgSink API */
    virtual void sendControlMsg(HttpControlMsg);

    /// Traffic parsing
    bool clientParseRequests();
    void readNextRequest();

    /// try to make progress on a transaction or read more I/O
    void kick();

    bool isOpen() const;

    Http1::TeChunkedParser *bodyParser; ///< parses HTTP/1.1 chunked request body

    /** number of body bytes we need to comm_read for the "current" request
     *
     * \retval 0         We do not need to read any [more] body bytes
     * \retval negative  May need more but do not know how many; could be zero!
     * \retval positive  Need to read exactly that many more body bytes
     */
    int64_t mayNeedToReadMoreBody() const;

#if USE_AUTH
    /**
     * Fetch the user details for connection based authentication
     * NOTE: this is ONLY connection based because NTLM and Negotiate is against HTTP spec.
     */
    const Auth::UserRequest::Pointer &getAuth() const { return auth_; }

    /**
     * Set the user details for connection-based authentication to use from now until connection closure.
     *
     * Any change to existing credentials shows that something invalid has happened. Such as:
     * - NTLM/Negotiate auth was violated by the per-request headers missing a revalidation token
     * - NTLM/Negotiate auth was violated by the per-request headers being for another user
     * - SSL-Bump CONNECT tunnel with persistent credentials has ended
     */
    void setAuth(const Auth::UserRequest::Pointer &aur, const char *cause);
#endif

    Ip::Address log_addr;

    struct {
        bool readMore; ///< needs comm_read (for this request or new requests)
        bool swanSang; // XXX: temporary flag to check proper cleanup
    } flags;
    struct {
        Comm::ConnectionPointer serverConnection; /* pinned server side connection */
        char *host;             /* host name of pinned connection */
        int port;               /* port of pinned connection */
        bool pinned;             /* this connection was pinned */
        bool auth;               /* pinned for www authentication */
        bool reading;   ///< we are monitoring for peer connection closure
        bool zeroReply; ///< server closed w/o response (ERR_ZERO_SIZE_OBJECT)
        CachePeer *peer;             /* CachePeer the connection goes via */
        AsyncCall::Pointer readHandler; ///< detects serverConnection closure
        AsyncCall::Pointer closeHandler; /*The close handler for pinned server side connection*/
    } pinning;

    bool transparent() const;

    /// true if we stopped receiving the request
    const char *stoppedReceiving() const { return stoppedReceiving_; }
    /// true if we stopped sending the response
    const char *stoppedSending() const { return stoppedSending_; }
    /// note request receiving error and close as soon as we write the response
    void stopReceiving(const char *error);
    /// note response sending error and close as soon as we read the request
    void stopSending(const char *error);

    void expectNoForwarding(); ///< cleans up virgin request [body] forwarding state

    /* BodyPipe API */
    BodyPipe::Pointer expectRequestBody(int64_t size);
    virtual void noteMoreBodySpaceAvailable(BodyPipe::Pointer) = 0;
    virtual void noteBodyConsumerAborted(BodyPipe::Pointer) = 0;

    bool handleRequestBodyData();

    /// Forward future client requests using the given server connection.
    /// Optionally, monitor pinned server connection for remote-end closures.
    void pinConnection(const Comm::ConnectionPointer &pinServerConn, HttpRequest *request, CachePeer *peer, bool auth, bool monitor = true);
    /// Undo pinConnection() and, optionally, close the pinned connection.
    void unpinConnection(const bool andClose);
    /// Returns validated pinnned server connection (and stops its monitoring).
    Comm::ConnectionPointer borrowPinnedConnection(HttpRequest *request, const CachePeer *aPeer);
    /**
     * Checks if there is pinning info if it is valid. It can close the server side connection
     * if pinned info is not valid.
     \param request   if it is not NULL also checks if the pinning info refers to the request client side HttpRequest
     \param CachePeer      if it is not NULL also check if the CachePeer is the pinning CachePeer
     \return          The details of the server side connection (may be closed if failures were present).
     */
    const Comm::ConnectionPointer validatePinnedConnection(HttpRequest *request, const CachePeer *peer);
    /**
     * returts the pinned CachePeer if exists, NULL otherwise
     */
    CachePeer *pinnedPeer() const {return pinning.peer;}
    bool pinnedAuth() const {return pinning.auth;}

    /// called just before a FwdState-dispatched job starts using connection
    virtual void notePeerConnection(Comm::ConnectionPointer) {}

    // pining related comm callbacks
    virtual void clientPinnedConnectionClosed(const CommCloseCbParams &io);

    // comm callbacks
    void clientReadFtpData(const CommIoCbParams &io);
    void connStateClosed(const CommCloseCbParams &io);
    void requestTimeout(const CommTimeoutCbParams &params);

    // AsyncJob API
    virtual void start();
    virtual bool doneAll() const { return BodyProducer::doneAll() && false;}
    virtual void swanSong();

    /// Changes state so that we close the connection and quit after serving
    /// the client-side-detected error response instead of getting stuck.
    void quitAfterError(HttpRequest *request); // meant to be private

    /// The caller assumes responsibility for connection closure detection.
    void stopPinnedConnectionMonitoring();

#if USE_OPENSSL
    /// the second part of old httpsAccept, waiting for future HttpsServer home
    void postHttpsAccept();

    /// Initializes and starts a peek-and-splice negotiation with the SSL client
    void startPeekAndSplice();
    /// Called when the initialization of peek-and-splice negotiation finidhed
    void startPeekAndSpliceDone();
    /// Called when a peek-and-splice step finished. For example after
    /// server SSL certificates received and fake server SSL certificates
    /// generated
    void doPeekAndSpliceStep();
    /// called by FwdState when it is done bumping the server
    void httpsPeeked(Comm::ConnectionPointer serverConnection);

    /// Splice a bumped client connection on peek-and-splice mode
    void splice();

    /// Check on_unsupported_protocol access list and splice if required
    /// \retval true on splice
    /// \retval false otherwise
    bool spliceOnError(const err_type err);

    /// Start to create dynamic Security::ContextPtr for host or uses static port SSL context.
    void getSslContextStart();
    /**
     * Done create dynamic ssl certificate.
     *
     * \param[in] isNew if generated certificate is new, so we need to add this certificate to storage.
     */
    void getSslContextDone(Security::ContextPtr sslContext, bool isNew = false);
    /// Callback function. It is called when squid receive message from ssl_crtd.
    static void sslCrtdHandleReplyWrapper(void *data, const Helper::Reply &reply);
    /// Proccess response from ssl_crtd.
    void sslCrtdHandleReply(const Helper::Reply &reply);

    void switchToHttps(HttpRequest *request, Ssl::BumpMode bumpServerMode);
    bool switchedToHttps() const { return switchedToHttps_; }
    Ssl::ServerBump *serverBump() {return sslServerBump;}
    inline void setServerBump(Ssl::ServerBump *srvBump) {
        if (!sslServerBump)
            sslServerBump = srvBump;
        else
            assert(sslServerBump == srvBump);
    }
    const SBuf &sslCommonName() const {return sslCommonName_;}
    void resetSslCommonName(const char *name) {sslCommonName_ = name;}
    /// Fill the certAdaptParams with the required data for certificate adaptation
    /// and create the key for storing/retrieve the certificate to/from the cache
    void buildSslCertGenerationParams(Ssl::CertificateProperties &certProperties);
    /// Called when the client sends the first request on a bumped connection.
    /// Returns false if no [delayed] error should be written to the client.
    /// Otherwise, writes the error to the client and returns true. Also checks
    /// for SQUID_X509_V_ERR_DOMAIN_MISMATCH on bumped requests.
    bool serveDelayedError(ClientSocketContext *context);

    Ssl::BumpMode sslBumpMode; ///< ssl_bump decision (Ssl::bumpEnd if n/a).

#else
    bool switchedToHttps() const { return false; }
#endif

    /* clt_conn_tag=tag annotation access */
    const SBuf &connectionTag() const { return connectionTag_; }
    void connectionTag(const char *aTag) { connectionTag_ = aTag; }

    /// handle a control message received by context from a peer and call back
    virtual void writeControlMsgAndCall(HttpReply *rep, AsyncCall::Pointer &call) = 0;

    /// ClientStream calls this to supply response header (once) and data
    /// for the current ClientSocketContext.
    virtual void handleReply(HttpReply *header, StoreIOBuffer receivedData) = 0;

    /// remove no longer needed leading bytes from the input buffer
    void consumeInput(const size_t byteCount);

    /* TODO: Make the methods below (at least) non-public when possible. */

    /// stop parsing the request and create context for relaying error info
    ClientSocketContext *abortRequestParsing(const char *const errUri);

    /// generate a fake CONNECT request with the given payload
    /// at the beginning of the client I/O buffer
    void fakeAConnectRequest(const char *reason, const SBuf &payload);

    /// client data which may need to forward as-is to server after an
    /// on_unsupported_protocol tunnel decision.
    SBuf preservedClientData;

    /* Registered Runner API */
    virtual void startShutdown();
    virtual void endingShutdown();

protected:
    void startDechunkingRequest();
    void finishDechunkingRequest(bool withSuccess);
    void abortChunkedRequestBody(const err_type error);
    err_type handleChunkedRequestBody();

    void startPinnedConnectionMonitoring();
    void clientPinnedConnectionRead(const CommIoCbParams &io);
#if USE_OPENSSL
    /// Handles a ready-for-reading TLS squid-to-server connection that
    /// we thought was idle.
    /// \return false if and only if the connection should be closed.
    bool handleIdleClientPinnedTlsRead();
#endif

    /// parse input buffer prefix into a single transfer protocol request
    /// return NULL to request more header bytes (after checking any limits)
    /// use abortRequestParsing() to handle parsing errors w/o creating request
    virtual ClientSocketContext *parseOneRequest() = 0;

    /// start processing a freshly parsed request
    virtual void processParsedRequest(ClientSocketContext *context) = 0;

    /// returning N allows a pipeline of 1+N requests (see pipeline_prefetch)
    virtual int pipelinePrefetchMax() const;

    /// timeout to use when waiting for the next request
    virtual time_t idleTimeout() const = 0;

    BodyPipe::Pointer bodyPipe; ///< set when we are reading request body

private:
    /* ::Server API */
    virtual bool connFinishedWithConn(int size);

    void clientAfterReadingRequests();
    bool concurrentRequestQueueFilled() const;

    void pinNewConnection(const Comm::ConnectionPointer &pinServer, HttpRequest *request, CachePeer *aPeer, bool auth);

    /* PROXY protocol functionality */
    bool proxyProtocolValidateClient();
    bool parseProxyProtocolHeader();
    bool parseProxy1p0();
    bool parseProxy2p0();
    bool proxyProtocolError(const char *reason);

    /// whether PROXY protocol header is still expected
    bool needProxyProtocolHeader_;

#if USE_AUTH
    /// some user details that can be used to perform authentication on this connection
    Auth::UserRequest::Pointer auth_;
#endif

    /// the parser state for current HTTP/1.x input buffer processing
    Http1::RequestParserPointer parser_;

#if USE_OPENSSL
    bool switchedToHttps_;
    /// The SSL server host name appears in CONNECT request or the server ip address for the intercepted requests
    String sslConnectHostOrIp; ///< The SSL server host name as passed in the CONNECT request
    SBuf sslCommonName_; ///< CN name for SSL certificate generation
    String sslBumpCertKey; ///< Key to use to store/retrieve generated certificate

    /// HTTPS server cert. fetching state for bump-ssl-server-first
    Ssl::ServerBump *sslServerBump;
    Ssl::CertSignAlgorithm signAlgorithm; ///< The signing algorithm to use
#endif

    /// the reason why we no longer write the response or nil
    const char *stoppedSending_;
    /// the reason why we no longer read the request or nil
    const char *stoppedReceiving_;

    SBuf connectionTag_; ///< clt_conn_tag=Tag annotation for client connection
};

void setLogUri(ClientHttpRequest * http, char const *uri, bool cleanUrl = false);

const char *findTrailingHTTPVersion(const char *uriAndHTTPVersion, const char *end = NULL);

int varyEvaluateMatch(StoreEntry * entry, HttpRequest * req);

/// accept requests to a given port and inform subCall about them
void clientStartListeningOn(AnyP::PortCfgPointer &port, const RefCount< CommCbFunPtrCallT<CommAcceptCbPtrFun> > &subCall, const Ipc::FdNoteId noteId);

void clientOpenListenSockets(void);
void clientConnectionsClose(void);
void httpRequestFree(void *);

/// decide whether to expect multiple requests on the corresponding connection
void clientSetKeepaliveFlag(ClientHttpRequest *http);

/* misplaced declaratrions of Stream callbacks provided/used by client side */
SQUIDCEXTERN CSR clientGetMoreData;
SQUIDCEXTERN CSS clientReplyStatus;
SQUIDCEXTERN CSD clientReplyDetach;
CSCB clientSocketRecipient;
CSD clientSocketDetach;

/* TODO: Move to HttpServer. Warning: Move requires large code nonchanges! */
ClientSocketContext *parseHttpRequest(ConnStateData *, const Http1::RequestParserPointer &);
void clientProcessRequest(ConnStateData *, const Http1::RequestParserPointer &, ClientSocketContext *);
void clientPostHttpsAccept(ConnStateData *);

#endif /* SQUID_CLIENTSIDE_H */

