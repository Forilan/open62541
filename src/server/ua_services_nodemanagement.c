#include "ua_server_internal.h"
#include "ua_services.h"

/**********************/
/* Consistency Checks */
/**********************/

/* Check if the requested parent node exists, has the right node class and is
 * referenced with an allowed (hierarchical) reference type. For "type" nodes,
 * only hasSubType references are allowed. */
static UA_StatusCode
checkParentReference(UA_Server *server, UA_Session *session, UA_NodeClass nodeClass,
                     const UA_NodeId *parentNodeId, const UA_NodeId *referenceTypeId) {
    /* See if the parent exists */
    const UA_Node *parent = UA_NodeStore_get(server->nodestore, parentNodeId);
    if(!parent) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Parent node not found");
        return UA_STATUSCODE_BADPARENTNODEIDINVALID;
    }

    /* Check the referencetype exists */
    const UA_ReferenceTypeNode *referenceType =
        (const UA_ReferenceTypeNode*)UA_NodeStore_get(server->nodestore, referenceTypeId);
    if(!referenceType) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference type to the parent not found");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    /* Check if the referencetype is a reference type node */
    if(referenceType->nodeClass != UA_NODECLASS_REFERENCETYPE) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference type to the parent invalid");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    /* Check that the reference type is not abstract */
    if(referenceType->isAbstract == true) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Abstract reference type to the parent invalid");
        return UA_STATUSCODE_BADREFERENCENOTALLOWED;
    }

    /* Check hassubtype relation for type nodes */
    const UA_NodeId subtypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
    if(nodeClass == UA_NODECLASS_DATATYPE ||
       nodeClass == UA_NODECLASS_VARIABLETYPE ||
       nodeClass == UA_NODECLASS_OBJECTTYPE ||
       nodeClass == UA_NODECLASS_REFERENCETYPE) {
        /* type needs hassubtype reference to the supertype */
        if(!UA_NodeId_equal(referenceTypeId, &subtypeId)) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                 "AddNodes: New type node need to have a "
                                 "hasSubtype reference");
            return UA_STATUSCODE_BADREFERENCENOTALLOWED;
        }
        /* supertype needs to be of the same node type  */
        if(parent->nodeClass != nodeClass) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                 "AddNodes: New type node needs to be of the same "
                                 "node type as the parent");
            return UA_STATUSCODE_BADPARENTNODEIDINVALID;
        }
        return UA_STATUSCODE_GOOD;
    }

    /* Test if the referencetype is hierarchical */
    const UA_NodeId hierarchicalReference =
        UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    if(!isNodeInTree(server->nodestore, referenceTypeId,
                     &hierarchicalReference, &subtypeId, 1)) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference to the parent is not hierarchical");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    return UA_STATUSCODE_GOOD;
}

/* Check the consistency of the variable (or variable type) attributes data
 * type, value rank, array dimensions internally and against the parent variable
 * type. */
static UA_StatusCode
typeCheckVariableNode(UA_Server *server, UA_Session *session, const UA_VariableNode *node,
                      const UA_NodeId *typeDef) {
    /* Omit some type checks for ns0 generation */
    const UA_NodeId baseDataVariableType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    if(UA_NodeId_equal(&node->nodeId, &baseDataVariableType))
        return UA_STATUSCODE_GOOD;

    /* Get the variable type */
    const UA_VariableTypeNode *vt =
        (const UA_VariableTypeNode*)UA_NodeStore_get(server->nodestore, typeDef);
    if(!vt || vt->nodeClass != UA_NODECLASS_VARIABLETYPE)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    if(node->nodeClass == UA_NODECLASS_VARIABLE && vt->isAbstract)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;

    /* Check the datatype against the vt */
    if(!compatibleDataType(server, &node->dataType, &vt->dataType))
        return UA_STATUSCODE_BADTYPEMISMATCH;

    /* We need the value for some checks. Might come from a datasource. */
    UA_DataValue value;
    UA_DataValue_init(&value);
    UA_StatusCode retval = readValueAttribute(server, node, &value);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Get the array dimensions */
    size_t arrayDims = node->arrayDimensionsSize;
    if(arrayDims == 0 && value.hasValue && value.value.type &&
       !UA_Variant_isScalar(&value.value)) {
        arrayDims = 1; /* No array dimensions on an array implies one dimension */
    }

    /* Check valueRank against array dimensions */
    retval = compatibleValueRankArrayDimensions(node->valueRank, arrayDims);
    if(retval != UA_STATUSCODE_GOOD)
        goto cleanup;

    /* Check valueRank against the vt */
    retval = compatibleValueRanks(node->valueRank, vt->valueRank);
    if(retval != UA_STATUSCODE_GOOD)
        goto cleanup;

    /* Check array dimensions against the vt */
    retval = compatibleArrayDimensions(node->arrayDimensionsSize, node->arrayDimensions,
                                       vt->arrayDimensionsSize, vt->arrayDimensions);
    if(retval != UA_STATUSCODE_GOOD)
        goto cleanup;

    /* Set a sane valueRank (the most permissive) */
    UA_Int32 valueRank = node->valueRank;
    if(valueRank == 0 && value.hasValue && !UA_Variant_isScalar(&value.value)) {
        valueRank = -2;
        UA_Server_writeValueRank(server, node->nodeId, valueRank);
    }

    /* This internally converts the value to a valid type if possible */
    if(value.hasValue)
        retval = typeCheckValue(server, &node->dataType, node->valueRank,
                                node->arrayDimensionsSize, node->arrayDimensions,
                                &value.value, NULL, NULL);

 cleanup:
    UA_DataValue_deleteMembers(&value); /* Free the value before any return clause */
    return retval;
}

/********************/
/* Instantiate Node */
/********************/

static UA_StatusCode
setObjectInstanceHandle(UA_Server *server, UA_Session *session, UA_ObjectNode* node,
                        void * (*constructor)(const UA_NodeId instance)) {
    if(node->nodeClass != UA_NODECLASS_OBJECT)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    if(!node->instanceHandle)
        node->instanceHandle = constructor(node->nodeId);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyChildNodes(UA_Server *server, UA_Session *session, 
               const UA_NodeId *sourceNodeId, const UA_NodeId *destinationNodeId, 
               UA_InstantiationCallback *instantiationCallback);

static UA_StatusCode
Service_AddNode_finish(UA_Server *server, UA_Session *session,
                       const UA_NodeId *nodeId, const UA_NodeId *parentNodeId,
                       const UA_NodeId *referenceTypeId, const UA_NodeId *typeDefinition,
                       UA_InstantiationCallback *instantiationCallback);

static UA_StatusCode
instantiateNode(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
                UA_NodeClass nodeClass, const UA_NodeId *typeId,
                UA_InstantiationCallback *instantiationCallback) {
    /* Currently, only variables and objects are instantiated */
    if(nodeClass != UA_NODECLASS_VARIABLE &&
       nodeClass != UA_NODECLASS_OBJECT)
        return UA_STATUSCODE_GOOD;
        
    /* Get the type node */
    const UA_Node *typenode = UA_NodeStore_get(server->nodestore, typeId);
    if(!typenode)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;

    /* See if the type has the correct node class */
    if(nodeClass == UA_NODECLASS_VARIABLE) {
        if(typenode->nodeClass != UA_NODECLASS_VARIABLETYPE ||
           ((const UA_VariableTypeNode*)typenode)->isAbstract)
            return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    } else { /* nodeClass == UA_NODECLASS_OBJECT */
        if(typenode->nodeClass != UA_NODECLASS_OBJECTTYPE ||
           ((const UA_ObjectTypeNode*)typenode)->isAbstract)
            return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    }

    /* Get the hierarchy of the type and all its supertypes */
    UA_NodeId *hierarchy = NULL;
    size_t hierarchySize = 0;
    UA_StatusCode retval = getTypeHierarchy(server->nodestore, typenode,
                                            true, &hierarchy, &hierarchySize);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    
    /* Copy members of the type and supertypes */
    for(size_t i = 0; i < hierarchySize; ++i)
        retval |= copyChildNodes(server, session, &hierarchy[i],
                                 nodeId, instantiationCallback);
    UA_Array_delete(hierarchy, hierarchySize, &UA_TYPES[UA_TYPES_NODEID]);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Call the object constructor */
    if(typenode->nodeClass == UA_NODECLASS_OBJECTTYPE) {
        const UA_ObjectLifecycleManagement *olm =
            &((const UA_ObjectTypeNode*)typenode)->lifecycleManagement;
        if(olm->constructor)
            UA_Server_editNode(server, session, nodeId,
                               (UA_EditNodeCallback)setObjectInstanceHandle,
                               olm->constructor);
    }

    /* Add a hasType reference */
    UA_AddReferencesItem addref;
    UA_AddReferencesItem_init(&addref);
    addref.sourceNodeId = *nodeId;
    addref.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASTYPEDEFINITION);
    addref.isForward = true;
    addref.targetNodeId.nodeId = *typeId;
    retval = Service_AddReferences_single(server, session, &addref);

    /* Call custom callback */
    if(retval == UA_STATUSCODE_GOOD && instantiationCallback)
        instantiationCallback->method(*nodeId, *typeId, instantiationCallback->handle);
    return retval;
}

/* Search for an instance of "browseName" in node searchInstance Used during
 * copyChildNodes to find overwritable/mergable nodes */
static UA_StatusCode
instanceFindAggregateByBrowsename(UA_Server *server, UA_Session *session, 
                                  const UA_NodeId *searchInstance, 
                                  const UA_QualifiedName *browseName,
                                  UA_NodeId *outInstanceNodeId) {
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = *searchInstance;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_AGGREGATES);
    bd.includeSubtypes = true;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.nodeClassMask = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE | UA_NODECLASS_METHOD;
    bd.resultMask = UA_BROWSERESULTMASK_NODECLASS | UA_BROWSERESULTMASK_BROWSENAME;
    
    UA_BrowseResult br;
    UA_BrowseResult_init(&br);
    Service_Browse_single(server, session, NULL, &bd, 0, &br);
    if(br.statusCode != UA_STATUSCODE_GOOD)
        return br.statusCode;
    
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    for(size_t i = 0; i < br.referencesSize; ++i) {
        UA_ReferenceDescription *rd = &br.references[i];
        if(rd->browseName.namespaceIndex == browseName->namespaceIndex &&
           UA_String_equal(&rd->browseName.name, &browseName->name)) {
            retval = UA_NodeId_copy(&rd->nodeId.nodeId, outInstanceNodeId);
            break;
        }
    }
    
    UA_BrowseResult_deleteMembers(&br);
    return retval;
}

static UA_StatusCode
copyChildNode(UA_Server *server, UA_Session *session,
              const UA_NodeId *destinationNodeId, 
              const UA_ReferenceDescription *rd,
              UA_InstantiationCallback *instantiationCallback) {
    UA_NodeId existingChild = UA_NODEID_NULL;
    UA_StatusCode retval =
        instanceFindAggregateByBrowsename(server, session, destinationNodeId,
                                          &rd->browseName, &existingChild);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
        
    /* Have a child with that browseName. Try to deep-copy missing members. */
    if(!UA_NodeId_isNull(&existingChild)) {
        if(rd->nodeClass == UA_NODECLASS_VARIABLE ||
           rd->nodeClass == UA_NODECLASS_OBJECT)
            retval = copyChildNodes(server, session, &rd->nodeId.nodeId,
                                    &existingChild, instantiationCallback);
        UA_NodeId_deleteMembers(&existingChild);
        return retval;
    }

    /* No existing child with that browsename. Create it. */
    if(rd->nodeClass == UA_NODECLASS_METHOD) {
        /* Add a reference to the method in the objecttype */
        UA_AddReferencesItem newItem;
        UA_AddReferencesItem_init(&newItem);
        newItem.sourceNodeId = *destinationNodeId;
        newItem.referenceTypeId = rd->referenceTypeId;
        newItem.isForward = true;
        newItem.targetNodeId = rd->nodeId;
        newItem.targetNodeClass = UA_NODECLASS_METHOD;
        retval = Service_AddReferences_single(server, session, &newItem);
    } else if(rd->nodeClass == UA_NODECLASS_VARIABLE ||
              rd->nodeClass == UA_NODECLASS_OBJECT) {
        /* Copy the node */
        UA_Node *node = UA_NodeStore_getCopy(server->nodestore, &rd->nodeId.nodeId);
        if(!node)
            return UA_STATUSCODE_BADNODEIDINVALID;

        /* Get the type */
        UA_NodeId typeId;
        getNodeType(server, node, &typeId);

        /* Reset the NodeId (random numeric id will be assigned in the nodestore) */
        UA_NodeId_deleteMembers(&node->nodeId);
        node->nodeId.namespaceIndex = destinationNodeId->namespaceIndex;
                
        /* Add the node to the nodestore */
        retval = UA_NodeStore_insert(server->nodestore, node);
        if(retval == UA_STATUSCODE_GOOD)
            retval = Service_AddNode_finish(server, session, &node->nodeId, destinationNodeId,
                                            &rd->referenceTypeId, &typeId, instantiationCallback);

        /* Clean up */
        UA_NodeId_deleteMembers(&typeId);
    }
    return retval;
}

/* Copy any children of Node sourceNodeId to another node destinationNodeId. */
static UA_StatusCode
copyChildNodes(UA_Server *server, UA_Session *session, 
               const UA_NodeId *sourceNodeId, const UA_NodeId *destinationNodeId, 
               UA_InstantiationCallback *instantiationCallback) {
    /* Browse to get all children of the source */
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = *sourceNodeId;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_AGGREGATES);
    bd.includeSubtypes = true;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.nodeClassMask = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE | UA_NODECLASS_METHOD;
    bd.resultMask = UA_BROWSERESULTMASK_REFERENCETYPEID | UA_BROWSERESULTMASK_NODECLASS |
        UA_BROWSERESULTMASK_BROWSENAME;

    UA_BrowseResult br;
    UA_BrowseResult_init(&br);
    Service_Browse_single(server, session, NULL, &bd, 0, &br);
    if(br.statusCode != UA_STATUSCODE_GOOD)
        return br.statusCode;
  
    /* Copy all children from source to destination */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    for(size_t i = 0; i < br.referencesSize; ++i)
        retval |= copyChildNode(server, session, destinationNodeId, 
                                &br.references[i], instantiationCallback);
    UA_BrowseResult_deleteMembers(&br);
    return retval;
}

/*******************************************/
/* Create nodes from attribute description */
/*******************************************/

static UA_StatusCode
copyStandardAttributes(UA_Node *node, const UA_AddNodesItem *item,
                       const UA_NodeAttributes *attr) {
    UA_StatusCode retval;
    retval  = UA_NodeId_copy(&item->requestedNewNodeId.nodeId, &node->nodeId);
    retval |= UA_QualifiedName_copy(&item->browseName, &node->browseName);
    retval |= UA_LocalizedText_copy(&attr->displayName, &node->displayName);
    retval |= UA_LocalizedText_copy(&attr->description, &node->description);
    node->writeMask = attr->writeMask;
    return retval;
}

static UA_StatusCode
copyCommonVariableAttributes(UA_VariableNode *node,
                             const UA_VariableAttributes *attr) {
    /* Copy the array dimensions */
    UA_StatusCode retval =
        UA_Array_copy(attr->arrayDimensions, attr->arrayDimensionsSize,
                      (void**)&node->arrayDimensions, &UA_TYPES[UA_TYPES_UINT32]);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    node->arrayDimensionsSize = attr->arrayDimensionsSize;

    /* Data type and value rank */
    retval |= UA_NodeId_copy(&attr->dataType, &node->dataType);
    node->valueRank = attr->valueRank;

    /* Copy the value */
    node->valueSource = UA_VALUESOURCE_DATA;
    retval |= UA_Variant_copy(&attr->value, &node->value.data.value.value);
    node->value.data.value.hasValue = true;

    return retval;
}

static UA_StatusCode
copyVariableNodeAttributes(UA_VariableNode *vnode,
                           const UA_VariableAttributes *attr) {
    vnode->accessLevel = attr->accessLevel;
    vnode->historizing = attr->historizing;
    vnode->minimumSamplingInterval = attr->minimumSamplingInterval;
    return copyCommonVariableAttributes(vnode, attr);
}

static UA_StatusCode
copyVariableTypeNodeAttributes(UA_VariableTypeNode *vtnode,
                               const UA_VariableTypeAttributes *attr) {
    vtnode->isAbstract = attr->isAbstract;
    return copyCommonVariableAttributes((UA_VariableNode*)vtnode,
                                        (const UA_VariableAttributes*)attr);
}

static UA_StatusCode
copyObjectNodeAttributes(UA_ObjectNode *onode, const UA_ObjectAttributes *attr) {
    onode->eventNotifier = attr->eventNotifier;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyReferenceTypeNodeAttributes(UA_ReferenceTypeNode *rtnode,
                                const UA_ReferenceTypeAttributes *attr) {
    rtnode->isAbstract = attr->isAbstract;
    rtnode->symmetric = attr->symmetric;
    return UA_LocalizedText_copy(&attr->inverseName, &rtnode->inverseName);
}

static UA_StatusCode
copyObjectTypeNodeAttributes(UA_ObjectTypeNode *otnode,
                             const UA_ObjectTypeAttributes *attr) {
    otnode->isAbstract = attr->isAbstract;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyViewNodeAttributes(UA_ViewNode *vnode, const UA_ViewAttributes *attr) {
    vnode->containsNoLoops = attr->containsNoLoops;
    vnode->eventNotifier = attr->eventNotifier;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyDataTypeNodeAttributes(UA_DataTypeNode *dtnode,
                           const UA_DataTypeAttributes *attr) {
    dtnode->isAbstract = attr->isAbstract;
    return UA_STATUSCODE_GOOD;
}

#define CHECK_ATTRIBUTES(TYPE)                                          \
    if(item->nodeAttributes.content.decoded.type != &UA_TYPES[UA_TYPES_##TYPE]) { \
        retval = UA_STATUSCODE_BADNODEATTRIBUTESINVALID;                \
        break;                                                          \
    }

/* Copy the attributes into a new node. On success, newNode points to the
 * created node */
static UA_StatusCode
createNodeFromAttributes(const UA_AddNodesItem *item, UA_Node **newNode) {
    /* Check that we can read the attributes */
    if(item->nodeAttributes.encoding < UA_EXTENSIONOBJECT_DECODED ||
       !item->nodeAttributes.content.decoded.type)
        return UA_STATUSCODE_BADNODEATTRIBUTESINVALID;

    /* Create the node */
    // todo: error case where the nodeclass is faulty
    void *node = UA_NodeStore_newNode(item->nodeClass);
    if(!node)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Copy the attributes into the node */
    void *data = item->nodeAttributes.content.decoded.data;
    UA_StatusCode retval = copyStandardAttributes(node, item, data);
    switch(item->nodeClass) {
    case UA_NODECLASS_OBJECT:
        CHECK_ATTRIBUTES(OBJECTATTRIBUTES);
        retval |= copyObjectNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VARIABLE:
        CHECK_ATTRIBUTES(VARIABLEATTRIBUTES);
        retval |= copyVariableNodeAttributes(node, data);
        break;
    case UA_NODECLASS_OBJECTTYPE:
        CHECK_ATTRIBUTES(OBJECTTYPEATTRIBUTES);
        retval |= copyObjectTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VARIABLETYPE:
        CHECK_ATTRIBUTES(VARIABLETYPEATTRIBUTES);
        retval |= copyVariableTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_REFERENCETYPE:
        CHECK_ATTRIBUTES(REFERENCETYPEATTRIBUTES);
        retval |= copyReferenceTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_DATATYPE:
        CHECK_ATTRIBUTES(DATATYPEATTRIBUTES);
        retval |= copyDataTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VIEW:
        CHECK_ATTRIBUTES(VIEWATTRIBUTES);
        retval |= copyViewNodeAttributes(node, data);
        break;
    case UA_NODECLASS_METHOD:
    case UA_NODECLASS_UNSPECIFIED:
    default:
        retval = UA_STATUSCODE_BADNODECLASSINVALID;
    }

    if(retval == UA_STATUSCODE_GOOD)
        *newNode = node;
    else
        UA_NodeStore_deleteNode(node);

    return retval;
}

/************/
/* Add Node */
/************/

static void
Service_AddNode_begin(UA_Server *server, UA_Session *session,
                      const UA_AddNodesItem *item, UA_AddNodesResult *result) {
    /* Check the namespaceindex */
    if(item->requestedNewNodeId.nodeId.namespaceIndex >= server->namespacesSize) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Namespace invalid");
        result->statusCode = UA_STATUSCODE_BADNODEIDINVALID;
        return;
    }

    /* Add the node to the nodestore */
    UA_Node *node = NULL;
    result->statusCode = createNodeFromAttributes(item, &node);
    if(result->statusCode == UA_STATUSCODE_GOOD)
        result->statusCode = UA_NodeStore_insert(server->nodestore, node);
    if(result->statusCode != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Node could not be added to the nodestore "
                             "with error code %s", UA_StatusCode_name(result->statusCode));
        return;
    }

    /* Copy the nodeid of the new node */
    result->statusCode = UA_NodeId_copy(&node->nodeId, &result->addedNodeId);
    if(result->statusCode != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Could not copy the nodeid");
        Service_DeleteNodes_single(server, &adminSession, &node->nodeId, true);
    }
}

static UA_StatusCode
Service_AddNode_finish(UA_Server *server, UA_Session *session,
                       const UA_NodeId *nodeId, const UA_NodeId *parentNodeId,
                       const UA_NodeId *referenceTypeId, const UA_NodeId *typeDefinition,
                       UA_InstantiationCallback *instantiationCallback) {
    /* Get the node */
    const UA_Node *node = UA_NodeStore_get(server->nodestore, nodeId);
    if(!node)
        return UA_STATUSCODE_BADNODEIDUNKNOWN;

    /* Use the typeDefinition as parent for type-nodes */
    const UA_NodeId hasSubtype = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
    if(node->nodeClass == UA_NODECLASS_VARIABLETYPE ||
       node->nodeClass == UA_NODECLASS_OBJECTTYPE ||
       node->nodeClass == UA_NODECLASS_REFERENCETYPE ||
       node->nodeClass == UA_NODECLASS_DATATYPE) {
        referenceTypeId = &hasSubtype;
        typeDefinition = parentNodeId;
    }

    /* Replace empty typeDefinition with the most permissive default */
    const UA_NodeId baseDataVariableType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    const UA_NodeId baseObjectType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE);
    if((node->nodeClass == UA_NODECLASS_VARIABLE ||
        node->nodeClass == UA_NODECLASS_OBJECT) &&
       UA_NodeId_isNull(typeDefinition)) {
        if(node->nodeClass == UA_NODECLASS_VARIABLE)
            typeDefinition = &baseDataVariableType;
        else
            typeDefinition = &baseObjectType;
    }

    /* Check parent reference. Objects may have no parent. */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(node->nodeClass != UA_NODECLASS_OBJECT || !UA_NodeId_isNull(parentNodeId) ||
       !UA_NodeId_isNull(referenceTypeId)) {
        retval = checkParentReference(server, session, node->nodeClass,
                                      parentNodeId, referenceTypeId);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "AddNodes: The parent reference is invalid");
            goto cleanup;
        }
    }
    
    /* Type check node */
    if(node->nodeClass == UA_NODECLASS_VARIABLE ||
       node->nodeClass == UA_NODECLASS_VARIABLETYPE) {
        retval = typeCheckVariableNode(server, session, (const UA_VariableNode*)node,
                                       typeDefinition);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "AddNodes: Type checking failed with error code %s",
                                UA_StatusCode_name(retval));
            goto cleanup;
        }
    }

    /* Add parent reference */
    if(!UA_NodeId_isNull(parentNodeId)) {
        UA_AddReferencesItem ref_item;
        UA_AddReferencesItem_init(&ref_item);
        ref_item.sourceNodeId = node->nodeId;
        ref_item.referenceTypeId = *referenceTypeId;
        ref_item.isForward = false;
        ref_item.targetNodeId.nodeId = *parentNodeId;
        retval = Service_AddReferences_single(server, session, &ref_item);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "AddNodes: Adding reference to parent failed");
            goto cleanup;
        }
    }

    /* Instantiate node */
    retval = instantiateNode(server, session, nodeId, node->nodeClass,
                             typeDefinition, instantiationCallback);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Node instantiation failed "
                            "with code %s", UA_StatusCode_name(retval));
        goto cleanup;
    }

    return UA_STATUSCODE_GOOD;

 cleanup:
    Service_DeleteNodes_single(server, &adminSession, nodeId, true);
    return retval;
}

static void
Service_AddNodes_single(UA_Server *server, UA_Session *session,
                        const UA_AddNodesItem *item, UA_AddNodesResult *result,
                        UA_InstantiationCallback *instantiationCallback) {
    /* AddNodes_begin */
    Service_AddNode_begin(server, session, item, result);
    if(result->statusCode != UA_STATUSCODE_GOOD)
        return;

    /* AddNodes_finish */
    result->statusCode = Service_AddNode_finish(server, session, &result->addedNodeId,
                                                &item->parentNodeId.nodeId, &item->referenceTypeId,
                                                &item->typeDefinition.nodeId, instantiationCallback);
    if(result->statusCode != UA_STATUSCODE_GOOD)
        UA_NodeId_deleteMembers(&result->addedNodeId);
}

void Service_AddNodes(UA_Server *server, UA_Session *session,
                      const UA_AddNodesRequest *request,
                      UA_AddNodesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing AddNodesRequest");
    if(request->nodesToAddSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }
    size_t size = request->nodesToAddSize;

    response->results = UA_Array_new(size, &UA_TYPES[UA_TYPES_ADDNODESRESULT]);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
#ifdef _MSC_VER
    UA_Boolean *isExternal = UA_alloca(size);
    UA_UInt32 *indices = UA_alloca(sizeof(UA_UInt32)*size);
#else
    UA_Boolean isExternal[size];
    UA_UInt32 indices[size];
#endif
    memset(isExternal, false, sizeof(UA_Boolean) * size);
    for(size_t j = 0; j <server->externalNamespacesSize; ++j) {
        size_t indexSize = 0;
        for(size_t i = 0;i < size;++i) {
            if(request->nodesToAdd[i].requestedNewNodeId.nodeId.namespaceIndex !=
               server->externalNamespaces[j].index)
                continue;
            isExternal[i] = true;
            indices[indexSize] = (UA_UInt32)i;
            ++indexSize;
        }
        if(indexSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->addNodes(ens->ensHandle, &request->requestHeader,
                      request->nodesToAdd, indices, (UA_UInt32)indexSize,
                      response->results, response->diagnosticInfos);
    }
#endif

    response->resultsSize = size;
    for(size_t i = 0; i < size; ++i) {
#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
        if(!isExternal[i])
#endif
            Service_AddNodes_single(server, session, &request->nodesToAdd[i],
                                    &response->results[i], NULL);
    }
}

UA_StatusCode
__UA_Server_addNode(UA_Server *server, const UA_NodeClass nodeClass,
                    const UA_NodeId *requestedNewNodeId,
                    const UA_NodeId *parentNodeId,
                    const UA_NodeId *referenceTypeId,
                    const UA_QualifiedName *browseName,
                    const UA_NodeId *typeDefinition,
                    const UA_NodeAttributes *attributes,
                    const UA_DataType *attributesType,
                    UA_InstantiationCallback *instantiationCallback,
                    UA_NodeId *outNewNodeId) {
    /* Create the AddNodesItem */
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = *requestedNewNodeId;
    item.browseName = *browseName;
    item.nodeClass = nodeClass;
    item.parentNodeId.nodeId = *parentNodeId;
    item.referenceTypeId = *referenceTypeId;
    item.typeDefinition.nodeId = *typeDefinition;
    item.nodeAttributes = (UA_ExtensionObject) {
        .encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE,
        .content.decoded = {attributesType, (void*)(uintptr_t)attributes}};

    /* Call the normal addnodes service */
    UA_AddNodesResult result;
    UA_AddNodesResult_init(&result);
    Service_AddNodes_single(server, &adminSession,
                            &item, &result, instantiationCallback);
    if(outNewNodeId)
        *outNewNodeId = result.addedNodeId;
    else
        UA_NodeId_deleteMembers(&result.addedNodeId);
    return result.statusCode;
}

UA_StatusCode
__UA_Server_addNode_begin(UA_Server *server, const UA_NodeClass nodeClass,
                          const UA_NodeId *requestedNewNodeId,
                          const UA_QualifiedName *browseName,
                          const UA_NodeAttributes *attr,
                          const UA_DataType *attributeType,
                          UA_NodeId *outNewNodeId) {
    /* Create the item */
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = *requestedNewNodeId;
    item.browseName = *browseName;
    item.nodeClass = nodeClass;
    item.nodeAttributes = (UA_ExtensionObject){
        .encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE,
        .content.decoded = {attributeType, (void*)(uintptr_t)attr}};

    /* Add the node without checks or instantiation */
    UA_AddNodesResult result;
    UA_AddNodesResult_init(&result);
    Service_AddNode_begin(server, &adminSession, &item, &result);
    if(outNewNodeId)
        *outNewNodeId = result.addedNodeId;
    else
        UA_NodeId_deleteMembers(&result.addedNodeId);
    return result.statusCode;
}

UA_StatusCode
__UA_Server_addNode_finish(UA_Server *server, const UA_NodeId *nodeId,
                           const UA_NodeId *parentNodeId,
                           const UA_NodeId *referenceTypeId,
                           const UA_NodeId *typeDefinition,
                           UA_InstantiationCallback *instantiationCallback) {
    return Service_AddNode_finish(server, &adminSession, nodeId, parentNodeId,
                                  referenceTypeId, typeDefinition, instantiationCallback);
}

/**************************************************/
/* Add Special Nodes (not possible over the wire) */
/**************************************************/

#ifdef UA_ENABLE_METHODCALLS

UA_StatusCode
UA_Server_addMethodNode_begin(UA_Server *server, const UA_NodeId requestedNewNodeId,
                              const UA_QualifiedName browseName,
                              const UA_MethodAttributes attr, UA_MethodCallback method, void *handle,
                              UA_NodeId *outNewNodeId) {
    /* Create the node */
    UA_MethodNode *node = (UA_MethodNode*)UA_NodeStore_newNode(UA_NODECLASS_METHOD);
    if(!node)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Set the node attributes */
    node->executable = attr.executable;
    node->attachedMethod = method;
    node->methodHandle = handle;
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = requestedNewNodeId;
    item.browseName = browseName;
    UA_StatusCode retval =
        copyStandardAttributes((UA_Node*)node, &item, (const UA_NodeAttributes*)&attr);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_NodeStore_deleteNode((UA_Node*)node);
        return retval;
    }

    /* Add the node to the nodestore */
    UA_RCU_LOCK();
    retval = UA_NodeStore_insert(server->nodestore, (UA_Node*)node);
    if(outNewNodeId) {
        retval = UA_NodeId_copy(&node->nodeId, outNewNodeId);
        if(retval != UA_STATUSCODE_GOOD)
            UA_NodeStore_remove(server->nodestore, &node->nodeId);
    }
    UA_RCU_UNLOCK();
    return retval;
}

UA_StatusCode
UA_Server_addMethodNode_finish(UA_Server *server, const UA_NodeId nodeId,
                               const UA_NodeId parentNodeId, const UA_NodeId referenceTypeId,
                               size_t inputArgumentsSize, const UA_Argument* inputArguments,
                               size_t outputArgumentsSize, const UA_Argument* outputArguments) {
    UA_NodeId inputArgsId = UA_NODEID_NULL;
    UA_NodeId outputArgsId = UA_NODEID_NULL;

    const UA_QualifiedName inputArgsName = UA_QUALIFIEDNAME(0, "InputArguments");
    const UA_QualifiedName outputArgsName = UA_QUALIFIEDNAME(0, "OutputArguments");

    const UA_NodeId hasproperty = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
    const UA_NodeId propertytype = UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE);
    const UA_NodeId argsId = UA_NODEID_NUMERIC(nodeId.namespaceIndex, 0);

    /* Browse to see which argument nodes exist */
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = nodeId;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
    bd.includeSubtypes = false;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.nodeClassMask = UA_NODECLASS_VARIABLE;
    bd.resultMask = UA_BROWSERESULTMASK_BROWSENAME;
    
    UA_BrowseResult br;
    UA_BrowseResult_init(&br);
    Service_Browse_single(server, &adminSession, NULL, &bd, 0, &br);
    UA_StatusCode retval = br.statusCode;
    if(retval != UA_STATUSCODE_GOOD)
        goto finish;

    for(size_t i = 0; i < br.referencesSize; i++) {
        UA_ReferenceDescription *rd = &br.references[i];
        if(rd->browseName.namespaceIndex == 0 &&
           UA_String_equal(&rd->browseName.name, &inputArgsName.name))
            inputArgsId = rd->nodeId.nodeId;
        else if(rd->browseName.namespaceIndex == 0 &&
                UA_String_equal(&rd->browseName.name, &outputArgsName.name))
            outputArgsId = rd->nodeId.nodeId;
    }

    /* Add the Input Arguments VariableNode */
    if(inputArgumentsSize > 0 && !UA_NodeId_isNull(&inputArgsId)) {
        UA_VariableAttributes inputargs;
        UA_VariableAttributes_init(&inputargs);
        inputargs.displayName = UA_LOCALIZEDTEXT("en_US", "InputArguments");
        /* UAExpert creates a monitoreditem on inputarguments ... */
        inputargs.minimumSamplingInterval = 100000.0f;
        inputargs.valueRank = 1;
        inputargs.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
        /* dirty-cast, but is treated as const ... */
        UA_Variant_setArray(&inputargs.value, (void*)(uintptr_t)inputArguments, inputArgumentsSize,
                            &UA_TYPES[UA_TYPES_ARGUMENT]);
        retval = UA_Server_addVariableNode(server, argsId, nodeId, hasproperty, inputArgsName,
                                           propertytype, inputargs, NULL, &inputArgsId);
    }

    /* Add the Output Arguments VariableNode */
    if(outputArgumentsSize > 0 && !UA_NodeId_isNull(&outputArgsId)) {
        UA_VariableAttributes outputargs;
        UA_VariableAttributes_init(&outputargs);
        outputargs.displayName = UA_LOCALIZEDTEXT("en_US", "OutputArguments");
        /* UAExpert creates a monitoreditem on outputarguments ... */
        outputargs.minimumSamplingInterval = 100000.0f;
        outputargs.valueRank = 1;
        outputargs.dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
        /* dirty-cast, but is treated as const ... */
        UA_Variant_setArray(&outputargs.value, (void*)(uintptr_t)outputArguments, outputArgumentsSize,
                            &UA_TYPES[UA_TYPES_ARGUMENT]);
        retval |= UA_Server_addVariableNode(server, argsId, nodeId, hasproperty, outputArgsName,
                                           propertytype, outputargs, NULL, &outputArgsId);
    }

    /* Call finish to add the parent reference */
    retval |= Service_AddNode_finish(server, &adminSession, &nodeId, &parentNodeId,
                                     &referenceTypeId, &UA_NODEID_NULL, NULL);

 finish:
    if(retval != UA_STATUSCODE_GOOD) {
        Service_DeleteNodes_single(server, &adminSession, &nodeId, true);
        Service_DeleteNodes_single(server, &adminSession, &inputArgsId, true);
        Service_DeleteNodes_single(server, &adminSession, &outputArgsId, true);
    }
    UA_BrowseResult_deleteMembers(&br);
    return retval;
}

UA_StatusCode
UA_Server_addMethodNode(UA_Server *server, const UA_NodeId requestedNewNodeId,
                        const UA_NodeId parentNodeId, const UA_NodeId referenceTypeId,
                        const UA_QualifiedName browseName, const UA_MethodAttributes attr,
                        UA_MethodCallback method, void *handle,
                        size_t inputArgumentsSize, const UA_Argument* inputArguments, 
                        size_t outputArgumentsSize, const UA_Argument* outputArguments,
                        UA_NodeId *outNewNodeId) {
    /* Call begin */
    UA_StatusCode retval = UA_Server_addMethodNode_begin(server, requestedNewNodeId,
                                                         browseName, attr, method,
                                                         handle, outNewNodeId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Ensure the correct nodeid is used */
    const UA_NodeId *newId = &requestedNewNodeId;
    if(outNewNodeId)
        newId = outNewNodeId;

    /* Call finish */
    return UA_Server_addMethodNode_finish(server, *newId, parentNodeId, referenceTypeId,
                                          inputArgumentsSize, inputArguments,
                                          outputArgumentsSize, outputArguments);
}

#endif

/******************/
/* Add References */
/******************/

static UA_StatusCode
deleteOneWayReference(UA_Server *server, UA_Session *session, UA_Node *node,
                      const UA_DeleteReferencesItem *item);

/* Adds a one-way reference to the local nodestore */
static UA_StatusCode
addOneWayReference(UA_Server *server, UA_Session *session,
                   UA_Node *node, const UA_AddReferencesItem *item) {
    /* Increase the array size */
    size_t i = node->referencesSize;
    size_t refssize = (i+1) | 3; // so the realloc is not necessary every time
    UA_ReferenceNode *new_refs = UA_realloc(node->references,
                                            sizeof(UA_ReferenceNode) * refssize);
    if(!new_refs)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Add the new reference */
    node->references = new_refs;
    UA_ReferenceNode_init(&new_refs[i]);
    UA_StatusCode retval = UA_NodeId_copy(&item->referenceTypeId,
                                          &new_refs[i].referenceTypeId);
    retval |= UA_ExpandedNodeId_copy(&item->targetNodeId, &new_refs[i].targetId);
    new_refs[i].isInverse = !item->isForward;

    /* Increase the array size counter or clean up */
    if(retval == UA_STATUSCODE_GOOD)
        node->referencesSize = i+1;
    else
        UA_ReferenceNode_deleteMembers(&new_refs[i]);
    return retval;
}

UA_StatusCode
Service_AddReferences_single(UA_Server *server, UA_Session *session,
                             const UA_AddReferencesItem *item) {
    /* Currently no expandednodeids are allowed */
    if(item->targetServerUri.length > 0)
        return UA_STATUSCODE_BADNOTIMPLEMENTED;

    /* Add the first direction */
#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    UA_StatusCode retval =
        UA_Server_editNode(server, session, &item->sourceNodeId,
                           (UA_EditNodeCallback)addOneWayReference, item);
#else
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_Boolean handledExternally = UA_FALSE;
    for(size_t j = 0; j <server->externalNamespacesSize; ++j) {
        if(item->sourceNodeId.namespaceIndex != server->externalNamespaces[j].index) {
            continue;
        } else {
            UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
            retval = (UA_StatusCode)ens->addOneWayReference(ens->ensHandle, item);
            handledExternally = UA_TRUE;
            break;
        }
    }
    if(!handledExternally)
        retval = UA_Server_editNode(server, session, &item->sourceNodeId,
                                    (UA_EditNodeCallback)addOneWayReference, item);
#endif

    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Add the second direction */
    UA_AddReferencesItem secondItem;
    UA_AddReferencesItem_init(&secondItem);
    secondItem.sourceNodeId = item->targetNodeId.nodeId;
    secondItem.referenceTypeId = item->referenceTypeId;
    secondItem.isForward = !item->isForward;
    secondItem.targetNodeId.nodeId = item->sourceNodeId;
    /* keep default secondItem.targetNodeClass = UA_NODECLASS_UNSPECIFIED */
#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    retval = UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                                (UA_EditNodeCallback)addOneWayReference, &secondItem);
#else
    handledExternally = UA_FALSE;
    for(size_t j = 0; j < server->externalNamespacesSize; ++j) {
        if(secondItem.sourceNodeId.namespaceIndex != server->externalNamespaces[j].index) {
            continue;
        } else {
            UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
            retval = (UA_StatusCode)ens->addOneWayReference(ens->ensHandle, &secondItem);
            handledExternally = UA_TRUE;
            break;
        }
    }
    if(!handledExternally)
        retval = UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                                    (UA_EditNodeCallback)addOneWayReference, &secondItem);
#endif

    /* remove reference if the second direction failed */
    if(retval != UA_STATUSCODE_GOOD) {
        UA_DeleteReferencesItem deleteItem;
        deleteItem.sourceNodeId = item->sourceNodeId;
        deleteItem.referenceTypeId = item->referenceTypeId;
        deleteItem.isForward = item->isForward;
        deleteItem.targetNodeId = item->targetNodeId;
        deleteItem.deleteBidirectional = false;
        /* ignore returned status code */
        UA_Server_editNode(server, session, &item->sourceNodeId,
                           (UA_EditNodeCallback)deleteOneWayReference, &deleteItem);
    }
    return retval;
}

void Service_AddReferences(UA_Server *server, UA_Session *session,
                           const UA_AddReferencesRequest *request,
                           UA_AddReferencesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing AddReferencesRequest");
    if(request->referencesToAddSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->referencesToAddSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->referencesToAddSize;

#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    for(size_t i = 0; i < response->resultsSize; ++i)
        response->results[i] =
            Service_AddReferences_single(server, session, &request->referencesToAdd[i]);
#else
    size_t size = request->referencesToAddSize;
# ifdef NO_ALLOCA
    UA_Boolean isExternal[size];
    UA_UInt32 indices[size];
# else
    UA_Boolean *isExternal = UA_alloca(sizeof(UA_Boolean) * size);
    UA_UInt32 *indices = UA_alloca(sizeof(UA_UInt32) * size);
# endif /*NO_ALLOCA */
    memset(isExternal, false, sizeof(UA_Boolean) * size);
    for(size_t j = 0; j < server->externalNamespacesSize; ++j) {
        size_t indicesSize = 0;
        for(size_t i = 0;i < size;++i) {
            if(request->referencesToAdd[i].sourceNodeId.namespaceIndex
               != server->externalNamespaces[j].index)
                continue;
            isExternal[i] = true;
            indices[indicesSize] = (UA_UInt32)i;
            ++indicesSize;
        }
        if (indicesSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->addReferences(ens->ensHandle, &request->requestHeader, request->referencesToAdd,
                           indices, (UA_UInt32)indicesSize, response->results, response->diagnosticInfos);
    }

    for(size_t i = 0; i < response->resultsSize; ++i) {
        if(!isExternal[i])
            response->results[i] =
                Service_AddReferences_single(server, session, &request->referencesToAdd[i]);
    }
#endif
}

UA_StatusCode
UA_Server_addReference(UA_Server *server, const UA_NodeId sourceId,
                       const UA_NodeId refTypeId,
                       const UA_ExpandedNodeId targetId,
                       UA_Boolean isForward) {
    UA_AddReferencesItem item;
    UA_AddReferencesItem_init(&item);
    item.sourceNodeId = sourceId;
    item.referenceTypeId = refTypeId;
    item.isForward = isForward;
    item.targetNodeId = targetId;
    UA_RCU_LOCK();
    UA_StatusCode retval = Service_AddReferences_single(server, &adminSession, &item);
    UA_RCU_UNLOCK();
    return retval;
}

/****************/
/* Delete Nodes */
/****************/

UA_StatusCode
Service_DeleteNodes_single(UA_Server *server, UA_Session *session,
                           const UA_NodeId *nodeId,
                           UA_Boolean deleteReferences) {
    const UA_Node *node = UA_NodeStore_get(server->nodestore, nodeId);
    if(!node)
        return UA_STATUSCODE_BADNODEIDUNKNOWN;

    /* destroy an object before removing it */
    if(node->nodeClass == UA_NODECLASS_OBJECT) {
        /* find the object type(s) */
        UA_BrowseDescription bd;
        UA_BrowseDescription_init(&bd);
        bd.browseDirection = UA_BROWSEDIRECTION_INVERSE;
        bd.nodeId = *nodeId;
        bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
        bd.includeSubtypes = true;
        bd.nodeClassMask = UA_NODECLASS_OBJECTTYPE;

        /* browse type definitions with admin rights */
        UA_BrowseResult result;
        UA_BrowseResult_init(&result);
        Service_Browse_single(server, &adminSession, NULL, &bd, 0, &result);
        for(size_t i = 0; i < result.referencesSize; ++i) {
            /* call the destructor */
            UA_ReferenceDescription *rd = &result.references[i];
            const UA_ObjectTypeNode *typenode =
                (const UA_ObjectTypeNode*)UA_NodeStore_get(server->nodestore, &rd->nodeId.nodeId);
            if(!typenode)
                continue;
            if(typenode->nodeClass != UA_NODECLASS_OBJECTTYPE || !typenode->lifecycleManagement.destructor)
                continue;

            /* if there are several types with lifecycle management, call all the destructors */
            typenode->lifecycleManagement.destructor(*nodeId, ((const UA_ObjectNode*)node)->instanceHandle);
        }
        UA_BrowseResult_deleteMembers(&result);
    }

    /* remove references */
    /* TODO: check if consistency is violated */
    if(deleteReferences == true) { 
        for(size_t i = 0; i < node->referencesSize; ++i) {
            UA_DeleteReferencesItem item;
            UA_DeleteReferencesItem_init(&item);
            item.isForward = node->references[i].isInverse;
            item.sourceNodeId = node->references[i].targetId.nodeId;
            item.targetNodeId.nodeId = node->nodeId;
            UA_Server_editNode(server, session, &node->references[i].targetId.nodeId,
                               (UA_EditNodeCallback)deleteOneWayReference, &item);
        }
    }

    return UA_NodeStore_remove(server->nodestore, nodeId);
}

void Service_DeleteNodes(UA_Server *server, UA_Session *session,
                         const UA_DeleteNodesRequest *request,
                         UA_DeleteNodesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing DeleteNodesRequest");
    if(request->nodesToDeleteSize == 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->nodesToDeleteSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;;
        return;
    }
    response->resultsSize = request->nodesToDeleteSize;

    for(size_t i = 0; i < request->nodesToDeleteSize; ++i) {
        UA_DeleteNodesItem *item = &request->nodesToDelete[i];
        response->results[i] =
            Service_DeleteNodes_single(server, session, &item->nodeId,
                                       item->deleteTargetReferences);
    }
}

UA_StatusCode
UA_Server_deleteNode(UA_Server *server, const UA_NodeId nodeId,
                     UA_Boolean deleteReferences) {
    UA_RCU_LOCK();
    UA_StatusCode retval = Service_DeleteNodes_single(server, &adminSession,
                                                      &nodeId, deleteReferences);
    UA_RCU_UNLOCK();
    return retval;
}

/*********************/
/* Delete References */
/*********************/

// TODO: Check consistency constraints, remove the references.
static UA_StatusCode
deleteOneWayReference(UA_Server *server, UA_Session *session, UA_Node *node,
                      const UA_DeleteReferencesItem *item) {
    UA_Boolean edited = false;
    for(size_t i = node->referencesSize-1; ; --i) {
        if(i > node->referencesSize)
            break; /* underflow after i == 0 */
        if(!UA_NodeId_equal(&item->targetNodeId.nodeId,
                            &node->references[i].targetId.nodeId))
            continue;
        if(!UA_NodeId_equal(&item->referenceTypeId,
                            &node->references[i].referenceTypeId))
            continue;
        if(item->isForward == node->references[i].isInverse)
            continue;
        /* move the last entry to override the current position */
        UA_ReferenceNode_deleteMembers(&node->references[i]);
        node->references[i] = node->references[node->referencesSize-1];
        --node->referencesSize;
        edited = true;
        break;
    }
    if(!edited)
        return UA_STATUSCODE_UNCERTAINREFERENCENOTDELETED;
    /* we removed the last reference */
    if(node->referencesSize == 0 && node->references) {
        UA_free(node->references);
        node->references = NULL;
    }
    return UA_STATUSCODE_GOOD;;
}

UA_StatusCode
Service_DeleteReferences_single(UA_Server *server, UA_Session *session,
                                const UA_DeleteReferencesItem *item) {
    UA_StatusCode retval = UA_Server_editNode(server, session, &item->sourceNodeId,
                                              (UA_EditNodeCallback)deleteOneWayReference, item);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    if(!item->deleteBidirectional || item->targetNodeId.serverIndex != 0)
        return retval;
    UA_DeleteReferencesItem secondItem;
    UA_DeleteReferencesItem_init(&secondItem);
    secondItem.isForward = !item->isForward;
    secondItem.sourceNodeId = item->targetNodeId.nodeId;
    secondItem.targetNodeId.nodeId = item->sourceNodeId;
    return UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                              (UA_EditNodeCallback)deleteOneWayReference, &secondItem);
}

void
Service_DeleteReferences(UA_Server *server, UA_Session *session,
                         const UA_DeleteReferencesRequest *request,
                         UA_DeleteReferencesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing DeleteReferencesRequest");
    if(request->referencesToDeleteSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->referencesToDeleteSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;;
        return;
    }
    response->resultsSize = request->referencesToDeleteSize;

    for(size_t i = 0; i < request->referencesToDeleteSize; ++i)
        response->results[i] =
            Service_DeleteReferences_single(server, session, &request->referencesToDelete[i]);
}

UA_StatusCode
UA_Server_deleteReference(UA_Server *server, const UA_NodeId sourceNodeId,
                          const UA_NodeId referenceTypeId, UA_Boolean isForward,
                          const UA_ExpandedNodeId targetNodeId,
                          UA_Boolean deleteBidirectional) {
    UA_DeleteReferencesItem item;
    item.sourceNodeId = sourceNodeId;
    item.referenceTypeId = referenceTypeId;
    item.isForward = isForward;
    item.targetNodeId = targetNodeId;
    item.deleteBidirectional = deleteBidirectional;
    UA_RCU_LOCK();
    UA_StatusCode retval =
        Service_DeleteReferences_single(server, &adminSession, &item);
    UA_RCU_UNLOCK();
    return retval;
}

/**********************/
/* Set Value Callback */
/**********************/

static UA_StatusCode
setValueCallback(UA_Server *server, UA_Session *session,
                 UA_VariableNode *node, UA_ValueCallback *callback) {
    if(node->nodeClass != UA_NODECLASS_VARIABLE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    node->value.data.callback = *callback;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setVariableNode_valueCallback(UA_Server *server, const UA_NodeId nodeId,
                                        const UA_ValueCallback callback) {
    UA_RCU_LOCK();
    UA_StatusCode retval =
        UA_Server_editNode(server, &adminSession, &nodeId,
                           (UA_EditNodeCallback)setValueCallback, &callback);
    UA_RCU_UNLOCK();
    return retval;
}

/******************/
/* Set DataSource */
/******************/

static UA_StatusCode
setDataSource(UA_Server *server, UA_Session *session,
              UA_VariableNode* node, UA_DataSource *dataSource) {
    if(node->nodeClass != UA_NODECLASS_VARIABLE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    if(node->valueSource == UA_VALUESOURCE_DATA)
        UA_DataValue_deleteMembers(&node->value.data.value);
    node->value.dataSource = *dataSource;
    node->valueSource = UA_VALUESOURCE_DATASOURCE;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Server_setVariableNode_dataSource(UA_Server *server, const UA_NodeId nodeId,
                                     const UA_DataSource dataSource) {
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession, &nodeId,
                                              (UA_EditNodeCallback)setDataSource, &dataSource);
    UA_RCU_UNLOCK();
    return retval;
}

/****************************/
/* Set Lifecycle Management */
/****************************/

static UA_StatusCode
setOLM(UA_Server *server, UA_Session *session,
       UA_ObjectTypeNode* node, UA_ObjectLifecycleManagement *olm) {
    if(node->nodeClass != UA_NODECLASS_OBJECTTYPE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    node->lifecycleManagement = *olm;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setObjectTypeNode_lifecycleManagement(UA_Server *server, UA_NodeId nodeId,
                                                UA_ObjectLifecycleManagement olm) {
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession, &nodeId,
                                              (UA_EditNodeCallback)setOLM, &olm);
    UA_RCU_UNLOCK();
    return retval;
}

/***********************/
/* Set Method Callback */
/***********************/

#ifdef UA_ENABLE_METHODCALLS

struct addMethodCallback {
    UA_MethodCallback callback;
    void *handle;
};

static UA_StatusCode
editMethodCallback(UA_Server *server, UA_Session* session,
                   UA_Node* node, const void* handle) {
    if(node->nodeClass != UA_NODECLASS_METHOD)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    const struct addMethodCallback *newCallback = handle;
    UA_MethodNode *mnode = (UA_MethodNode*) node;
    mnode->attachedMethod = newCallback->callback;
    mnode->methodHandle   = newCallback->handle;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setMethodNode_callback(UA_Server *server, const UA_NodeId methodNodeId,
                                 UA_MethodCallback method, void *handle) {
    struct addMethodCallback cb = { method, handle };
    UA_RCU_LOCK();
    UA_StatusCode retval =
        UA_Server_editNode(server, &adminSession,
                           &methodNodeId, editMethodCallback, &cb);
    UA_RCU_UNLOCK();
    return retval;
}

#endif
