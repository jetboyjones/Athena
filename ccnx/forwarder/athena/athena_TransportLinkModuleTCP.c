/*
 * Copyright (c) 2015, Xerox Corporation (Xerox)and Palo Alto Research Center (PARC)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Patent rights are not granted under this agreement. Patent rights are
 *       available under FRAND terms.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL XEROX or PARC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @author Kevin Fox, Palo Alto Research Center (Xerox PARC)
 * @copyright 2015, Xerox Corporation (Xerox)and Palo Alto Research Center (PARC).  All rights reserved.
 */
#include <config.h>

#include <LongBow/runtime.h>

#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/param.h>
#include <arpa/inet.h>

#include <parc/algol/parc_Network.h>
#include <ccnx/forwarder/athena/athena_TransportLinkModule.h>

#include <ccnx/common/codec/ccnxCodec_TlvPacket.h>
#include <ccnx/common/ccnx_WireFormatMessage.h>

static int _listenerBacklog = 16;

//
// Platform support required for managing SIGPIPE.
//
// MacOS (BSD) provides SO_NOSIGPIPE socket options for returning EPIPE on writes,
// Linux provides MSB_NOSIGNAL send(2) options to allow the same.
// Alternate implementations could interpose a SIGPIPE signal handler.
//
#if defined(SO_NOSIGPIPE) // MacOS, BSD
    #define BSD_IGNORESIGPIPE 1
#elif defined(MSG_NOSIGNAL) // Primarily Linux
    #define LINUX_IGNORESIGPIPE 1
#else
"Platform not supported";
#endif

//
// Private data for each link instance
//
typedef struct _TCPLinkData {
    int fd;
    struct sockaddr_in myAddress;
    socklen_t myAddressLength;
    struct sockaddr_in peerAddress;
    socklen_t peerAddressLength;
    struct {
        size_t receive_ReadHeaderFailure;
        size_t receive_BadMessageLength;
        size_t receive_ReadError;
        size_t receive_ReadWouldBlock;
        size_t receive_ShortRead;
        size_t receive_ShortWrite;
        size_t receive_DecodeFailed;
    } _stats;
} _TCPLinkData;

static _TCPLinkData *
_TCPLinkData_Create()
{
    _TCPLinkData *linkData = parcMemory_AllocateAndClear(sizeof(_TCPLinkData));
    assertNotNull(linkData, "Could not create private data for new link");
    return linkData;
}

static void
_TCPLinkData_Destroy(_TCPLinkData **linkData)
{
    parcMemory_Deallocate(linkData);
}

/**
 * @abstract create link name based on file descriptor
 * @discussion
 *
 * @param [in] type of connection
 * @param [in] fd file descriptor
 * @return allocated name, must be released with parcMemory_Deallocate()
 *
 * Example:
 * @code
 * {
 *
 * }
 * @endcode
 */
static const char *
_createNameFromLinkData(const _TCPLinkData *linkData)
{
    char nameBuffer[MAXPATHLEN];
    const char *protocol = "tcp";

    // Get our local hostname and port
    char myHost[NI_MAXHOST], myPort[NI_MAXSERV];
    int myResult = getnameinfo((struct sockaddr *) &linkData->myAddress, linkData->myAddressLength,
                               myHost, NI_MAXHOST, myPort, NI_MAXSERV, NI_NUMERICSERV);

    // Get our peer's hostname and port
    char peerHost[NI_MAXHOST], peerPort[NI_MAXSERV];
    int peerResult = getnameinfo((struct sockaddr *) &linkData->peerAddress, linkData->peerAddressLength,
                                 peerHost, NI_MAXHOST, peerPort, NI_MAXSERV, NI_NUMERICSERV);

    if ((peerResult == 0) && (myResult == 0)) { // point to point connection
        sprintf(nameBuffer, "%s://%s:%s<->%s:%s", protocol, myHost, myPort, peerHost, peerPort);
    } else if (myResult == 0) { // listener only
        sprintf(nameBuffer, "%s://%s:%s", protocol, myHost, myPort);
    } else { // some unknown possibility
        sprintf(nameBuffer, "%s://Unknown", protocol);
    }

    return parcMemory_StringDuplicate(nameBuffer, strlen(nameBuffer));
}

static int
_TCPSend(AthenaTransportLink *athenaTransportLink, CCNxMetaMessage *ccnxMetaMessage)
{
    struct _TCPLinkData *linkData = athenaTransportLink_GetPrivateData(athenaTransportLink);

    if (ccnxTlvDictionary_GetSchemaVersion(ccnxMetaMessage) == CCNxTlvDictionary_SchemaVersion_V0) {
        parcLog_Warning(athenaTransportLink_GetLogger(athenaTransportLink),
                        "sending deprecated version %d message\n", ccnxTlvDictionary_GetSchemaVersion(ccnxMetaMessage));
    }

    // Get wire format and write it out.
    PARCBuffer *wireFormatBuffer = ccnxWireFormatMessage_GetWireFormatBuffer(ccnxMetaMessage);

    if (wireFormatBuffer == NULL) {
        CCNxCodecNetworkBufferIoVec *iovec = ccnxWireFormatMessage_GetIoVec(ccnxMetaMessage);
        assertNotNull(iovec, "Null io vector");

        size_t iovcnt = ccnxCodecNetworkBufferIoVec_GetCount((CCNxCodecNetworkBufferIoVec *) iovec);
        const struct iovec *array = ccnxCodecNetworkBufferIoVec_GetArray((CCNxCodecNetworkBufferIoVec *) iovec);

        // If it's a single vector wrap it in a buffer to avoid a copy
        if (iovcnt == 1) {
            wireFormatBuffer = parcBuffer_Wrap(array[0].iov_base, array[0].iov_len, 0, array[0].iov_len);
        } else {
            size_t totalbytes = 0;
            for (int i = 0; i < iovcnt; i++) {
                totalbytes += array[i].iov_len;
            }
            wireFormatBuffer = parcBuffer_Allocate(totalbytes);
            for (int i = 0; i < iovcnt; i++) {
                parcBuffer_PutArray(wireFormatBuffer, array[i].iov_len, array[i].iov_base);
            }
            parcBuffer_Flip(wireFormatBuffer);
        }
    } else {
        wireFormatBuffer = parcBuffer_Acquire(wireFormatBuffer);
    }

    size_t length = parcBuffer_Limit(wireFormatBuffer);
    char *buffer = parcBuffer_Overlay(wireFormatBuffer, length);

    parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink),
                  "sending message (size=%d)", length);

    int writeCount = 0;
    // If it was a short read, attempt to read the remainder of the message
    while (writeCount < length) {
#ifdef LINUX_IGNORESIGPIPE
        int count = send(linkData->fd, &buffer[writeCount], length - writeCount, MSG_NOSIGNAL);
#else
        ssize_t count = write(linkData->fd, &buffer[writeCount], length - writeCount);
#endif

        if (count <= 0) { // on error close the link, else return to retry a zero write
            if (count == -1) {
                if (errno == EPIPE) {
                    athenaTransportLink_SetEvent(athenaTransportLink, AthenaTransportLinkEvent_Error);
                }
                parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink),
                              "send error (%s)", strerror(errno));
            } else {
                linkData->_stats.receive_ShortWrite++;
                parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink), "short write");
            }
            parcBuffer_Release(&wireFormatBuffer);
            return -1;
        }
        writeCount += count;
    }

    parcBuffer_Release(&wireFormatBuffer);
    return 0;
}

static bool
_linkIsEOF(AthenaTransportLink *athenaTransportLink)
{
    struct _TCPLinkData *linkData = athenaTransportLink_GetPrivateData(athenaTransportLink);

    // If poll indicates there's a read event and a subsequent read returns zero our peer has hungup.
    struct pollfd pollfd = { .fd = linkData->fd, .events = POLLIN };
    int events = poll(&pollfd, 1, 0);
    if (events == -1) {
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink), "poll error (%s)", strerror(errno));
        return true; // poll error, close the link
    } else if (events == 0) {
        // there are no pending events, was truly a zero read
        return false;
    }
    if (pollfd.revents & POLLIN) {
        char peekBuffer;
        ssize_t readCount = recv(linkData->fd, (void *) &peekBuffer, 1, MSG_PEEK);
        if (readCount == -1) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) { // read blocked
                linkData->_stats.receive_ReadWouldBlock++;
                return false;
            }
            return true; // read error
        }
        if (readCount == 0) { // EOF
            return true;
        }
    }
    return false;
}

static CCNxMetaMessage *
_TCPReceive(AthenaTransportLink *athenaTransportLink)
{
    struct _TCPLinkData *linkData = athenaTransportLink_GetPrivateData(athenaTransportLink);
    CCNxMetaMessage *ccnxMetaMessage = NULL;

    // Peek at our message header to determine the total length of buffer we need to allocate.
    size_t fixedHeaderLength = ccnxCodecTlvPacket_MinimalHeaderLength();
    PARCBuffer *wireFormatBuffer = parcBuffer_Allocate(fixedHeaderLength);
    const uint8_t *peekBuffer = parcBuffer_Overlay(wireFormatBuffer, 0);

    ssize_t readCount = recv(linkData->fd, (void *) peekBuffer, fixedHeaderLength, MSG_PEEK);

    if (readCount == -1) {
        linkData->_stats.receive_ReadError++;
        parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink), "read error (%s)", strerror(errno));
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // A zero read means either no more data is available or our peer has hungup.
    if (readCount == 0) {
        if (_linkIsEOF(athenaTransportLink)) {
            athenaTransportLink_SetEvent(athenaTransportLink, AthenaTransportLinkEvent_Error);
        } else {
            // A zero read, try again later
        }
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // Check for a short header read, since we're only peeking here we just return and retry later
    if (readCount != fixedHeaderLength) {
        linkData->_stats.receive_ReadHeaderFailure++;
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // Obtain the total size of the message from the header
    size_t messageLength = ccnxCodecTlvPacket_GetPacketLength(wireFormatBuffer);

    // Today, if the length is bad we flush the link and return.
    // Could do more to check the integrity of the message and framing.
    if (messageLength < fixedHeaderLength) {
        linkData->_stats.receive_BadMessageLength++;
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink), "Framing error, flushing link.");
        char trash[MAXPATHLEN];
        // Flush link to attempt to resync our framing
        while (read(linkData->fd, trash, sizeof(trash)) == sizeof(trash)) {
            parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink), "... flushing link.");
        }
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // Allocate the remainder of message buffer and read a message into it.
    wireFormatBuffer = parcBuffer_Resize(wireFormatBuffer, messageLength);
    char *buffer = parcBuffer_Overlay(wireFormatBuffer, 0);
    readCount = read(linkData->fd, buffer, messageLength);

    // On error, just return and retry.
    if (readCount == -1) {
        linkData->_stats.receive_ReadError++;
        parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink), "read error (%s)", strerror(errno));
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // A zero read means either no more data is currently available or our peer hungup.
    // Just return to retry as we'll detect EOF when we come back at the top of TCPReceive
    if (readCount == 0) {
        parcBuffer_Release(&wireFormatBuffer);
        return NULL;
    }

    // If it was a short read, attempt to read the remainder of the message
    while (readCount < messageLength) {
        ssize_t count = read(linkData->fd, &buffer[readCount], messageLength - readCount);
        if (count <= 0) { // on error or zero read, return to check at the top of TCPReceive for EOF
            linkData->_stats.receive_ShortRead++;
            parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink), "short read error (%s)", strerror(errno));
            parcBuffer_Release(&wireFormatBuffer);
            return NULL;
        }
        readCount += count;
    }

    parcLog_Debug(athenaTransportLink_GetLogger(athenaTransportLink), "received message (size=%d)", readCount);
    parcBuffer_SetPosition(wireFormatBuffer, parcBuffer_Position(wireFormatBuffer) + readCount);
    parcBuffer_Flip(wireFormatBuffer);

    // Construct, and return a ccnxMetaMessage from the wire format buffer.
    ccnxMetaMessage = ccnxMetaMessage_CreateFromWireFormatBuffer(wireFormatBuffer);
    if (ccnxTlvDictionary_GetSchemaVersion(ccnxMetaMessage) == CCNxTlvDictionary_SchemaVersion_V0) {
        parcLog_Warning(athenaTransportLink_GetLogger(athenaTransportLink),
                        "received deprecated version %d message\n", ccnxTlvDictionary_GetSchemaVersion(ccnxMetaMessage));
    }
    if (ccnxMetaMessage == NULL) {
        linkData->_stats.receive_DecodeFailed++;
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink), "Failed to decode message from received packet.");
    }
    parcBuffer_Release(&wireFormatBuffer);

    return ccnxMetaMessage;
}

static void
_TCPClose(AthenaTransportLink *athenaTransportLink)
{
    parcLog_Info(athenaTransportLink_GetLogger(athenaTransportLink),
                 "link %s closed", athenaTransportLink_GetName(athenaTransportLink));
    _TCPLinkData *linkData = athenaTransportLink_GetPrivateData(athenaTransportLink);
    close(linkData->fd);
    _TCPLinkData_Destroy(&linkData);
}

static void
_setConnectLinkState(AthenaTransportLink *athenaTransportLink, _TCPLinkData *linkData)
{
    athenaTransportLink_SetPrivateData(athenaTransportLink, linkData);

    // Register file descriptor to be polled.  This must be set before adding the link (case ???).
    athenaTransportLink_SetEventFd(athenaTransportLink, linkData->fd);

    // Determine and flag the link cost for forwarding messages.
    // Messages without sufficient hop count collateral will be dropped.
    // Local links will always be allowed to be taken (i.e. localhost).
    bool isLocal = false;
    if (linkData->peerAddress.sin_addr.s_addr == linkData->myAddress.sin_addr.s_addr) { // a local connection
        isLocal = true;
    }
    athenaTransportLink_SetLocal(athenaTransportLink, isLocal);

    // Allow messages to initially be sent
    athenaTransportLink_SetEvent(athenaTransportLink, AthenaTransportLinkEvent_Send);
}

static int
_setSocketOptions(AthenaTransportLinkModule *athenaTransportLinkModule, int fd)
{
    int on = 1;
    int result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *) &on, (socklen_t) sizeof(on));
    if (result) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "setsockopt failed to set SO_REUSEADDR (%s)", strerror(errno));
        return -1;
    }
#ifdef BSD_IGNORESIGPIPE
    result = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *) &on, sizeof(on));
    if (result) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "setsockopt failed to set SO_NOSIGPIPE (%s)", strerror(errno));
        return -1;
    }
#endif
    return 0;
}

static AthenaTransportLink *
_TCPOpenConnection(AthenaTransportLinkModule *athenaTransportLinkModule, const char *linkName, char *address, in_port_t port)
{
    const char *derivedLinkName;

    struct sockaddr_in *sockaddr = parcNetwork_SockInet4Address(address, port);

    _TCPLinkData *linkData = _TCPLinkData_Create();

    linkData->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (linkData->fd < 0) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule), "socket error (%s)", strerror(errno));
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    // Connect to the specified peer
    linkData->peerAddress = *sockaddr;
    linkData->peerAddressLength = sizeof(struct sockaddr);
    parcMemory_Deallocate(&sockaddr);

    int result = connect(linkData->fd, (struct sockaddr *) &linkData->peerAddress, linkData->peerAddressLength);
    if (result < 0) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule), "connect error (%s)", strerror(errno));
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    result = _setSocketOptions(athenaTransportLinkModule, linkData->fd);
    if (result) {
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    // Retrieve the local endpoint data, used to create the derived name.
    linkData->myAddressLength = sizeof(struct sockaddr);
    result = getsockname(linkData->fd, (struct sockaddr *) &linkData->myAddress, &linkData->myAddressLength);
    if (result != 0) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "Failed to obtain endpoint information from getsockname.");
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    derivedLinkName = _createNameFromLinkData(linkData);

    if (linkName == NULL) {
        linkName = derivedLinkName;
    }

    AthenaTransportLink *athenaTransportLink = athenaTransportLink_Create(linkName,
                                                                          _TCPSend,
                                                                          _TCPReceive,
                                                                          _TCPClose);
    if (athenaTransportLink == NULL) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "athenaTransportLink_Create failed");
        parcMemory_Deallocate(&derivedLinkName);
        _TCPLinkData_Destroy(&linkData);
        return athenaTransportLink;
    }

    _setConnectLinkState(athenaTransportLink, linkData);

    parcLog_Info(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                 "new link established: Name=\"%s\" (%s)", linkName, derivedLinkName);

    parcMemory_Deallocate(&derivedLinkName);
    return athenaTransportLink;
}

static CCNxMetaMessage *
_TCPReceiveListener(AthenaTransportLink *athenaTransportLink)
{
    struct _TCPLinkData *listenerData = athenaTransportLink_GetPrivateData(athenaTransportLink);

    _TCPLinkData *newLinkData = _TCPLinkData_Create();

    // Accept a new tunnel connection.
    newLinkData->peerAddressLength = sizeof(struct sockaddr_in);
    newLinkData->fd = accept(listenerData->fd, (struct sockaddr *) &newLinkData->peerAddress, &newLinkData->peerAddressLength);
    if (newLinkData->fd == -1) {
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink), "_TCPReceiveListener accept: %s", strerror(errno));
        _TCPLinkData_Destroy(&newLinkData);
        return NULL;
    }

    // Get the bound local hostname and port.  The listening address may have been wildcarded.
    newLinkData->myAddressLength = listenerData->myAddressLength;
    getsockname(newLinkData->fd, (struct sockaddr *) &newLinkData->myAddress, &newLinkData->myAddressLength);

    // Clone a new link from the current listener.
    const char *derivedLinkName = _createNameFromLinkData(newLinkData);
    AthenaTransportLink *newTransportLink = athenaTransportLink_Clone(athenaTransportLink,
                                                                      derivedLinkName,
                                                                      _TCPSend,
                                                                      _TCPReceive,
                                                                      _TCPClose);
    if (newTransportLink == NULL) {
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink),
                      "athenaTransportLink_Clone failed");
        parcMemory_Deallocate(&derivedLinkName);
        _TCPLinkData_Destroy(&newLinkData);
        return NULL;
    }

    _setConnectLinkState(newTransportLink, newLinkData);

    // Send the new link up to be added.
    int result = athenaTransportLink_AddLink(athenaTransportLink, newTransportLink);
    if (result == -1) {
        parcLog_Error(athenaTransportLink_GetLogger(athenaTransportLink),
                      "athenaTransportLinkModule_AddLink failed: %s", strerror(errno));
        close(newLinkData->fd);
        _TCPLinkData_Destroy(&newLinkData);
        athenaTransportLink_Release(&newTransportLink);
    } else {
        parcLog_Info(athenaTransportLink_GetLogger(athenaTransportLink),
                     "new link accepted by %s: %s %s",
                     athenaTransportLink_GetName(athenaTransportLink), derivedLinkName,
                     athenaTransportLink_IsNotLocal(athenaTransportLink) ? "" : "(Local)");
    }

    parcMemory_Deallocate(&derivedLinkName);

    // Could pass a message back here regarding the new link.
    return NULL;
}

static AthenaTransportLink *
_TCPOpenListener(AthenaTransportLinkModule *athenaTransportLinkModule, const char *linkName, char *address, in_port_t port)
{
    const char *derivedLinkName;

    struct sockaddr_in *sockaddr = parcNetwork_SockInet4Address(address, port);

    _TCPLinkData *linkData = _TCPLinkData_Create();

    linkData->myAddress = *sockaddr;
    linkData->myAddressLength = sizeof(struct sockaddr_in);
    parcMemory_Deallocate(&sockaddr);

    linkData->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (linkData->fd < 0) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "socket error (%s)", strerror(errno));
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    int result = _setSocketOptions(athenaTransportLinkModule, linkData->fd);
    if (result) {
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    // Set non-blocking flag
    int flags = fcntl(linkData->fd, F_GETFL, NULL);
    if (flags < 0) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "fcntl failed to get non-blocking flag (%s)", strerror(errno));
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }
    result = fcntl(linkData->fd, F_SETFL, flags | O_NONBLOCK);
    if (result) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "fcntl failed to set non-blocking flag (%s)", strerror(errno));
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    // bind and listen on requested address
    result = bind(linkData->fd, (struct sockaddr *) &linkData->myAddress, linkData->myAddressLength);
    if (result) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "bind error (%s)", strerror(errno));
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }
    int backlog = _listenerBacklog;
    result = listen(linkData->fd, backlog);
    if (result) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "listen error (%s)", strerror(errno));
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return NULL;
    }

    derivedLinkName = _createNameFromLinkData(linkData);

    if (linkName == NULL) {
        linkName = derivedLinkName;
    }

    // Listener doesn't require a send method.  The receive method is used to establish new connections.
    AthenaTransportLink *athenaTransportLink = athenaTransportLink_Create(linkName,
                                                                          NULL,
                                                                          _TCPReceiveListener,
                                                                          _TCPClose);
    if (athenaTransportLink == NULL) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "athenaTransportLink_Create failed");
        parcMemory_Deallocate(&derivedLinkName);
        close(linkData->fd);
        _TCPLinkData_Destroy(&linkData);
        return athenaTransportLink;
    }

    athenaTransportLink_SetPrivateData(athenaTransportLink, linkData);
    athenaTransportLink_SetEventFd(athenaTransportLink, linkData->fd);

    // Links established for listening are not used to route messages.
    // They can be kept in a listener list that doesn't consume a linkId.
    athenaTransportLink_SetRoutable(athenaTransportLink, false);

    parcLog_Info(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                 "new listener established: Name=\"%s\" (%s)", linkName, derivedLinkName);

    parcMemory_Deallocate(&derivedLinkName);
    return athenaTransportLink;
}

#define TCP_LISTENER_FLAG "listener"
#define LINK_NAME_SPECIFIER "name%3D"
#define LOCAL_LINK_FLAG "local%3D"

#include <parc/algol/parc_URIAuthority.h>

static AthenaTransportLink *
_TCPOpen(AthenaTransportLinkModule *athenaTransportLinkModule, PARCURI *connectionURI)
{
    AthenaTransportLink *result = 0;

    const char *authorityString = parcURI_GetAuthority(connectionURI);
    if (authorityString == NULL) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "Unable to parse connection authority %s", authorityString);
        errno = EINVAL;
        return NULL;
    }
    PARCURIAuthority *authority = parcURIAuthority_Parse(authorityString);
    const char *URIAddress = parcURIAuthority_GetHostName(authority);
    in_port_t port = parcURIAuthority_GetPort(authority);

    // Normalize the provided hostname
    struct sockaddr_in *addr = (struct sockaddr_in *) parcNetwork_SockAddress(URIAddress, port);
    char *address = inet_ntoa(addr->sin_addr);
    parcMemory_Deallocate(&addr);

    parcURIAuthority_Release(&authority);

    if (address == NULL) {
        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "Unable to lookup hostname %s", address);
        errno = EINVAL;
        return NULL;
    }

    bool listener = false;
    char name[MAXPATHLEN] = { 0 };
    char localFlag[MAXPATHLEN] = { 0 };
    int forceLocal = 0;
    char *linkName = NULL;

    PARCURIPath *remainder = parcURI_GetPath(connectionURI);
    size_t segments = parcURIPath_Count(remainder);
    for (int i = 0; i < segments; i++) {
        PARCURISegment *segment = parcURIPath_Get(remainder, i);
        const char *token = parcURISegment_ToString(segment);

        if (strcasecmp(token, TCP_LISTENER_FLAG) == 0) {
            listener = true;
            parcMemory_Deallocate(&token);
            continue;
        }

        if (strncasecmp(token, LINK_NAME_SPECIFIER, strlen(LINK_NAME_SPECIFIER)) == 0) {
            if (sscanf(token, "%*[^%%]%%3D%s", name) != 1) {
                parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                              "Improper connection name specification (%s)", token);
                parcMemory_Deallocate(&token);
                errno = EINVAL;
                return NULL;
            }
            linkName = name;
            parcMemory_Deallocate(&token);
            continue;
        }

        if (strncasecmp(token, LOCAL_LINK_FLAG, strlen(LOCAL_LINK_FLAG)) == 0) {
            if (sscanf(token, "%*[^%%]%%3D%s", localFlag) != 1) {
                parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                              "Improper local specification (%s)", token);
                parcMemory_Deallocate(&token);
                errno = EINVAL;
                return NULL;
            }
            if (strncasecmp(localFlag, "false", strlen("false")) == 0) {
                forceLocal = AthenaTransportLink_ForcedNonLocal;
            } else if (strncasecmp(localFlag, "true", strlen("true")) == 0) {
                forceLocal = AthenaTransportLink_ForcedLocal;
            } else {
                parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                              "Improper local state specification (%s)", token);
                parcMemory_Deallocate(&token);
                errno = EINVAL;
                return NULL;
            }
            parcMemory_Deallocate(&token);
            continue;
        }

        parcLog_Error(athenaTransportLinkModule_GetLogger(athenaTransportLinkModule),
                      "Unknown connection parameter (%s)", token);
        parcMemory_Deallocate(&token);
        errno = EINVAL;
        return NULL;
    }

    if (listener) {
        result = _TCPOpenListener(athenaTransportLinkModule, linkName, address, port);
    } else {
        result = _TCPOpenConnection(athenaTransportLinkModule, linkName, address, port);
    }

    // forced IsLocal/IsNotLocal, mainly for testing
    if (result && forceLocal) {
        athenaTransportLink_ForceLocal(result, forceLocal);
    }

    return result;
}

static int
_TCPPoll(AthenaTransportLink *athenaTransportLink, int timeout)
{
    return 0;
}

PARCArrayList *
athenaTransportLinkModuleTCP_Init()
{
    // TCP module for establishing point to point tunnel connections.
    AthenaTransportLinkModule *athenaTransportLinkModule;
    PARCArrayList *moduleList = parcArrayList_Create(NULL);
    assertNotNull(moduleList, "parcArrayList_Create failed to create module list");

    athenaTransportLinkModule = athenaTransportLinkModule_Create("TCP",
                                                                 _TCPOpen,
                                                                 _TCPPoll);
    assertNotNull(athenaTransportLinkModule, "parcMemory_AllocateAndClear failed allocate TCP athenaTransportLinkModule");
    bool result = parcArrayList_Add(moduleList, athenaTransportLinkModule);
    assertTrue(result == true, "parcArrayList_Add failed");

    return moduleList;
}
