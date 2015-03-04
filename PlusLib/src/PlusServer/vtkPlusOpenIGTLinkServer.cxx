/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/ 

#include "PlusConfigure.h"
#include "TrackedFrame.h"
#include "igtlImageMetaMessage.h"
#include "igtlImageMessage.h"
#include "igtlMessageHeader.h"
#include "igtlPlusClientInfoMessage.h"
#include "igtlStatusMessage.h"
#include "vtkDataCollector.h"
#include "vtkImageData.h"
#include "vtkObjectFactory.h"
#include "vtkPlusChannel.h"
#include "vtkPlusCommandProcessor.h"
#include "vtkPlusIgtlMessageCommon.h"
#include "vtkPlusIgtlMessageFactory.h" 
#include "vtkPlusOpenIGTLinkServer.h"
#include "vtkRecursiveCriticalSection.h"
#include "vtkTrackedFrameList.h"
#include "vtkTransformRepository.h" 
#include "vtkPlusCommand.h"

static const double DELAY_ON_SENDING_ERROR_SEC = 0.02; 
static const double DELAY_ON_NO_NEW_FRAMES_SEC = 0.005; 
static const int CLIENT_SOCKET_TIMEOUT_MSEC = 500; 

//----------------------------------------------------------------------------
// If a frame cannot be retrieved from the device buffers (because it was overwritten by new frames)
// then we skip a SAMPLING_SKIPPING_MARGIN_SEC long period to allow the application to catch up.
// This time should be long enough to comfortably retrieve a frame from the buffer.
static const double SAMPLING_SKIPPING_MARGIN_SEC=0.1;

vtkStandardNewMacro( vtkPlusOpenIGTLinkServer ); 

vtkCxxSetObjectMacro(vtkPlusOpenIGTLinkServer, TransformRepository, vtkTransformRepository);
vtkCxxSetObjectMacro(vtkPlusOpenIGTLinkServer, DataCollector, vtkDataCollector);

namespace
{
  const double CLEAR_PREVIOUS_COMMANDS_TIMEOUT_SEC = 30.0;
  const int IGTL_EMPTY_DATA_SIZE = -1;
}

//----------------------------------------------------------------------------
vtkPlusOpenIGTLinkServer::vtkPlusOpenIGTLinkServer()
: ListeningPort(-1)
, LastSentTrackedFrameTimestamp(0)
, NumberOfRetryAttempts(10) 
, DelayBetweenRetryAttemptsSec(0.100)
, MaxNumberOfIgtlMessagesToSend(100)
, MaxTimeSpentWithProcessingMs(50)
, LastProcessingTimePerFrameMs(-1)
, ConnectionReceiverThreadId(-1)
, DataSenderThreadId(-1)
, DataReceiverThreadId(-1)
, ConnectionActive(std::make_pair(false,false))
, DataSenderActive(std::make_pair(false,false))
, DataReceiverActive(std::make_pair(false,false))
, DataCollector(NULL)
, TransformRepository(NULL)
, Threader(vtkSmartPointer<vtkMultiThreader>::New())
, Mutex(vtkSmartPointer<vtkRecursiveCriticalSection>::New())
, ServerSocket(igtl::ServerSocket::New())
, SendValidTransformsOnly(true)
, IgtlMessageCrcCheckEnabled(0)
, PlusCommandProcessor(vtkSmartPointer<vtkPlusCommandProcessor>::New())
, OutputChannelId(NULL)
, BroadcastChannel(NULL)
, ConfigFilename(NULL)
, GracePeriodLogLevel(vtkPlusLogger::LOG_LEVEL_DEBUG)
, MissingInputGracePeriodSec(0.0)
, BroadcastStartTime(0.0)
{

}

//----------------------------------------------------------------------------
vtkPlusOpenIGTLinkServer::~vtkPlusOpenIGTLinkServer()
{
  this->Stop();
  this->SetTransformRepository(NULL);
  this->SetDataCollector(NULL);
  this->SetConfigFilename(NULL);
}

//----------------------------------------------------------------------------
void vtkPlusOpenIGTLinkServer::PrintSelf( ostream& os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::StartOpenIGTLinkService()
{
  if ( this->DataCollector == NULL )
  {
    LOG_WARNING( "Tried to start OpenIGTLink server without a vtkDataCollector" );
    return PLUS_FAIL;
  }

  if ( this->ConnectionReceiverThreadId < 0 )
  {
    this->ConnectionActive.first = true;
    this->ConnectionReceiverThreadId = this->Threader->SpawnThread( (vtkThreadFunctionType)&ConnectionReceiverThread, this );
    LOG_INFO( "Plus OpenIGTLink server started on port: " << this->ListeningPort ); 
  }

  if ( this->DataSenderThreadId < 0 )
  {
    this->DataSenderActive.first = true;
    this->DataSenderThreadId = this->Threader->SpawnThread( (vtkThreadFunctionType)&DataSenderThread, this );
  }

  if ( this->DataReceiverThreadId < 0 )
  {
    this->DataReceiverActive.first = true; 
    this->DataReceiverThreadId = this->Threader->SpawnThread( (vtkThreadFunctionType)&DataReceiverThread, this );
  }

  if ( !this->DefaultClientInfo.IgtlMessageTypes.empty() )
  {
    std::ostringstream messageTypes;
    for ( int i = 0; i < this->DefaultClientInfo.IgtlMessageTypes.size(); ++i )
    {
      messageTypes << this->DefaultClientInfo.IgtlMessageTypes[i] << " "; 
    }
    LOG_INFO("Server default message types to send: " << messageTypes.str() ); 
  }

  if ( !this->DefaultClientInfo.TransformNames.empty() )
  {
    std::ostringstream transformNames;
    for ( int i = 0; i < this->DefaultClientInfo.TransformNames.size(); ++i )
    {
      std::string tn; 
      this->DefaultClientInfo.TransformNames[i].GetTransformName(tn); 
      transformNames << tn << " "; 
    }
    LOG_INFO("Server default transform names to send: " << transformNames.str() ); 
  }

  if ( !this->DefaultClientInfo.StringNames.empty() )
  {
    std::ostringstream stringNames;
    for ( int i = 0; i < this->DefaultClientInfo.StringNames.size(); ++i )
    {
      stringNames << this->DefaultClientInfo.StringNames[i] << " "; 
    }
    LOG_INFO("Server default string names to send: " << stringNames.str() ); 
  }

  if ( !this->DefaultClientInfo.ImageStreams.empty() )
  {
    std::ostringstream imageNames;
    for ( int i = 0; i < this->DefaultClientInfo.ImageStreams.size(); ++i )
    {
      imageNames << this->DefaultClientInfo.ImageStreams[i].Name << " (EmbeddedTransformToFrame: " << this->DefaultClientInfo.ImageStreams[i].EmbeddedTransformToFrame << ") "; 
    }
    LOG_INFO("Server default images to send: " << imageNames.str() ); 
  }

  this->PlusCommandProcessor->SetPlusServer(this);
  //this->PlusCommandProcessor->Start();

  this->BroadcastStartTime = vtkAccurateTimer::GetSystemTime();

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::StopOpenIGTLinkService()
{

  /*
  // Stop command processor thread 
  if ( this->PlusCommandProcessor->IsRunning() )
  {
  this->PlusCommandProcessor->Stop();
  while ( this->PlusCommandProcessor->IsRunning() )
  {
  // Wait until the thread stops 
  vtkAccurateTimer::Delay( 0.2 ); 
  }
  }
  */

  // Stop data receiver thread 
  if ( this->DataReceiverThreadId >=0 )
  {
    this->DataReceiverActive.first = false; 
    while ( this->DataReceiverActive.second )
    {
      // Wait until the thread stops 
      vtkAccurateTimer::Delay( 0.2 ); 
    }
    this->DataReceiverThreadId = -1; 
  }

  // Stop data sender thread 
  if ( this->DataSenderThreadId >= 0 )
  {
    this->DataSenderActive.first = false; 
    while ( this->DataSenderActive.second )
    {
      // Wait until the thread stops 
      vtkAccurateTimer::Delay( 0.2 ); 
    } 
    this->DataSenderThreadId = -1;
  }

  // Stop connection receiver thread
  if ( this->ConnectionReceiverThreadId >= 0 )
  {
    this->ConnectionActive.first = false;
    while ( this->ConnectionActive.second )
    {
      // Wait until the thread stops 
      vtkAccurateTimer::Delay( 0.2 ); 
    }
    this->ConnectionReceiverThreadId = -1;
    LOG_INFO( "Plus OpenIGTLink server stopped."); 
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::ConnectionReceiverThread( vtkMultiThreader::ThreadInfo* data )
{
  vtkPlusOpenIGTLinkServer* self = (vtkPlusOpenIGTLinkServer*)( data->UserData );

  int r = self->ServerSocket->CreateServer( self->ListeningPort );

  if ( r < 0 )
  {
    LOG_ERROR( "Cannot create a server socket." );
    return NULL;
  }
  else
  {
    self->ConnectionActive.second = true; 
  }

  // Wait for connections until we want to stop the thread
  while ( self->ConnectionActive.first )
  {
    igtl::ClientSocket::Pointer newClientSocket = self->ServerSocket->WaitForConnection( CLIENT_SOCKET_TIMEOUT_MSEC );
    if (newClientSocket.IsNotNull())
    {
      // Lock before we change the clients list 
      PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(self->Mutex);

      newClientSocket->SetTimeout( CLIENT_SOCKET_TIMEOUT_MSEC ); 

      PlusIgtlClientInfo client; 
      client.ClientSocket = newClientSocket;

      self->IgtlClients.push_back(client); 
      self->LastCommandTimestamp[client.ClientId] = vtkAccurateTimer::GetSystemTime();

      int port = -1; 
      std::string address; 
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
      client.ClientSocket->GetSocketAddressAndPort(address, port);
#endif
      LOG_INFO( "Server received new client connection (" << address << ":" << port << ")." );
      LOG_INFO( "Number of connected clients: " << self->GetNumberOfConnectedClients() ); 
    }
  }

  // Close client sockets 
  std::list<PlusIgtlClientInfo>::iterator clientIterator; 
  for ( clientIterator = self->IgtlClients.begin(); clientIterator != self->IgtlClients.end(); ++clientIterator)
  {
    if ( (*clientIterator).ClientSocket.IsNotNull() )
    {
      (*clientIterator).ClientSocket->CloseSocket(); 
    }
  }
  self->IgtlClients.clear(); 

  // Close server socket 
  if ( self->ServerSocket.IsNotNull() )
  {
    self->ServerSocket->CloseSocket();
  }
  // Close thread
  self->ConnectionReceiverThreadId = -1;
  self->ConnectionActive.second = false; 
  return NULL;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::DataSenderThread( vtkMultiThreader::ThreadInfo* data )
{
  vtkPlusOpenIGTLinkServer* self = (vtkPlusOpenIGTLinkServer*)( data->UserData );
  self->DataSenderActive.second = true; 

  vtkPlusDevice* aDevice(NULL);
  vtkPlusChannel* aChannel(NULL);

  DeviceCollection aCollection;
  if( self->DataCollector->GetDevices(aCollection) != PLUS_SUCCESS || aCollection.size() == 0 )
  {
    LOG_ERROR("Unable to retrieve devices. Check configuration and connection.");
    return NULL;
  }

  // Find the requested channel ID in all the devices
  for( DeviceCollectionIterator it = aCollection.begin(); it != aCollection.end(); ++it )
  {
    aDevice = *it;
    if( aDevice->GetOutputChannelByName(aChannel, self->GetOutputChannelId() ) == PLUS_SUCCESS )
    {
      break;
    }
  }
  // The requested channel ID is not found, try to find any channel in any device
  if( aChannel == NULL )
  {
    for( DeviceCollectionIterator it = aCollection.begin(); it != aCollection.end(); ++it )
    {
      aDevice = *it;
      if( aDevice->OutputChannelCount() > 0 )
      {
        aChannel = *(aDevice->GetOutputChannelsStart());
        break;
      }
    }
  }
  // If we didn't find any channel then return
  if( aChannel == NULL )
  {
    LOG_WARNING("There are no channels to broadcast. Only command processing is available.");
  }

  self->BroadcastChannel = aChannel;
  if (self->BroadcastChannel)
  {
    self->BroadcastChannel->GetMostRecentTimestamp(self->LastSentTrackedFrameTimestamp);
  }

  double elapsedTimeSinceLastPacketSentSec = 0; 
  while ( self->ConnectionActive.first && self->DataSenderActive.first )
  {
    if ( self->IgtlClients.empty() )
    {
      // No client connected, wait for a while 
      vtkAccurateTimer::Delay(0.2);
      self->LastSentTrackedFrameTimestamp=0; // next time start sending from the most recent timestamp
      continue; 
    }

    if( self->HasGracePeriodExpired() )
    {
      self->GracePeriodLogLevel = vtkPlusLogger::LOG_LEVEL_WARNING;
    }

    // Send remote command execution replies to clients

    PlusCommandResponseList replies;
    self->PlusCommandProcessor->PopCommandResponses(replies);
    if (!replies.empty())
    {
      for (PlusCommandResponseList::iterator responseIt=replies.begin(); responseIt!=replies.end(); responseIt++)
      {
        igtl::MessageBase::Pointer igtlResponseMessage = self->CreateIgtlMessageFromCommandResponse(*responseIt);
        if (igtlResponseMessage.IsNull())
        {
          LOG_ERROR("Failed to create OpenIGTLink message from command response");
          continue;
        }
        igtlResponseMessage->Pack();

        bool broadcastResponse=false;
        
        // We treat image messages as special case: we send the results to all clients
        // TODO: now all images are broadcast to all clients, it should be more configurable (the command should be able
        // to specify if the image should be sent to the requesting client or all of them)
        vtkPlusCommandImageResponse* imageResponse=vtkPlusCommandImageResponse::SafeDownCast(*responseIt);
        if (imageResponse)
        {
          broadcastResponse=true;
        }

        if (broadcastResponse)
        {
          LOG_INFO("Broadcast command reply: "<<igtlResponseMessage->GetDeviceName());
          PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(self->Mutex);
          for (std::list<PlusIgtlClientInfo>::iterator clientIterator = self->IgtlClients.begin(); clientIterator != self->IgtlClients.end(); ++clientIterator)
          {
            if (clientIterator->ClientSocket.IsNull())
            {
              LOG_WARNING("Message reply cannot be sent to client, probably client has been disconnected");
              continue;
            }
            clientIterator->ClientSocket->Send(igtlResponseMessage->GetPackPointer(), igtlResponseMessage->GetPackSize());
          }
        }
        else
        {
          // Only send the response to the client that requested the command
          LOG_INFO("Send command reply: "<<igtlResponseMessage->GetDeviceName());
          PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(self->Mutex);
          igtl::ClientSocket::Pointer clientSocket=self->GetClientSocket((*responseIt)->GetClientId());
          if (clientSocket.IsNull())
          {
            LOG_WARNING("Message reply cannot be sent to client, probably client has been disconnected");
            continue;
          }          
          clientSocket->Send(igtlResponseMessage->GetPackPointer(), igtlResponseMessage->GetPackSize());
        }

      }
    }

    // Send image/tracking/string data

    vtkSmartPointer<vtkTrackedFrameList> trackedFrameList = vtkSmartPointer<vtkTrackedFrameList>::New(); 
    double startTimeSec = vtkAccurateTimer::GetSystemTime();

    // Acquire tracked frames since last acquisition (minimum 1 frame)
    if (self->LastProcessingTimePerFrameMs < 1)
    {
      // if processing was less than 1ms/frame then assume it was 1ms (1000FPS processing speed) to avoid division by zero
      self->LastProcessingTimePerFrameMs=1;
    }
    int numberOfFramesToGet = std::max(self->MaxTimeSpentWithProcessingMs / self->LastProcessingTimePerFrameMs, 1); 
    // Maximize the number of frames to send
    numberOfFramesToGet = std::min(numberOfFramesToGet, self->MaxNumberOfIgtlMessagesToSend); 

    if (self->BroadcastChannel!=NULL)
    {
      if ( ( self->BroadcastChannel->HasVideoSource() && !self->BroadcastChannel->GetVideoDataAvailable())
        || (!self->BroadcastChannel->HasVideoSource() && !self->BroadcastChannel->GetTrackingDataAvailable()) )
      {
        LOG_DYNAMIC("No data is broadcasted, as no data is available yet.", self->GracePeriodLogLevel); 
      }
      else
      {
        double oldestDataTimestamp=0;
        if (self->BroadcastChannel->GetOldestTimestamp(oldestDataTimestamp)==PLUS_SUCCESS)
        {
          if (self->LastSentTrackedFrameTimestamp<oldestDataTimestamp)
          {
            LOG_INFO("OpenIGTLink broadcasting started. No data was available between "<<self->LastSentTrackedFrameTimestamp<<"-"<<oldestDataTimestamp<<"sec, therefore no data were broadcasted during this time period.");
            self->LastSentTrackedFrameTimestamp=oldestDataTimestamp+SAMPLING_SKIPPING_MARGIN_SEC;
          }
          if ( self->BroadcastChannel->GetTrackedFrameList(self->LastSentTrackedFrameTimestamp, trackedFrameList, numberOfFramesToGet) != PLUS_SUCCESS )
          {
            LOG_ERROR("Failed to get tracked frame list from data collector (last recorded timestamp: " << std::fixed << self->LastSentTrackedFrameTimestamp ); 
            vtkAccurateTimer::Delay(DELAY_ON_SENDING_ERROR_SEC); 
          }
        }      
      }
    }

    // There is no new frame in the buffer
    if ( trackedFrameList->GetNumberOfTrackedFrames() == 0 )
    {
      vtkAccurateTimer::Delay(DELAY_ON_NO_NEW_FRAMES_SEC);
      elapsedTimeSinceLastPacketSentSec += vtkAccurateTimer::GetSystemTime() - startTimeSec; 

      // Send keep alive packet to clients 
      if ( 1000* elapsedTimeSinceLastPacketSentSec > ( CLIENT_SOCKET_TIMEOUT_MSEC / 2.0 ) )
      {
        self->KeepAlive(); 
        elapsedTimeSinceLastPacketSentSec = 0; 
      }

      continue;
    }

    for ( int i = 0; i < trackedFrameList->GetNumberOfTrackedFrames(); ++i )
    {
      // Send tracked frame
      self->SendTrackedFrame( *trackedFrameList->GetTrackedFrame(i) ); 
      elapsedTimeSinceLastPacketSentSec = 0; 
    }

    // Compute time spent with processing one frame in this round
    double computationTimeMs = (vtkAccurateTimer::GetSystemTime() - startTimeSec) * 1000.0;

    // Update last processing time if new tracked frames have been aquired
    if (trackedFrameList->GetNumberOfTrackedFrames() > 0 )
    {
      self->LastProcessingTimePerFrameMs = computationTimeMs / trackedFrameList->GetNumberOfTrackedFrames();
    } 
  }
  // Close thread
  self->DataSenderThreadId = -1;
  self->DataSenderActive.second = false; 
  return NULL;
}

//----------------------------------------------------------------------------
void* vtkPlusOpenIGTLinkServer::DataReceiverThread( vtkMultiThreader::ThreadInfo* data )
{
  vtkPlusOpenIGTLinkServer* self = (vtkPlusOpenIGTLinkServer*)( data->UserData );
  self->DataReceiverActive.second = true; 

  std::list<PlusIgtlClientInfo>::iterator clientIterator; 
  std::list<PlusIgtlClientInfo> igtlClients; 
  while ( self->ConnectionActive.first && self->DataReceiverActive.first )
  {
    // make a copy of client infos to avoid lock 
    {
      PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(self->Mutex);
      igtlClients = self->IgtlClients; 
    }

    if ( igtlClients.empty() )
    {
      // No client connected, wait for a while 
      vtkAccurateTimer::Delay(0.2);
      continue; 
    }

    for ( clientIterator = igtlClients.begin(); clientIterator != igtlClients.end(); ++clientIterator)
    {
      PlusIgtlClientInfo client = (*clientIterator); 
      igtl::MessageHeader::Pointer headerMsg;
      headerMsg = igtl::MessageHeader::New();
      headerMsg->InitPack();

      if( vtkAccurateTimer::GetSystemTime() - self->LastCommandTimestamp[client.ClientId] > CLEAR_PREVIOUS_COMMANDS_TIMEOUT_SEC && !self->PreviousCommands[client.ClientId].empty() )
      {
        self->LastCommandTimestamp[client.ClientId] = vtkAccurateTimer::GetSystemTime();
        self->PreviousCommands[client.ClientId].clear();
      }

      // Receive generic header from the socket
      int bytesReceived = client.ClientSocket->Receive( headerMsg->GetPackPointer(), headerMsg->GetPackSize() );
      if ( bytesReceived == IGTL_EMPTY_DATA_SIZE || bytesReceived != headerMsg->GetPackSize() )
      {
        continue; 
      }

      self->LastCommandTimestamp[client.ClientId] = vtkAccurateTimer::GetSystemTime();

      headerMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
      if (strcmp(headerMsg->GetDeviceType(), "CLIENTINFO") == 0)
      {
        igtl::PlusClientInfoMessage::Pointer clientInfoMsg = igtl::PlusClientInfoMessage::New(); 
        clientInfoMsg->SetMessageHeader(headerMsg); 
        clientInfoMsg->AllocatePack(); 

        client.ClientSocket->Receive(clientInfoMsg->GetPackBodyPointer(), clientInfoMsg->GetPackBodySize() ); 

        int c = clientInfoMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
        if (c & igtl::MessageHeader::UNPACK_BODY) 
        {
          int port = -1; 
          std::string clientAddress; 
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
          client.ClientSocket->GetSocketAddressAndPort(clientAddress, port);
#endif
          // Message received from client, need to lock to modify client info
          PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(self->Mutex);
          std::list<PlusIgtlClientInfo>::iterator it = std::find(self->IgtlClients.begin(), self->IgtlClients.end(), client ); 
          if ( it != self->IgtlClients.end() )
          {
            // Copy client info
            (*it).ShallowCopy(clientInfoMsg->GetClientInfo()); 
            LOG_INFO("Client info message received from client (" << clientAddress << ":" << port << ")."); 
          }
        }
      }
      else if (strcmp(headerMsg->GetDeviceType(), "GET_STATUS") == 0)
      {
        // Just ping server, we can skip message and respond
        client.ClientSocket->Skip(headerMsg->GetBodySizeToRead(), 0);

        igtl::StatusMessage::Pointer replyMsg = igtl::StatusMessage::New(); 
        replyMsg->SetCode(igtl::StatusMessage::STATUS_OK); 
        replyMsg->Pack(); 
        client.ClientSocket->Send(replyMsg->GetPackPointer(), replyMsg->GetPackBodySize()); 
      }
      else if ( (strcmp(headerMsg->GetDeviceType(), "STRING") == 0) )
      {
        // Received a remote command execution message
        // The command is encoded in in an XML string in a STRING message body

        igtl::StringMessage::Pointer commandMsg = igtl::StringMessage::New(); 
        commandMsg->SetMessageHeader(headerMsg); 
        commandMsg->AllocatePack(); 
        client.ClientSocket->Receive(commandMsg->GetPackBodyPointer(), commandMsg->GetPackBodySize() ); 
        
        int c = commandMsg->Unpack(self->IgtlMessageCrcCheckEnabled);
        if (c & igtl::MessageHeader::UNPACK_BODY) 
        {          
          const char* deviceName = "UNKNOWN";
          if (headerMsg->GetDeviceName() != NULL)
          {
            deviceName = headerMsg->GetDeviceName();
          }
          else
          {
            LOG_ERROR("Received message from unknown device");
          }

          std::string deviceNameStr=vtkPlusCommand::GetPrefixFromCommandDeviceName(deviceName);
          std::string uid=vtkPlusCommand::GetUidFromCommandDeviceName(deviceName);;
          if( !uid.empty() )
          {
            std::vector<std::string> & previousCommands = self->PreviousCommands[client.ClientId];
            if( std::find(previousCommands.begin(), previousCommands.end(), uid) != previousCommands.end() )
            {
              // Command already exists
              LOG_WARNING("Already received a command with id = " << uid << " from client id = " << client.ClientId <<". This repeated command will be ignored.");
              continue;
            }
            else
            {
              self->PreviousCommands[client.ClientId].push_back(uid);
            }
          }
          std::ostringstream ss;
          ss << "Received command from device " << deviceNameStr;
          if( !uid.empty() )
          {
            ss << " with UID " << uid;
          }
          ss << ": " << commandMsg->GetString();
          LOG_INFO(ss.str());

          self->PlusCommandProcessor->QueueCommand(client.ClientId, commandMsg->GetString(), deviceNameStr, uid);
        }
        else
        {
          LOG_ERROR("STRING message unpacking failed");
        }        
      }
      else if ( (strcmp(headerMsg->GetDeviceType(), "GET_IMGMETA") == 0) )
      {
        std::string deviceName("");
        if (headerMsg->GetDeviceName() != NULL)
        {
          deviceName = headerMsg->GetDeviceName();
        }
        self->PlusCommandProcessor->QueueGetImageMetaData(client.ClientId, deviceName);
      }
      else if(strcmp(headerMsg->GetDeviceType(), "GET_IMAGE") == 0)
      {
        std::string deviceName("");
        if (headerMsg->GetDeviceName() != NULL)
        {
          deviceName = headerMsg->GetDeviceName();
        }
        else
        {
          LOG_ERROR("Please select the image you want to acquire");
          return NULL;
        }
        self->PlusCommandProcessor->QueueGetImage(client.ClientId, deviceName);
      }
      else
      {
        // if the device type is unknown, skip reading. 
        LOG_WARNING("Unknown OpenIGTLink message is received. Device type: "<<headerMsg->GetDeviceType()<<". Device name: "<<headerMsg->GetDeviceName()<<".");
        client.ClientSocket->Skip(headerMsg->GetBodySizeToRead(), 0);
        continue; 
      }

    } // clientIterator

  } // ConnectionActive

  // Close thread
  self->DataReceiverThreadId = -1;
  self->DataReceiverActive.second = false; 
  return NULL;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::SendTrackedFrame( TrackedFrame& trackedFrame )
{
  int numberOfErrors = 0; 

  // Update transform repository with the tracked frame 
  if ( this->TransformRepository != NULL )
  {
    if ( this->TransformRepository->SetTransforms(trackedFrame) != PLUS_SUCCESS )
    {
      LOG_ERROR("Failed to set current transforms to transform repository"); 
      numberOfErrors++;
    }
  }

  // Convert relative timestamp to UTC
  double timestampSystem = trackedFrame.GetTimestamp(); // save original timestamp, we'll restore it later
  double timestampUniversal = vtkAccurateTimer::GetUniversalTimeFromSystemTime(timestampSystem);
  trackedFrame.SetTimestamp(timestampUniversal);  

  // Lock before we send message to the clients 
  PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
  bool clientDisconnected = false;

  std::list<PlusIgtlClientInfo>::iterator clientIterator = this->IgtlClients.begin();
  while ( clientIterator != this->IgtlClients.end() )
  {
    PlusIgtlClientInfo client = (*clientIterator);

    // Create igt messages
    std::vector<igtl::MessageBase::Pointer> igtlMessages; 
    std::vector<igtl::MessageBase::Pointer>::iterator igtlMessageIterator; 

    // Set message types 
    std::vector<std::string> messageTypes = this->DefaultClientInfo.IgtlMessageTypes; 
    if ( !client.IgtlMessageTypes.empty() )
    {
      messageTypes = client.IgtlMessageTypes; 
    }

    // Set transform names 
    std::vector<PlusTransformName> transformNames = this->DefaultClientInfo.TransformNames; 
    if ( !client.TransformNames.empty() )
    {
      transformNames = client.TransformNames; 
    }

    // Set image transform names
    std::vector<PlusIgtlClientInfo::ImageStream> imageStreams = this->DefaultClientInfo.ImageStreams; 
    if ( !client.ImageStreams.empty() )
    {
      imageStreams = client.ImageStreams; 
    }

    // Set string names
    std::vector<std::string> stringNames = this->DefaultClientInfo.StringNames;
    if ( !client.StringNames.empty() )
    {
      stringNames = client.StringNames; 
    }

    vtkSmartPointer<vtkPlusIgtlMessageFactory> igtlMessageFactory = vtkSmartPointer<vtkPlusIgtlMessageFactory>::New(); 
    if ( igtlMessageFactory->PackMessages( messageTypes, igtlMessages, trackedFrame, transformNames, imageStreams, stringNames, this->SendValidTransformsOnly, this->TransformRepository ) != PLUS_SUCCESS )
    {
      LOG_WARNING("Failed to pack all IGT messages"); 
    }

    // Send all messages to a client 
    for ( igtlMessageIterator = igtlMessages.begin(); igtlMessageIterator != igtlMessages.end(); ++igtlMessageIterator )
    {
      igtl::MessageBase::Pointer igtlMessage = (*igtlMessageIterator); 
      if ( igtlMessage.IsNull() )
      {
        continue; 
      }

      int retValue = 0;
      RETRY_UNTIL_TRUE( 
        (retValue = client.ClientSocket->Send( igtlMessage->GetPackPointer(), igtlMessage->GetPackSize()))!=0,
        this->NumberOfRetryAttempts, this->DelayBetweenRetryAttemptsSec);
      if ( retValue == 0 )
      {
        clientDisconnected = true; 
        igtl::TimeStamp::Pointer ts = igtl::TimeStamp::New(); 
        igtlMessage->GetTimeStamp(ts); 

        LOG_DEBUG( "Client disconnected - could not send " << igtlMessage->GetDeviceType() << " message to client (device name: " << igtlMessage->GetDeviceName()
          << "  Timestamp: " << std::fixed <<  ts->GetTimeStamp() << ").");
        break; 
      }

    } // igtlMessageIterator

    if ( clientDisconnected )
    {
      int port = -1; 
      std::string address; 
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
      client.ClientSocket->GetSocketAddressAndPort(address, port); 
#endif
      LOG_INFO( "Client disconnected (" <<  address << ":" << port << ")."); 
      clientIterator = this->IgtlClients.erase(clientIterator);
      LOG_INFO( "Number of connected clients: " << GetNumberOfConnectedClients() ); 
      clientDisconnected = false; 
      continue; 
    }

    // Send messages to the next client 
    ++clientIterator; 

  } // clientIterator

  // restore original timestamp
  trackedFrame.SetTimestamp(timestampSystem);

  return ( numberOfErrors == 0 ? PLUS_SUCCESS : PLUS_FAIL );
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::KeepAlive()
{
  int numberOfErrors = 0; 

  // Lock before we send message to the clients 
  PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
  bool clientDisconnected = false;

  std::list<PlusIgtlClientInfo>::iterator clientIterator = this->IgtlClients.begin();
  while ( clientIterator != this->IgtlClients.end() )
  {
    PlusIgtlClientInfo client = (*clientIterator);

    igtl::StatusMessage::Pointer replyMsg = igtl::StatusMessage::New(); 
    replyMsg->SetCode(igtl::StatusMessage::STATUS_OK); 
    replyMsg->Pack(); 

    int retValue = 0;
    RETRY_UNTIL_TRUE( 
      (retValue = client.ClientSocket->Send( replyMsg->GetPackPointer(), replyMsg->GetPackSize() ))!=0,
      this->NumberOfRetryAttempts, this->DelayBetweenRetryAttemptsSec);
    bool clientDisconnected = false; 
    if ( retValue == 0 )
    {
      clientDisconnected = true; 
      igtl::TimeStamp::Pointer ts = igtl::TimeStamp::New(); 
      replyMsg->GetTimeStamp(ts); 

      LOG_DEBUG( "Client disconnected - could not send " << replyMsg->GetDeviceType() << " message to client (device name: " << replyMsg->GetDeviceName()
        << "  Timestamp: " << std::fixed <<  ts->GetTimeStamp() << ").");
    }

    if ( clientDisconnected )
    {
      int port = -1; 
      std::string address; 
#if (OPENIGTLINK_VERSION_MAJOR > 1) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR > 9 ) || ( OPENIGTLINK_VERSION_MAJOR == 1 && OPENIGTLINK_VERSION_MINOR == 9 && OPENIGTLINK_VERSION_PATCH > 4 )
      client.ClientSocket->GetSocketAddressAndPort(address, port); 
#endif
      LOG_INFO( "Client disconnected (" <<  address << ":" << port << ")."); 
      clientIterator = this->IgtlClients.erase(clientIterator);
      LOG_INFO( "Number of connected clients: " << GetNumberOfConnectedClients() ); 
      clientDisconnected = false; 
      continue; 
    }

    // Send messages to the next client 
    ++clientIterator; 

  } // clientIterator

  LOG_TRACE("Keep alive packet sent to clients..."); 
  return ( numberOfErrors == 0 ? PLUS_SUCCESS : PLUS_FAIL );
}

//------------------------------------------------------------------------------
int vtkPlusOpenIGTLinkServer::GetNumberOfConnectedClients()
{
  // Lock before we send message to the clients 
  PlusLockGuard<vtkRecursiveCriticalSection> updateMutexGuardedLock(this->Mutex);
  return this->IgtlClients.size(); 
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::ReadConfiguration(vtkXMLDataElement* aConfigurationData, const char* aFilename)
{
  LOG_TRACE("vtkPlusOpenIGTLinkServer::ReadConfiguration");

  XML_FIND_NESTED_ELEMENT_REQUIRED(plusOpenIGTLinkServerConfig, aConfigurationData, "PlusOpenIGTLinkServer");

  if( aFilename == NULL )
  {
    LOG_ERROR("Unable to configure PlusServer without an acceptable config file submitted.");
    return PLUS_FAIL;
  }
  this->SetConfigFilename(aFilename);

  XML_READ_SCALAR_ATTRIBUTE_REQUIRED(int, ListeningPort, plusOpenIGTLinkServerConfig);
  XML_READ_STRING_ATTRIBUTE_REQUIRED(OutputChannelId, plusOpenIGTLinkServerConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, MissingInputGracePeriodSec, plusOpenIGTLinkServerConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(double, MaxTimeSpentWithProcessingMs, plusOpenIGTLinkServerConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, MaxNumberOfIgtlMessagesToSend, plusOpenIGTLinkServerConfig);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(SendValidTransformsOnly, plusOpenIGTLinkServerConfig);
  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(IgtlMessageCrcCheckEnabled, plusOpenIGTLinkServerConfig);

  this->DefaultClientInfo.IgtlMessageTypes.clear();
  this->DefaultClientInfo.TransformNames.clear();
  this->DefaultClientInfo.ImageStreams.clear();
  this->DefaultClientInfo.StringNames.clear();

  vtkXMLDataElement* defaultClientInfo = plusOpenIGTLinkServerConfig->FindNestedElementWithName("DefaultClientInfo"); 
  if ( defaultClientInfo != NULL )
  {
    if (this->DefaultClientInfo.SetClientInfoFromXmlData(defaultClientInfo)!=PLUS_SUCCESS)
    {
      return PLUS_FAIL;
    }
  }

  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
igtl::ClientSocket::Pointer vtkPlusOpenIGTLinkServer::GetClientSocket(int clientId)
{
  // Close client sockets 
  std::list<PlusIgtlClientInfo>::iterator clientIterator; 
  for ( clientIterator = this->IgtlClients.begin(); clientIterator != this->IgtlClients.end(); ++clientIterator)
  {
    if (clientIterator->ClientId==clientId)
    {
      return clientIterator->ClientSocket;
    }
  }
  return NULL;
}

//------------------------------------------------------------------------------
int vtkPlusOpenIGTLinkServer::ProcessPendingCommands()
{
  return this->PlusCommandProcessor->ExecuteCommands();
}

//------------------------------------------------------------------------------
vtkDataCollector* vtkPlusOpenIGTLinkServer::GetDataCollector()
{
  return this->DataCollector;
}

//------------------------------------------------------------------------------
vtkTransformRepository* vtkPlusOpenIGTLinkServer::GetTransformRepository()
{
  return this->TransformRepository;
}

//------------------------------------------------------------------------------
bool vtkPlusOpenIGTLinkServer::HasGracePeriodExpired()
{
  return (vtkAccurateTimer::GetSystemTime() - this->BroadcastStartTime) > this->MissingInputGracePeriodSec;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::Start(const std::string &inputConfigFileName)
{
  // Read main configuration file
  std::string configFilePath=inputConfigFileName;
  if (!vtksys::SystemTools::FileExists(configFilePath.c_str(), true))
  {
    configFilePath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(inputConfigFileName);
    if (!vtksys::SystemTools::FileExists(configFilePath.c_str(), true))
    {
      LOG_ERROR("Reading device set configuration file failed: "<<inputConfigFileName<<" does not exist in the current directory or in "<<vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationDirectory());
      return PLUS_FAIL;      
    }
  }
  vtkSmartPointer<vtkXMLDataElement> configRootElement = vtkSmartPointer<vtkXMLDataElement>::Take(vtkXMLUtilities::ReadElementFromFile(configFilePath.c_str()));
  if (configRootElement == NULL)
  {
    LOG_ERROR("Reading device set configuration file failed: syntax error in "<<inputConfigFileName);
    return PLUS_FAIL;
  }

  // Print configuration file contents for debugging purposes
  LOG_DEBUG("Device set configuration is read from file: " << inputConfigFileName);
  std::ostringstream xmlFileContents; 
  PlusCommon::PrintXML(xmlFileContents, vtkIndent(1), configRootElement);
  LOG_DEBUG("Device set configuration file contents: " << std::endl << xmlFileContents.str());

  vtkPlusConfig::GetInstance()->SetDeviceSetConfigurationData(configRootElement);

  // Create data collector instance 
  vtkSmartPointer<vtkDataCollector> dataCollector = vtkSmartPointer<vtkDataCollector>::New();
  if ( dataCollector->ReadConfiguration( configRootElement ) != PLUS_SUCCESS )
  {
    LOG_ERROR("Datacollector failed to read configuration"); 
    return PLUS_FAIL;
  }

  // Create transform repository instance 
  vtkSmartPointer<vtkTransformRepository> transformRepository = vtkSmartPointer<vtkTransformRepository>::New(); 
  if ( transformRepository->ReadConfiguration( configRootElement ) != PLUS_SUCCESS )
  {
    LOG_ERROR("Transform repository failed to read configuration"); 
    return PLUS_FAIL;
  }

  LOG_DEBUG( "Initializing data collector... " );
  if ( dataCollector->Connect() != PLUS_SUCCESS )
  {
    LOG_ERROR("Datacollector failed to connect to devices"); 
    return PLUS_FAIL;
  }

  if ( dataCollector->Start() != PLUS_SUCCESS )
  {
    LOG_ERROR("Datacollector failed to start"); 
    return PLUS_FAIL;
  }

  SetDataCollector( dataCollector );
  if ( ReadConfiguration(configRootElement, configFilePath.c_str()) != PLUS_SUCCESS )
  {
    LOG_ERROR("Failed to read PlusOpenIGTLinkServer configuration"); 
    return PLUS_FAIL;
  }

  SetTransformRepository( transformRepository ); 
  if ( StartOpenIGTLinkService() != PLUS_SUCCESS )
  {
    LOG_ERROR("Failed to start Plus OpenIGTLink server"); 
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//------------------------------------------------------------------------------
PlusStatus vtkPlusOpenIGTLinkServer::Stop()
{
  PlusStatus status=PLUS_SUCCESS;

  if (StopOpenIGTLinkService()!=PLUS_SUCCESS)
  {
    status=PLUS_FAIL;
  }

  if (this->GetDataCollector())
  {
    this->GetDataCollector()->Stop();
    this->GetDataCollector()->Disconnect();
  }
  SetDataCollector(NULL);

  SetTransformRepository(NULL);

  return status;
}

//------------------------------------------------------------------------------
igtl::MessageBase::Pointer vtkPlusOpenIGTLinkServer::CreateIgtlMessageFromCommandResponse(vtkPlusCommandResponse* response)
{
  vtkPlusCommandStringResponse* stringResponse=vtkPlusCommandStringResponse::SafeDownCast(response);
  if (stringResponse)
  {
    igtl::StringMessage::Pointer igtlMessage = igtl::StringMessage::New();
    igtlMessage->SetDeviceName(stringResponse->GetDeviceName().c_str());
    if (stringResponse->GetDeviceName().empty())
    {
      LOG_WARNING("OpenIGTLink STRING message device name is empty");
    }
    std::ostringstream replyStr;
    replyStr << "<CommandReply";
    replyStr << " Status=\"" << (stringResponse->GetStatus() == PLUS_SUCCESS ? "SUCCESS" : "FAIL") << "\"";
    replyStr << " Message=\"";
    // Write to XML, encoding special characters, such as " ' \ < > &
    vtkXMLUtilities::EncodeString(stringResponse->GetMessage().c_str(), VTK_ENCODING_NONE, replyStr, VTK_ENCODING_NONE, 1 /* encode special characters */ );
    replyStr << "\"";
    replyStr << " />";

    igtlMessage->SetString(replyStr.str().c_str());
    LOG_DEBUG("Command response: "<<replyStr.str());
    return igtlMessage.GetPointer();
  }

  vtkPlusCommandImageResponse* imageResponse=vtkPlusCommandImageResponse::SafeDownCast(response);
  if (imageResponse)
  {
    std::string imageName=imageResponse->GetImageName();    
    if (imageName.empty())
    {
      imageName="PlusServerImage";
    }

    vtkSmartPointer<vtkMatrix4x4> imageToReferenceTransform=vtkSmartPointer<vtkMatrix4x4>::New();
    if (imageResponse->GetImageToReferenceTransform()!=NULL)
    {
      imageToReferenceTransform=imageResponse->GetImageToReferenceTransform();
    }

    vtkImageData* imageData=imageResponse->GetImageData();
    if (imageData==NULL)
    {
      LOG_ERROR("Invalid image data in command response");
      return NULL;
    }

    igtl::ImageMessage::Pointer igtlMessage = igtl::ImageMessage::New();
    igtlMessage->SetDeviceName(imageName.c_str());  
    
    if ( vtkPlusIgtlMessageCommon::PackImageMessage(igtlMessage, imageData, 
      imageToReferenceTransform, vtkAccurateTimer::GetSystemTime()) != PLUS_SUCCESS )
    {
      LOG_ERROR("Failed to create image mesage from command response");
      return NULL;
    }
    return igtlMessage.GetPointer();
  }
  vtkPlusCommandImageMetaDataResponse* imageMetaDataResponse=vtkPlusCommandImageMetaDataResponse::SafeDownCast(response);
  if (imageMetaDataResponse)
  {
    std::string imageMetaDataName="PlusServerImageMetaData";
    PlusCommon::ImageMetaDataList imageMetaDataList;
    imageMetaDataResponse->GetImageMetaDataItems(imageMetaDataList);
    igtl::ImageMetaMessage::Pointer igtlMessage = igtl::ImageMetaMessage::New();
    igtlMessage->SetDeviceName(imageMetaDataName.c_str());                  
    if ( vtkPlusIgtlMessageCommon::PackImageMetaMessage(igtlMessage,imageMetaDataList) != PLUS_SUCCESS )
    {
      LOG_ERROR("Failed to create image mesage from command response");
      return NULL;
    }
    return igtlMessage.GetPointer();
  }  

  LOG_ERROR("vtkPlusOpenIGTLinkServer::CreateIgtlMessageFromCommandResponse failed: invalid command response");
  return NULL;
}
