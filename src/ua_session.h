/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef UA_SESSION_H_
#define UA_SESSION_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "queue.h"
#include "ua_types.h"
#include "ua_securechannel.h"
#include "ua_server.h"
#include "ua_util.h"

#define UA_MAXCONTINUATIONPOINTS 5

typedef struct ContinuationPointEntry {
    LIST_ENTRY(ContinuationPointEntry) pointers;
    UA_ByteString        identifier;
    UA_BrowseDescription browseDescription;
    UA_UInt32            maxReferences;

    /* The last point in the node references? */
    size_t referenceKindIndex;
    size_t targetIndex;
} ContinuationPointEntry;

struct UA_Subscription;
typedef struct UA_Subscription UA_Subscription;

#ifdef UA_ENABLE_SUBSCRIPTIONS
typedef struct UA_PublishResponseEntry {
    SIMPLEQ_ENTRY(UA_PublishResponseEntry) listEntry;
    UA_UInt32 requestId;
    UA_PublishResponse response;
} UA_PublishResponseEntry;
#endif

struct UA_Session {
    UA_ApplicationDescription clientDescription;
    UA_String         sessionName;
    UA_Boolean        activated;
    void             *sessionHandle; // pointer assigned in userland-callback
    UA_NodeId         authenticationToken;
    UA_NodeId         sessionId;
    UA_UInt32         maxRequestMessageSize;
    UA_UInt32         maxResponseMessageSize;
    UA_Double         timeout; // [ms]
    UA_DateTime       validTill;
    UA_ByteString     serverNonce;
    UA_SecureChannel *channel;
    UA_UInt16 availableContinuationPoints;
    LIST_HEAD(ContinuationPointList, ContinuationPointEntry) continuationPoints;
#ifdef UA_ENABLE_SUBSCRIPTIONS
    UA_UInt32 lastSubscriptionID;
    UA_UInt32 lastSeenSubscriptionID;
    LIST_HEAD(UA_ListOfUASubscriptions, UA_Subscription) serverSubscriptions;
    SIMPLEQ_HEAD(UA_ListOfQueuedPublishResponses, UA_PublishResponseEntry) responseQueue;
#endif
};

/* Local access to the services (for startup and maintenance) uses this Session
 * with all possible access rights (Session ID: 1) */
extern UA_Session adminSession;

void UA_Session_init(UA_Session *session);
void UA_Session_deleteMembersCleanup(UA_Session *session, UA_Server *server);

/* If any activity on a session happens, the timeout is extended */
void UA_Session_updateLifetime(UA_Session *session);

#ifdef UA_ENABLE_SUBSCRIPTIONS
void UA_Session_addSubscription(UA_Session *session, UA_Subscription *newSubscription);

UA_Subscription *
UA_Session_getSubscriptionByID(UA_Session *session, UA_UInt32 subscriptionID);

UA_StatusCode
UA_Session_deleteSubscription(UA_Server *server, UA_Session *session,
                              UA_UInt32 subscriptionID);

UA_UInt32
UA_Session_getUniqueSubscriptionID(UA_Session *session);
#endif

/**
 * Log Helper
 * ----------
 * We have to jump through some hoops to enable the use of format strings
 * without arguments since (pedantic) C99 does not allow variadic macros with
 * zero arguments. So we add a dummy argument that is not printed (%.0s is
 * string of length zero). */

#define UA_LOG_TRACE_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_TRACE(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_TRACE_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_TRACE_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#define UA_LOG_DEBUG_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_DEBUG(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_DEBUG_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_DEBUG_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#define UA_LOG_INFO_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_INFO(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_INFO_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_INFO_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#define UA_LOG_WARNING_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_WARNING(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_WARNING_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_WARNING_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#define UA_LOG_ERROR_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_ERROR(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_ERROR_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_ERROR_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#define UA_LOG_FATAL_SESSION_INTERNAL(LOGGER, SESSION, MSG, ...)        \
    UA_LOG_FATAL(LOGGER, UA_LOGCATEGORY_SESSION,                        \
                 "Connection %i | SecureChannel %i | Session " UA_PRINTF_GUID_FORMAT " | " MSG "%.0s", \
                 ((SESSION)->channel ? ((SESSION)->channel->connection ? (SESSION)->channel->connection->sockfd : 0) : 0), \
                 ((SESSION)->channel ? (SESSION)->channel->securityToken.channelId : 0), \
                 UA_PRINTF_GUID_DATA((SESSION)->sessionId.identifier.guid), __VA_ARGS__)

#define UA_LOG_FATAL_SESSION(LOGGER, SESSION, ...)                      \
    UA_MACRO_EXPAND(UA_LOG_FATAL_SESSION_INTERNAL(LOGGER, SESSION, __VA_ARGS__, ""))

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* UA_SESSION_H_ */
