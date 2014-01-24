//
//  AvatarMixer.cpp
//  hifi
//
//  Created by Stephen Birarda on 9/5/13.
//  Copyright (c) 2013 HighFidelity, Inc. All rights reserved.
//
//  Original avatar-mixer main created by Leonardo Murillo on 03/25/13.
//
//  The avatar mixer receives head, hand and positional data from all connected
//  nodes, and broadcasts that data back to them, every BROADCAST_INTERVAL ms.

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>

#include <Logging.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>
#include <UUID.h>

#include "AvatarData.h"

#include "AvatarMixer.h"

const char AVATAR_MIXER_LOGGING_NAME[] = "avatar-mixer";

const unsigned int AVATAR_DATA_SEND_INTERVAL_USECS = (1 / 60.0) * 1000 * 1000;

AvatarMixer::AvatarMixer(const unsigned char* dataBuffer, int numBytes) :
    ThreadedAssignment(dataBuffer, numBytes)
{
    // make sure we hear about node kills so we can tell the other nodes
    connect(NodeList::getInstance(), &NodeList::nodeKilled, this, &AvatarMixer::nodeKilled);
}

unsigned char* addNodeToBroadcastPacket(unsigned char *currentPosition, Node *nodeToAdd) {
    QByteArray rfcUUID = nodeToAdd->getUUID().toRfc4122();
    memcpy(currentPosition, rfcUUID.constData(), rfcUUID.size());
    currentPosition += rfcUUID.size();
    
    AvatarData *nodeData = (AvatarData *)nodeToAdd->getLinkedData();
    currentPosition += nodeData->getBroadcastData(currentPosition);
    
    return currentPosition;
}

void attachAvatarDataToNode(Node* newNode) {
    if (newNode->getLinkedData() == NULL) {
        newNode->setLinkedData(new AvatarData(newNode));
    }
}

// NOTE: some additional optimizations to consider.
//    1) use the view frustum to cull those avatars that are out of view. Since avatar data doesn't need to be present
//       if the avatar is not in view or in the keyhole.
//    2) after culling for view frustum, sort order the avatars by distance, send the closest ones first.
//    3) if we need to rate limit the amount of data we send, we can use a distance weighted "semi-random" function to
//       determine which avatars are included in the packet stream
//    4) we should optimize the avatar data format to be more compact (100 bytes is pretty wasteful).
void broadcastAvatarData() {
    static unsigned char broadcastPacket[MAX_PACKET_SIZE];
    static unsigned char avatarDataBuffer[MAX_PACKET_SIZE];
    int numHeaderBytes = populateTypeAndVersion(broadcastPacket, PACKET_TYPE_BULK_AVATAR_DATA);
    unsigned char* currentBufferPosition = broadcastPacket + numHeaderBytes;
    int packetLength = currentBufferPosition - broadcastPacket;
    int packetsSent = 0;
    
    NodeList* nodeList = NodeList::getInstance();
    
    foreach (const SharedNodePointer& node, nodeList->getNodeHash()) {
        if (node->getLinkedData() && node->getType() == NODE_TYPE_AGENT && node->getActiveSocket()) {
            
            // reset packet pointers for this node
            currentBufferPosition = broadcastPacket + numHeaderBytes;
            packetLength = currentBufferPosition - broadcastPacket;
            
            // this is an AGENT we have received head data from
            // send back a packet with other active node data to this node
            foreach (const SharedNodePointer& otherNode, nodeList->getNodeHash()) {
                if (otherNode->getLinkedData() && otherNode->getUUID() != node->getUUID()) {
                    
                    unsigned char* avatarDataEndpoint = addNodeToBroadcastPacket((unsigned char*)&avatarDataBuffer[0],
                                                                                 otherNode.data());
                    int avatarDataLength = avatarDataEndpoint - (unsigned char*)&avatarDataBuffer;
                    
                    if (avatarDataLength + packetLength <= MAX_PACKET_SIZE) {
                        memcpy(currentBufferPosition, &avatarDataBuffer[0], avatarDataLength);
                        packetLength += avatarDataLength;
                        currentBufferPosition += avatarDataLength;
                    } else {
                        packetsSent++;
                        //printf("packetsSent=%d packetLength=%d\n", packetsSent, packetLength);
                        nodeList->getNodeSocket().writeDatagram((char*) broadcastPacket,
                                                                currentBufferPosition - broadcastPacket,
                                                                node->getActiveSocket()->getAddress(),
                                                                node->getActiveSocket()->getPort());
                        
                        // reset the packet
                        currentBufferPosition = broadcastPacket + numHeaderBytes;
                        packetLength = currentBufferPosition - broadcastPacket;
                        
                        // copy the avatar that didn't fit into the next packet
                        memcpy(currentBufferPosition, &avatarDataBuffer[0], avatarDataLength);
                        packetLength += avatarDataLength;
                        currentBufferPosition += avatarDataLength;
                    }
                }
            }
            
            packetsSent++;
            //printf("packetsSent=%d packetLength=%d\n", packetsSent, packetLength);
            nodeList->getNodeSocket().writeDatagram((char*) broadcastPacket, currentBufferPosition - broadcastPacket,
                                                    node->getActiveSocket()->getAddress(),
                                                    node->getActiveSocket()->getPort());
        }
    }
}

void AvatarMixer::nodeKilled(SharedNodePointer killedNode) {
    if (killedNode->getType() == NODE_TYPE_AGENT
        && killedNode->getLinkedData()) {
        // this was an avatar we were sending to other people
        // send a kill packet for it to our other nodes
        unsigned char packetData[MAX_PACKET_SIZE];
        int numHeaderBytes = populateTypeAndVersion(packetData, PACKET_TYPE_KILL_AVATAR);
        
        QByteArray rfcUUID = killedNode->getUUID().toRfc4122();
        memcpy(packetData + numHeaderBytes, rfcUUID.constData(), rfcUUID.size());
        
        NodeList::getInstance()->broadcastToNodes(packetData, numHeaderBytes + NUM_BYTES_RFC4122_UUID,
                                                  QSet<NODE_TYPE>() << NODE_TYPE_AVATAR_MIXER);
    }
}

void AvatarMixer::processDatagram(const QByteArray& dataByteArray, const HifiSockAddr& senderSockAddr) {
    
    NodeList* nodeList = NodeList::getInstance();
    
    switch (dataByteArray[0]) {
        case PACKET_TYPE_HEAD_DATA: {
            QUuid nodeUUID = QUuid::fromRfc4122(dataByteArray.mid(numBytesForPacketHeader((unsigned char*) dataByteArray.data()),
                                                                  NUM_BYTES_RFC4122_UUID));
            
            // add or update the node in our list
            SharedNodePointer avatarNode = nodeList->nodeWithUUID(nodeUUID);
            
            if (avatarNode) {
                // parse positional data from an node
                nodeList->updateNodeWithData(avatarNode.data(), senderSockAddr,
                                             (unsigned char*) dataByteArray.data(), dataByteArray.size());
                
            }
            break;
        }
        case PACKET_TYPE_KILL_AVATAR: {
            nodeList->processKillNode(dataByteArray);
            break;
        }
        default:
            // hand this off to the NodeList
            nodeList->processNodeData(senderSockAddr, (unsigned char*) dataByteArray.data(), dataByteArray.size());
            break;
    }
    
}

void AvatarMixer::run() {
    commonInit(AVATAR_MIXER_LOGGING_NAME, NODE_TYPE_AVATAR_MIXER);
    
    NodeList* nodeList = NodeList::getInstance();
    nodeList->addNodeTypeToInterestSet(NODE_TYPE_AGENT);
    
    nodeList->linkedDataCreateCallback = attachAvatarDataToNode;
    
    int nextFrame = 0;
    timeval startTime;
    
    gettimeofday(&startTime, NULL);
    
    while (!_isFinished) {
        
        QCoreApplication::processEvents();
        
        if (_isFinished) {
            break;
        }
        
        broadcastAvatarData();
        
        int usecToSleep = usecTimestamp(&startTime) + (++nextFrame * AVATAR_DATA_SEND_INTERVAL_USECS) - usecTimestampNow();
        
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        } else {
            qDebug() << "AvatarMixer loop took too" << -usecToSleep << "of extra time. Won't sleep.";
        }
    }
}
