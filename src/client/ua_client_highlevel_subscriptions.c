/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ua_client_highlevel.h"
#include "ua_client_internal.h"
#include "ua_util.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS /* conditional compilation */

UA_StatusCode
UA_Client_Subscriptions_new(UA_Client *client, UA_SubscriptionSettings settings,
                            UA_UInt32 *newSubscriptionId) {
    UA_CreateSubscriptionRequest request;
    UA_CreateSubscriptionRequest_init(&request);
    request.requestedPublishingInterval = settings.requestedPublishingInterval;
    request.requestedLifetimeCount = settings.requestedLifetimeCount;
    request.requestedMaxKeepAliveCount = settings.requestedMaxKeepAliveCount;
    request.maxNotificationsPerPublish = settings.maxNotificationsPerPublish;
    request.publishingEnabled = settings.publishingEnabled;
    request.priority = settings.priority;

    UA_CreateSubscriptionResponse response = UA_Client_Service_createSubscription(client, request);
    UA_StatusCode retval = response.responseHeader.serviceResult;
    if(retval != UA_STATUSCODE_GOOD) {
        UA_CreateSubscriptionResponse_deleteMembers(&response);
        return retval;
    }

    UA_Client_Subscription *newSub = (UA_Client_Subscription *)UA_malloc(sizeof(UA_Client_Subscription));
    if(!newSub) {
        UA_CreateSubscriptionResponse_deleteMembers(&response);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    LIST_INIT(&newSub->monitoredItems);
    newSub->lifeTime = response.revisedLifetimeCount;
    newSub->keepAliveCount = response.revisedMaxKeepAliveCount;
    newSub->publishingInterval = response.revisedPublishingInterval;
    newSub->subscriptionID = response.subscriptionId;
    newSub->notificationsPerPublish = request.maxNotificationsPerPublish;
    newSub->priority = request.priority;
    LIST_INSERT_HEAD(&client->subscriptions, newSub, listEntry);

    if(newSubscriptionId)
        *newSubscriptionId = newSub->subscriptionID;

    UA_CreateSubscriptionResponse_deleteMembers(&response);
    return UA_STATUSCODE_GOOD;
}

static UA_Client_Subscription *findSubscription(const UA_Client *client, UA_UInt32 subscriptionId)
{
    UA_Client_Subscription *sub = NULL;
    LIST_FOREACH(sub, &client->subscriptions, listEntry) {
        if(sub->subscriptionID == subscriptionId)
            break;
    }
    return sub;
}

/* remove the subscription remotely */
UA_StatusCode
UA_Client_Subscriptions_remove(UA_Client *client, UA_UInt32 subscriptionId) {
    UA_Client_Subscription *sub = findSubscription(client, subscriptionId);
    if(!sub)
        return UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;

    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_Client_MonitoredItem *mon, *tmpmon;
    LIST_FOREACH_SAFE(mon, &sub->monitoredItems, listEntry, tmpmon) {
        retval =
            UA_Client_Subscriptions_removeMonitoredItem(client, sub->subscriptionID,
                                                        mon->monitoredItemId);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
    }

    /* remove the subscription remotely */
    UA_DeleteSubscriptionsRequest request;
    UA_DeleteSubscriptionsRequest_init(&request);
    request.subscriptionIdsSize = 1;
    request.subscriptionIds = &sub->subscriptionID;
    UA_DeleteSubscriptionsResponse response = UA_Client_Service_deleteSubscriptions(client, request);
    retval = response.responseHeader.serviceResult;
    if(retval == UA_STATUSCODE_GOOD && response.resultsSize > 0)
        retval = response.results[0];
    UA_DeleteSubscriptionsResponse_deleteMembers(&response);

    if(retval != UA_STATUSCODE_GOOD && retval != UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID) {
        UA_LOG_INFO(client->config.logger, UA_LOGCATEGORY_CLIENT,
                    "Could not remove subscription %u with error code %s",
                    sub->subscriptionID, UA_StatusCode_name(retval));
        return retval;
    }

    UA_Client_Subscriptions_forceDelete(client, sub);
    return UA_STATUSCODE_GOOD;
}

void
UA_Client_Subscriptions_forceDelete(UA_Client *client,
                                    UA_Client_Subscription *sub) {
    UA_Client_MonitoredItem *mon, *mon_tmp;
    LIST_FOREACH_SAFE(mon, &sub->monitoredItems, listEntry, mon_tmp) {
        UA_NodeId_deleteMembers(&mon->monitoredNodeId);
        LIST_REMOVE(mon, listEntry);
        UA_free(mon);
    }
    LIST_REMOVE(sub, listEntry);
    UA_free(sub);
}

UA_StatusCode
UA_Client_Subscriptions_addMonitoredEvent(UA_Client *client, const UA_UInt32 subscriptionId,
                                         const UA_NodeId nodeId, const UA_UInt32 attributeID,
                                         UA_SimpleAttributeOperand *selectClause,
                                         const size_t nSelectClauses,
                                         UA_ContentFilterElement *whereClause,
                                         const size_t nWhereClauses,
                                         const UA_MonitoredEventHandlingFunction hf,
                                         void *hfContext, UA_UInt32 *newMonitoredItemId) {
    UA_Client_Subscription *sub = findSubscription(client, subscriptionId);
    if(!sub)
        return UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;

    /* Send the request */
    UA_CreateMonitoredItemsRequest request;
    UA_CreateMonitoredItemsRequest_init(&request);
    request.subscriptionId = subscriptionId;

    UA_MonitoredItemCreateRequest item;
    UA_MonitoredItemCreateRequest_init(&item);
    item.itemToMonitor.nodeId = nodeId;
    item.itemToMonitor.attributeId = attributeID;
    item.monitoringMode = UA_MONITORINGMODE_REPORTING;
    item.requestedParameters.clientHandle = ++(client->monitoredItemHandles);
    item.requestedParameters.samplingInterval = 0;
    item.requestedParameters.discardOldest = false;

    UA_EventFilter *evFilter = UA_EventFilter_new();
    if(!evFilter) {
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    UA_EventFilter_init(evFilter);
    evFilter->selectClausesSize = nSelectClauses;
    evFilter->selectClauses = selectClause;
    evFilter->whereClause.elementsSize = nWhereClauses;
    evFilter->whereClause.elements = whereClause;

    item.requestedParameters.filter.encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE;
    item.requestedParameters.filter.content.decoded.type = &UA_TYPES[UA_TYPES_EVENTFILTER];
    item.requestedParameters.filter.content.decoded.data = evFilter;

    request.itemsToCreate = &item;
    request.itemsToCreateSize = 1;
    UA_CreateMonitoredItemsResponse response = UA_Client_Service_createMonitoredItems(client, request);

    // slight misuse of retval here to check if the deletion was successfull.
    UA_StatusCode retval;
    if(response.resultsSize == 0)
        retval = response.responseHeader.serviceResult;
    else
        retval = response.results[0].statusCode;
    if(retval != UA_STATUSCODE_GOOD) {
        UA_CreateMonitoredItemsResponse_deleteMembers(&response);
        UA_EventFilter_delete(evFilter);
        return retval;
    }

    /* Create the handler */
    UA_Client_MonitoredItem *newMon = (UA_Client_MonitoredItem *)UA_malloc(sizeof(UA_Client_MonitoredItem));
    if(!newMon) {
        UA_CreateMonitoredItemsResponse_deleteMembers(&response);
        UA_EventFilter_delete(evFilter);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }

    newMon->monitoringMode = UA_MONITORINGMODE_REPORTING;
    UA_NodeId_copy(&nodeId, &newMon->monitoredNodeId);
    newMon->attributeID = attributeID;
    newMon->clientHandle = client->monitoredItemHandles;
    newMon->samplingInterval = 0;
    newMon->queueSize = 0;
    newMon->discardOldest = false;

    newMon->handlerEvents = hf;
    newMon->handlerEventsContext = hfContext;
    newMon->monitoredItemId = response.results[0].monitoredItemId;
    LIST_INSERT_HEAD(&sub->monitoredItems, newMon, listEntry);
    *newMonitoredItemId = newMon->monitoredItemId;

    UA_LOG_DEBUG(client->config.logger, UA_LOGCATEGORY_CLIENT,
                 "Created a monitored item with client handle %u", client->monitoredItemHandles);

    UA_EventFilter_delete(evFilter);
    UA_CreateMonitoredItemsResponse_deleteMembers(&response);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Client_Subscriptions_addMonitoredItem(UA_Client *client, UA_UInt32 subscriptionId,
                                         UA_NodeId nodeId, UA_UInt32 attributeID,
                                         UA_MonitoredItemHandlingFunction hf,
                                         void *hfContext, UA_UInt32 *newMonitoredItemId,
                                         UA_Double samplingInterval) {
    UA_Client_Subscription *sub = findSubscription(client, subscriptionId);
    if(!sub)
        return UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;

    /* Create the handler */
    UA_Client_MonitoredItem *newMon = (UA_Client_MonitoredItem*)UA_malloc(sizeof(UA_Client_MonitoredItem));
    if(!newMon)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Send the request */
    UA_CreateMonitoredItemsRequest request;
    UA_CreateMonitoredItemsRequest_init(&request);
    request.subscriptionId = subscriptionId;
    UA_MonitoredItemCreateRequest item;
    UA_MonitoredItemCreateRequest_init(&item);
    item.itemToMonitor.nodeId = nodeId;
    item.itemToMonitor.attributeId = attributeID;
    item.monitoringMode = UA_MONITORINGMODE_REPORTING;
    item.requestedParameters.clientHandle = ++(client->monitoredItemHandles);
    item.requestedParameters.samplingInterval = samplingInterval;
    item.requestedParameters.discardOldest = true;
    item.requestedParameters.queueSize = 1;
    request.itemsToCreate = &item;
    request.itemsToCreateSize = 1;
    UA_CreateMonitoredItemsResponse response = UA_Client_Service_createMonitoredItems(client, request);

    // slight misuse of retval here to check if the addition was successfull.
    UA_StatusCode retval = response.responseHeader.serviceResult;
    if(retval == UA_STATUSCODE_GOOD) {
        if(response.resultsSize == 1)
            retval = response.results[0].statusCode;
        else
            retval = UA_STATUSCODE_BADUNEXPECTEDERROR;
    }
    if(retval != UA_STATUSCODE_GOOD) {
        UA_free(newMon);
        UA_CreateMonitoredItemsResponse_deleteMembers(&response);
        return retval;
    }

    /* Set the handler */
    newMon->monitoringMode = UA_MONITORINGMODE_REPORTING;
    UA_NodeId_copy(&nodeId, &newMon->monitoredNodeId);
    newMon->attributeID = attributeID;
    newMon->clientHandle = client->monitoredItemHandles;
    newMon->samplingInterval = samplingInterval;
    newMon->queueSize = 1;
    newMon->discardOldest = true;
    newMon->handler = hf;
    newMon->handlerContext = hfContext;
    newMon->monitoredItemId = response.results[0].monitoredItemId;
    LIST_INSERT_HEAD(&sub->monitoredItems, newMon, listEntry);
    *newMonitoredItemId = newMon->monitoredItemId;

    UA_LOG_DEBUG(client->config.logger, UA_LOGCATEGORY_CLIENT,
                 "Created a monitored item with client handle %u",
                 client->monitoredItemHandles);

    UA_CreateMonitoredItemsResponse_deleteMembers(&response);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Client_Subscriptions_removeMonitoredItem(UA_Client *client, UA_UInt32 subscriptionId,
                                            UA_UInt32 monitoredItemId) {
    UA_Client_Subscription *sub = findSubscription(client, subscriptionId);
    if(!sub)
        return UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;

    UA_Client_MonitoredItem *mon;
    LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
        if(mon->monitoredItemId == monitoredItemId)
            break;
    }
    if(!mon)
        return UA_STATUSCODE_BADMONITOREDITEMIDINVALID;

    /* remove the monitoreditem remotely */
    UA_DeleteMonitoredItemsRequest request;
    UA_DeleteMonitoredItemsRequest_init(&request);
    request.subscriptionId = sub->subscriptionID;
    request.monitoredItemIdsSize = 1;
    request.monitoredItemIds = &mon->monitoredItemId;
    UA_DeleteMonitoredItemsResponse response = UA_Client_Service_deleteMonitoredItems(client, request);

    UA_StatusCode retval = response.responseHeader.serviceResult;
    if(retval == UA_STATUSCODE_GOOD && response.resultsSize > 1)
        retval = response.results[0];
    UA_DeleteMonitoredItemsResponse_deleteMembers(&response);
    if(retval != UA_STATUSCODE_GOOD &&
       retval != UA_STATUSCODE_BADMONITOREDITEMIDINVALID) {
        UA_LOG_INFO(client->config.logger, UA_LOGCATEGORY_CLIENT,
                    "Could not remove monitoreditem %u with error code %s",
                    monitoredItemId, UA_StatusCode_name(retval));
        return retval;
    }

    LIST_REMOVE(mon, listEntry);
    UA_NodeId_deleteMembers(&mon->monitoredNodeId);
    UA_free(mon);
    return UA_STATUSCODE_GOOD;
}

static void
UA_Client_processPublishResponse(UA_Client *client, UA_PublishRequest *request,
                                 UA_PublishResponse *response) {
    if(response->responseHeader.serviceResult != UA_STATUSCODE_GOOD)
        return;

    UA_Client_Subscription *sub = findSubscription(client, response->subscriptionId);
    if(!sub)
        return;

    /* Check if the server has acknowledged any of the sent ACKs */
    for(size_t i = 0; i < response->resultsSize && i < request->subscriptionAcknowledgementsSize; ++i) {
        /* remove also acks that are unknown to the server */
        if(response->results[i] != UA_STATUSCODE_GOOD &&
           response->results[i] != UA_STATUSCODE_BADSEQUENCENUMBERUNKNOWN)
            continue;

        /* Remove the ack from the list */
        UA_SubscriptionAcknowledgement *orig_ack = &request->subscriptionAcknowledgements[i];
        UA_Client_NotificationsAckNumber *ack;
        LIST_FOREACH(ack, &client->pendingNotificationsAcks, listEntry) {
            if(ack->subAck.subscriptionId == orig_ack->subscriptionId &&
               ack->subAck.sequenceNumber == orig_ack->sequenceNumber) {
                LIST_REMOVE(ack, listEntry);
                UA_free(ack);
                UA_assert(ack != LIST_FIRST(&client->pendingNotificationsAcks));
                break;
            }
        }
    }

    /* Process the notification messages */
    UA_NotificationMessage *msg = &response->notificationMessage;
    for(size_t k = 0; k < msg->notificationDataSize; ++k) {
        if(msg->notificationData[k].encoding != UA_EXTENSIONOBJECT_DECODED)
            continue;

        if(msg->notificationData[k].content.decoded.type == &UA_TYPES[UA_TYPES_DATACHANGENOTIFICATION]) {
            UA_DataChangeNotification *dataChangeNotification = (UA_DataChangeNotification *)msg->notificationData[k].content.decoded.data;
            for(size_t j = 0; j < dataChangeNotification->monitoredItemsSize; ++j) {
                UA_MonitoredItemNotification *mitemNot = &dataChangeNotification->monitoredItems[j];
                UA_Client_MonitoredItem *mon;
                LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
                    if(mon->clientHandle == mitemNot->clientHandle) {
                        mon->handler(mon->monitoredItemId, &mitemNot->value, mon->handlerContext);
                        break;
                    }
                }
                if(!mon)
                    UA_LOG_DEBUG(client->config.logger, UA_LOGCATEGORY_CLIENT,
                                 "Could not process a notification with clienthandle %u on subscription %u",
                                 mitemNot->clientHandle, sub->subscriptionID);
            }
        }
        else if(msg->notificationData[k].content.decoded.type == &UA_TYPES[UA_TYPES_EVENTNOTIFICATIONLIST]) {
            UA_EventNotificationList *eventNotificationList = (UA_EventNotificationList *)msg->notificationData[k].content.decoded.data;
            for (size_t j = 0; j < eventNotificationList->eventsSize; ++j) {
                UA_EventFieldList *eventFieldList = &eventNotificationList->events[j];
                UA_Client_MonitoredItem *mon;
                LIST_FOREACH(mon, &sub->monitoredItems, listEntry) {
                    if(mon->clientHandle == eventFieldList->clientHandle) {
                        mon->handlerEvents(mon->monitoredItemId, eventFieldList->eventFieldsSize,
                                           eventFieldList->eventFields, mon->handlerContext);
                        break;
                    }
                }
                if(!mon)
                    UA_LOG_DEBUG(client->config.logger, UA_LOGCATEGORY_CLIENT,
                                 "Could not process a notification with clienthandle %u on subscription %u",
                                 eventFieldList->clientHandle, sub->subscriptionID);
            }
        }
        else {
            continue; // no other types are supported
        }
    }

    /* Add to the list of pending acks */
    UA_Client_NotificationsAckNumber *tmpAck =
        (UA_Client_NotificationsAckNumber*)UA_malloc(sizeof(UA_Client_NotificationsAckNumber));
    if(!tmpAck) {
        UA_LOG_WARNING(client->config.logger, UA_LOGCATEGORY_CLIENT,
                       "Not enough memory to store the acknowledgement for a publish "
                       "message on subscription %u", sub->subscriptionID);
        return;
    }
    tmpAck->subAck.sequenceNumber = msg->sequenceNumber;
    tmpAck->subAck.subscriptionId = sub->subscriptionID;
    LIST_INSERT_HEAD(&client->pendingNotificationsAcks, tmpAck, listEntry);
}

UA_StatusCode
UA_Client_Subscriptions_manuallySendPublishRequest(UA_Client *client) {
    if(client->state < UA_CLIENTSTATE_SESSION)
        return UA_STATUSCODE_BADSERVERNOTCONNECTED;

    UA_StatusCode retval = UA_STATUSCODE_GOOD;

    UA_DateTime now = UA_DateTime_nowMonotonic();
    UA_DateTime maxDate = now + (UA_DateTime)(client->config.timeout * UA_MSEC_TO_DATETIME);

    UA_Boolean moreNotifications = true;
    while(moreNotifications) {
        UA_PublishRequest request;
        UA_PublishRequest_init(&request);
        request.subscriptionAcknowledgementsSize = 0;

        UA_Client_NotificationsAckNumber *ack;
        LIST_FOREACH(ack, &client->pendingNotificationsAcks, listEntry)
            ++request.subscriptionAcknowledgementsSize;
        if(request.subscriptionAcknowledgementsSize > 0) {
            request.subscriptionAcknowledgements = (UA_SubscriptionAcknowledgement*)
                UA_malloc(sizeof(UA_SubscriptionAcknowledgement) * request.subscriptionAcknowledgementsSize);
            if(!request.subscriptionAcknowledgements)
                return UA_STATUSCODE_BADOUTOFMEMORY;
        }

        int i = 0;
        LIST_FOREACH(ack, &client->pendingNotificationsAcks, listEntry) {
            request.subscriptionAcknowledgements[i].sequenceNumber = ack->subAck.sequenceNumber;
            request.subscriptionAcknowledgements[i].subscriptionId = ack->subAck.subscriptionId;
            ++i;
        }

        UA_PublishResponse response = UA_Client_Service_publish(client, request);
        UA_Client_processPublishResponse(client, &request, &response);
        
        now = UA_DateTime_nowMonotonic();
        if (now > maxDate){
            moreNotifications = UA_FALSE;
            retval = UA_STATUSCODE_GOODNONCRITICALTIMEOUT;
        }else{
            moreNotifications = response.moreNotifications;
        }
        
        UA_PublishResponse_deleteMembers(&response);
        UA_PublishRequest_deleteMembers(&request);
    }
    
    if(client->state < UA_CLIENTSTATE_SESSION)
        return UA_STATUSCODE_BADSERVERNOTCONNECTED;

    return retval;
}

#endif /* UA_ENABLE_SUBSCRIPTIONS */
