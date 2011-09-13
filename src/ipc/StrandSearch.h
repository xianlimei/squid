/*
 * $Id$
 *
 */

#ifndef SQUID_IPC_STRAND_SEARCH_H
#define SQUID_IPC_STRAND_SEARCH_H

#include "ipc/forward.h"
#include "ipc/StrandCoord.h"
#include "SquidString.h"

namespace Ipc
{

/// asynchronous strand search request
class StrandSearchRequest
{
public:
    StrandSearchRequest();
    explicit StrandSearchRequest(const TypedMsgHdr &hdrMsg); ///< from recvmsg()
    void pack(TypedMsgHdr &hdrMsg) const; ///< prepare for sendmsg()

public:
    int requestorId; ///< sender-provided return address
    String tag; ///< set when looking for a matching StrandCoord::tag
};

/// asynchronous strand search response
class StrandSearchResponse
{
public:
    StrandSearchResponse(const StrandCoord &strand);
    explicit StrandSearchResponse(const TypedMsgHdr &hdrMsg); ///< from recvmsg()
    void pack(TypedMsgHdr &hdrMsg) const; ///< prepare for sendmsg()

public:
    StrandCoord strand; ///< answer matching StrandSearchRequest criteria
};

} // namespace Ipc;

#endif /* SQUID_IPC_STRAND_SEARCH_H */
