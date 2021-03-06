/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkMPIMoveData.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkMPIMoveData.h"

#include "vtkAppendFilter.h"
#include "vtkAppendPolyData.h"
#include "vtkCellData.h"
#include "vtkCharArray.h"
#include "vtkDataSetReader.h"
#include "vtkDataSetWriter.h"
#include "vtkDirectedGraph.h"
#include "vtkGraphReader.h"
#include "vtkGraphWriter.h"
#include "vtkImageAppend.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMergeGraphs.h"
#include "vtkMPIMToNSocketConnection.h"
#include "vtkMultiProcessController.h"
#include "vtkObjectFactory.h"
#include "vtkOutlineFilter.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkProcessModule.h"
#include "vtkSmartPointer.h"
#include "vtkSocketCommunicator.h"
#include "vtkSocketController.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkTimerLog.h"
#include "vtkToolkits.h"
#include "vtkUndirectedGraph.h"
#include "vtkUnstructuredGrid.h"
#include "vtk_zlib.h"
#include <vtksys/ios/sstream>

#ifdef VTK_USE_MPI
#include "vtkMPICommunicator.h"
#include "vtkAllToNRedistributePolyData.h"
#endif


bool vtkMPIMoveData::UseZLibCompression = false;


vtkStandardNewMacro(vtkMPIMoveData);

vtkCxxSetObjectMacro(vtkMPIMoveData,Controller, vtkMultiProcessController);
vtkCxxSetObjectMacro(vtkMPIMoveData,MPIMToNSocketConnection, vtkMPIMToNSocketConnection);
//-----------------------------------------------------------------------------
vtkMPIMoveData::vtkMPIMoveData()
{
  this->Controller = 0;
  this->ClientDataServerSocketController = 0;
  this->MPIMToNSocketConnection = 0;

  this->SetController(vtkMultiProcessController::GetGlobalController());

  this->MoveMode = vtkMPIMoveData::PASS_THROUGH;
  // This tells which server/client this object is on.
  this->Server = -1;

  // This is set on the data server and render server when we are running
  // with a render server.
  this->MPIMToNSocketConnection = 0;

  this->NumberOfBuffers = 0;
  this->BufferLengths = 0;
  this->BufferOffsets = 0;
  this->Buffers = 0;
  this->BufferTotalLength = 9;

  this->OutputDataType = VTK_POLY_DATA;

  this->UpdateNumberOfPieces = 0;
  this->UpdatePiece = 0;

  this->DeliverOutlineToClient = 0;
}

//-----------------------------------------------------------------------------
vtkMPIMoveData::~vtkMPIMoveData()
{
  this->SetController(0);
  this->ClientDataServerSocketController = 0;
  this->SetMPIMToNSocketConnection(0);
  this->ClearBuffer();
}

//----------------------------------------------------------------------------
void vtkMPIMoveData::SetUseZLibCompression(bool b)
{
  vtkMPIMoveData::UseZLibCompression = b;
}

//----------------------------------------------------------------------------
bool vtkMPIMoveData::GetUseZLibCompression()
{
  return vtkMPIMoveData::UseZLibCompression;
}

//----------------------------------------------------------------------------
int vtkMPIMoveData::FillInputPortInformation(int, vtkInformation *info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataObject");
  info->Set(vtkAlgorithm::INPUT_IS_OPTIONAL(), 1);
  return 1;
}

//-----------------------------------------------------------------------------
int vtkMPIMoveData::RequestDataObject(vtkInformation*,
                                      vtkInformationVector**,
                                      vtkInformationVector* outputVector)
{
  vtkDataObject* output =
    outputVector->GetInformationObject(0)->Get(vtkDataObject::DATA_OBJECT());

  vtkDataObject* outputCopy = 0;
  if (this->OutputDataType == VTK_POLY_DATA)
    {
    if (output && output->IsA("vtkPolyData"))
      {
      return 1;
      }
    outputCopy = vtkPolyData::New();
    }
  else if (this->OutputDataType == VTK_UNSTRUCTURED_GRID)
    {
    if (output && output->IsA("vtkUnstructuredGrid"))
      {
      return 1;
      }
    outputCopy = vtkUnstructuredGrid::New();
    }
  else if (this->OutputDataType == VTK_IMAGE_DATA)
    {
    if (output && output->IsA("vtkImageData"))
      {
      return 1;
      }
    outputCopy = vtkImageData::New();
    }
  else if (this->OutputDataType == VTK_DIRECTED_GRAPH)
    {
    if (output && output->IsA("vtkDirectedGraph"))
      {
      return 1;
      }
    outputCopy = vtkDirectedGraph::New();
    }
  else if (this->OutputDataType == VTK_UNDIRECTED_GRAPH)
    {
    if (output && output->IsA("vtkUndirectedGraph"))
      {
      return 1;
      }
    outputCopy = vtkUndirectedGraph::New();
    }
  else
    {
    vtkErrorMacro("Unrecognized output type: " << this->OutputDataType
                  << ". Cannot create output.");
    return 0;
    }

  outputCopy->SetPipelineInformation(outputVector->GetInformationObject(0));
  outputCopy->Delete();
  return 1;
}

//-----------------------------------------------------------------------------
int vtkMPIMoveData::RequestInformation(vtkInformation*,
                                       vtkInformationVector** inputVector,
                                       vtkInformationVector* outputVector)
{
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  if (inputVector[0]->GetNumberOfInformationObjects() > 0)
    {
    outInfo->Set(
      vtkStreamingDemandDrivenPipeline::MAXIMUM_NUMBER_OF_PIECES(),
      inputVector[0]->GetInformationObject(0)->Get(
        vtkStreamingDemandDrivenPipeline::MAXIMUM_NUMBER_OF_PIECES()));
    return 1;
    }

  outInfo->Set(
    vtkStreamingDemandDrivenPipeline::MAXIMUM_NUMBER_OF_PIECES(), -1);

  return 1;
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DetermineClientDataServerController()
{
  this->ClientDataServerSocketController = 0;
  if (this->Server != vtkMPIMoveData::RENDER_SERVER)
    {
    vtkProcessModule* pm = vtkProcessModule::GetProcessModule();
    this->ClientDataServerSocketController =
      pm? pm->GetActiveSocketController() : 0;
    }
}

//-----------------------------------------------------------------------------
// This filter  is going to replace the many variations of collection fitlers.
// It handles collection and duplication.
// It handles poly data and unstructured grid.
// It handles rendering on the data server and render server.


// Pass through, No render server. (Distributed rendering on data server).
// Data server copy input to output.

// Passthrough, Yes RenderServer (Distributed rendering on render server).
// Data server MtoN
// Move data from N data server processes to N render server processes.

// Duplicate, No render server. (Tile rendering on data server and client).
// GatherAll on data server.
// Data server process 0 sends data to client.

// Duplicate, Yes RenderServer (Tile rendering on rendering server and client).
// GatherToZero on data server.
// Data server process 0 sends to client
// Data server process 0 sends to render server 0
// Render server process 0 broad casts to all render server processes.

// Collect, render server: yes or no. (client rendering).
// GatherToZero on data server.
// Data server process 0 sends data to client.

//-----------------------------------------------------------------------------
// We should avoid marshalling more than once.
int vtkMPIMoveData::RequestData(vtkInformation*,
                                vtkInformationVector** inputVector,
                                vtkInformationVector* outputVector)
{
  this->DetermineClientDataServerController();

  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  vtkDataObject* input = 0;
  vtkDataObject* output = outInfo->Get(vtkDataObject::DATA_OBJECT());

  if (inputVector[0]->GetNumberOfInformationObjects() > 0)
    {
    input = inputVector[0]->GetInformationObject(0)->Get(
        vtkDataObject::DATA_OBJECT());
    }


  if (this->OutputDataType == VTK_IMAGE_DATA)
    {
    if (this->MoveMode == vtkMPIMoveData::PASS_THROUGH && this->MPIMToNSocketConnection)
      {
      vtkErrorMacro("Image data delivery to render server not supported.");
      return 0;
      }
    }

  this->UpdatePiece = outInfo->Get(
    vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());
  this->UpdateNumberOfPieces = outInfo->Get(
    vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES());

  // This case deals with everything running as one MPI group
  // Client, Data and render server are all the same program.
  // This covers single process mode too, although this filter
  // is unnecessary in that mode and really should not be put in.
  if (this->MPIMToNSocketConnection == 0 &&
      this->ClientDataServerSocketController == 0)
    {
    // Clone in this mode is used for plots and picking.
    if (this->MoveMode == vtkMPIMoveData::CLONE)
      {
      this->DataServerGatherAll(input, output);
      return 1;
      }
    // Collect mode for rendering on node 0.
    if (this->MoveMode == vtkMPIMoveData::COLLECT)
      {
      this->DataServerGatherToZero(input, output);
      return 1;
      }
    // PassThrough mode for compositing.
    if (this->MoveMode == vtkMPIMoveData::PASS_THROUGH)
      {
      output->ShallowCopy(input);
      return 1;
      }
    // Collect and PassThrough.
    if (this->MoveMode == vtkMPIMoveData::COLLECT_AND_PASS_THROUGH)
      {
      // Collect
      this->DataServerGatherToZero(input, output);
      // PassThrough
      output->ShallowCopy(input);
      return 1;
      }
    vtkErrorMacro("MoveMode not set.");
    return 0;
    }

  // PassThrough with no RenderServer. (Distributed rendering on data server).
  // Data server copy input to output.
  // Render server and client will not have an input.
  if (this->MoveMode == vtkMPIMoveData::PASS_THROUGH &&
      this->MPIMToNSocketConnection == 0)
    {
    if (input)
      {
      output->ShallowCopy(input);
      }
    return 1;
    }

  // Passthrough and RenderServer (Distributed rendering on render server).
  // Data server MtoN
  // Move data from N data server processes to N render server processes.
  if (this->MoveMode == vtkMPIMoveData::PASS_THROUGH &&
      this->MPIMToNSocketConnection)
    {
    if (this->Server == vtkMPIMoveData::DATA_SERVER)
      {
      this->DataServerAllToN(input,output,
                      this->MPIMToNSocketConnection->GetNumberOfConnections());
      this->DataServerSendToRenderServer(output);
      output->Initialize();
      return 1;
      }
    if (this->Server == vtkMPIMoveData::RENDER_SERVER)
      {
      this->RenderServerReceiveFromDataServer(output);
      return 1;
      }
    // Client does nothing.
    return 1;
    }

  // Duplicate with no RenderServer.(Tile rendering on data server and client).
  // GatherAll on data server.
  // Data server process 0 sends data to client.
  if (this->MoveMode == vtkMPIMoveData::CLONE &&
      this->MPIMToNSocketConnection ==0)
    {
    if (this->Server == vtkMPIMoveData::DATA_SERVER)
      {
      this->DataServerGatherAll(input, output);
      this->DataServerSendToClient(output);
      return 1;
      }
    if (this->Server == vtkMPIMoveData::CLIENT)
      {
      this->ClientReceiveFromDataServer(output);
      return 1;
      }
    }

  // Duplicate and RenderServer(Tile rendering on rendering server and client).
  // GatherToZero on data server.
  // Data server process 0 sends to client
  // Data server process 0 sends to render server 0
  // Render server process 0 broad casts to all render server processes.
  if (this->MoveMode == vtkMPIMoveData::CLONE &&
      this->MPIMToNSocketConnection)
    {
    if (this->Server == vtkMPIMoveData::DATA_SERVER)
      {
      this->DataServerGatherToZero(input, output);
      this->DataServerSendToClient(output);
      this->DataServerZeroSendToRenderServerZero(output);
      return 1;
      }
    if (this->Server == vtkMPIMoveData::CLIENT)
      {
      this->ClientReceiveFromDataServer(output);
      return 1;
      }
    if (this->Server == vtkMPIMoveData::RENDER_SERVER)
      {
      this->RenderServerZeroReceiveFromDataServerZero(output);
      this->RenderServerZeroBroadcast(output);
      }
    }

  // Collect and data server or render server (client rendering).
  // GatherToZero on data server.
  // Data server process 0 sends data to client.
  if (this->MoveMode == vtkMPIMoveData::COLLECT)
    {
    if (this->Server == vtkMPIMoveData::DATA_SERVER)
      {
      this->DataServerGatherToZero(input, output);
      this->DataServerSendToClient(output);
      return 1;
      }
    if (this->Server == vtkMPIMoveData::CLIENT)
      {
      this->ClientReceiveFromDataServer(output);
      return 1;
      }
    // Render server does nothing
    return 1;
    }

  if (this->MoveMode == vtkMPIMoveData::COLLECT_AND_PASS_THROUGH)
    {
    if (this->MPIMToNSocketConnection == 0)
      {
      // In client-server mode without render server.
      if (this->Server == vtkMPIMoveData::DATA_SERVER)
        {
        this->DataServerGatherToZero(input, output);
        this->DataServerSendToClient(output);
        output->Initialize();
        output->ShallowCopy(input);
        return 1;
        }
      if (this->Server == vtkMPIMoveData::CLIENT)
        {
        this->ClientReceiveFromDataServer(output);
        return 1;
        }
      }
    else
      {
      // in client-dataserver-renderserver mode.
      if (this->Server == vtkMPIMoveData::DATA_SERVER)
        {
        // Pass Through
        this->DataServerAllToN(input,output,
          this->MPIMToNSocketConnection->GetNumberOfConnections());
        this->DataServerSendToRenderServer(output);
        output->Initialize();

        // Collect to client.
        this->DataServerGatherToZero(input, output);
        this->DataServerSendToClient(output);
        output->Initialize();
        return 1;
        }
      if (this->Server == vtkMPIMoveData::RENDER_SERVER)
        {
        this->RenderServerReceiveFromDataServer(output);
        return 1;
        }
      if (this->Server == vtkMPIMoveData::CLIENT)
        {
        this->ClientReceiveFromDataServer(output);
        return 1;
        }
      }
    }

  return 1;
}

//-----------------------------------------------------------------------------
// Use LANL filter to redistribute the data.
// We will marshal more than once, but that is OK.
void vtkMPIMoveData::DataServerAllToN(vtkDataObject* inData,
                                      vtkDataObject* outData, int n)
{
  vtkMultiProcessController* controller = this->Controller;
  vtkPolyData* input = vtkPolyData::SafeDownCast(inData);
  vtkPolyData* output = vtkPolyData::SafeDownCast(outData);
  int m;

  if (controller == 0)
    {
    vtkErrorMacro("Missing controller.");
    return;
    }

  m = this->Controller->GetNumberOfProcesses();
  if (n > m)
    {
    vtkWarningMacro("Too many render servers.");
    n = m;
    }
  if (input == 0 || output == 0)
    {
    vtkErrorMacro("All to N only works for poly data currently.");
    return;
    }

  if (n == m)
    {
    output->ShallowCopy(input);
    }

  // Perform the M to N operation.
#ifdef VTK_USE_MPI
  vtkPolyData* tmp;
   vtkAllToNRedistributePolyData* AllToN = NULL;
   vtkPolyData* inputCopy = vtkPolyData::New();
   inputCopy->ShallowCopy(input);
   AllToN = vtkAllToNRedistributePolyData::New();
   AllToN->SetController(controller);
   AllToN->SetNumberOfProcesses(n);
   AllToN->SetInput(inputCopy);
   inputCopy->Delete();
   tmp = AllToN->GetOutput();
   tmp->SetUpdateNumberOfPieces(this->UpdateNumberOfPieces);
   tmp->SetUpdatePiece(this->UpdatePiece);
   tmp->Update();
   output->ShallowCopy(tmp);
   AllToN->Delete();
   AllToN= 0;
#endif
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DataServerGatherAll(vtkDataObject* input,
                                         vtkDataObject* output)
{
  int numProcs= this->Controller->GetNumberOfProcesses();

  if (numProcs <= 1)
    {
    output->ShallowCopy(input);
    return;
    }

#ifdef VTK_USE_MPI
  int idx;
  vtkMPICommunicator* com = vtkMPICommunicator::SafeDownCast(
                                         this->Controller->GetCommunicator());

  if (com == 0)
    {
    vtkErrorMacro("MPICommunicator neededfor this operation.");
    return;
    }
  this->ClearBuffer();
  this->MarshalDataToBuffer(input);

  // Save a copy of the buffer so we can receive into the buffer.
  // We will be responsiblefor deleting the buffer.
  // This assumes one buffer. MashalData will produce only one buffer
  // One data set, one buffer.
  vtkIdType inBufferLength = this->BufferTotalLength;
  char *inBuffer = this->Buffers;
  this->Buffers = NULL;
  this->ClearBuffer();

  // Allocate arrays used by the AllGatherV call.
  this->BufferLengths = new vtkIdType[numProcs];
  this->BufferOffsets = new vtkIdType[numProcs];

  // Compute the degenerate input offsets and lengths.
  // Broadcast our size to all other processes.
  com->AllGather(&inBufferLength, this->BufferLengths, 1);

  // Compute the displacements.
  this->BufferTotalLength = 0;
  for (idx = 0; idx < numProcs; ++idx)
    {
    this->BufferOffsets[idx] = this->BufferTotalLength;
    this->BufferTotalLength += this->BufferLengths[idx];
    }
  // Gather the marshaled data sets from all procs.
  this->NumberOfBuffers = numProcs;
  this->Buffers = new char[this->BufferTotalLength];
  com->AllGatherV(inBuffer, this->Buffers, inBufferLength,
                  this->BufferLengths, this->BufferOffsets);

  this->ReconstructDataFromBuffer(output);

  //int fixme; // Do not clear buffers here
  this->ClearBuffer();
#endif
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DataServerGatherToZero(vtkDataObject* input,
                                            vtkDataObject* output)
{
  int numProcs= this->Controller->GetNumberOfProcesses();
  if (numProcs == 1)
    {
    output->ShallowCopy(input);
    return;
    }

    vtkTimerLog::MarkStartEvent("Dataserver gathering to 0");

#ifdef VTK_USE_MPI
  int idx;
  int myId= this->Controller->GetLocalProcessId();
  vtkMPICommunicator* com = vtkMPICommunicator::SafeDownCast(
                                         this->Controller->GetCommunicator());

  if (com == 0)
    {
    vtkErrorMacro("MPICommunicator neededfor this operation.");
    return;
    }
  this->ClearBuffer();
  this->MarshalDataToBuffer(input);

  // Save a copy of the buffer so we can receive into the buffer.
  // We will be responsiblefor deleting the buffer.
  // This assumes one buffer. MashalData will produce only one buffer
  // One data set, one buffer.
  vtkIdType inBufferLength = this->BufferTotalLength;
  char *inBuffer = this->Buffers;
  this->Buffers = NULL;
  this->ClearBuffer();

  if (myId == 0)
    {
    // Allocate arrays used by the AllGatherV call.
    this->BufferLengths = new vtkIdType[numProcs];
    this->BufferOffsets = new vtkIdType[numProcs];
    }

  // Compute the degenerate input offsets and lengths.
  // Broadcast our size to process 0.
  com->Gather(&inBufferLength, this->BufferLengths, 1, 0);

  // Compute the displacements.
  this->BufferTotalLength = 0;
  if (myId == 0)
    {
    for (idx = 0; idx < numProcs; ++idx)
      {
      this->BufferOffsets[idx] = this->BufferTotalLength;
      this->BufferTotalLength += this->BufferLengths[idx];
      }
    // Gather the marshaled data sets to 0.
    this->Buffers = new char[this->BufferTotalLength];
    }
  com->GatherV(inBuffer, this->Buffers, inBufferLength,
                  this->BufferLengths, this->BufferOffsets, 0);
  this->NumberOfBuffers = numProcs;

  if (myId == 0)
    {
    this->ReconstructDataFromBuffer(output);
    }

  //int fixme; // Do not clear buffers here
  this->ClearBuffer();

  delete [] inBuffer;
  inBuffer = NULL;
#endif

  vtkTimerLog::MarkEndEvent("Dataserver gathering to 0");
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DataServerSendToRenderServer(vtkDataObject* output)
{
  vtkSocketCommunicator* com =
      this->MPIMToNSocketConnection->GetSocketCommunicator();

  if (com == 0)
    {
    // Some data server may not have sockets because there are more data
    // processes than render server processes.
    return;
    }

  //int fixme;
  // We might be able to eliminate this marshal.
  this->ClearBuffer();
  this->MarshalDataToBuffer(output);

  com->Send(&(this->NumberOfBuffers), 1, 1, 23480);
  com->Send(this->BufferLengths, this->NumberOfBuffers, 1, 23481);
  com->Send(this->Buffers, this->BufferTotalLength, 1, 23482);
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::RenderServerReceiveFromDataServer(vtkDataObject* output)
{
  vtkSocketCommunicator* com =
      this->MPIMToNSocketConnection->GetSocketCommunicator();

  if (com == 0)
    {
    vtkErrorMacro("All render server processes should have sockets.");
    return;
    }

  this->ClearBuffer();
  com->Receive(&(this->NumberOfBuffers), 1, 1, 23480);
  this->BufferLengths = new vtkIdType[this->NumberOfBuffers];
  com->Receive(this->BufferLengths, this->NumberOfBuffers, 1, 23481);
  // Compute additional buffer information.
  this->BufferOffsets = new vtkIdType[this->NumberOfBuffers];
  this->BufferTotalLength = 0;
  for (int idx = 0; idx < this->NumberOfBuffers; ++idx)
    {
    this->BufferOffsets[idx] = this->BufferTotalLength;
    this->BufferTotalLength += this->BufferLengths[idx];
    }
  this->Buffers = new char[this->BufferTotalLength];
  com->Receive(this->Buffers, this->BufferTotalLength, 1, 23482);

  //int fixme;  // Can we avoid this?
  this->ReconstructDataFromBuffer(output);
  this->ClearBuffer();
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DataServerZeroSendToRenderServerZero(vtkDataObject* data)
{
  int myId = this->Controller->GetLocalProcessId();

  if (myId == 0)
    {
    vtkSocketCommunicator* com =
        this->MPIMToNSocketConnection->GetSocketCommunicator();

    if (com == 0)
      {
      // Proc 0 (at least) should have a communicator.
      vtkErrorMacro("Missing socket connection.");
      return;
      }

    //int fixme;
    // We might be able to eliminate this marshal.
    this->ClearBuffer();
    this->MarshalDataToBuffer(data);
    com->Send(&(this->NumberOfBuffers), 1, 1, 23480);
    com->Send(this->BufferLengths, this->NumberOfBuffers, 1, 23481);
    com->Send(this->Buffers, this->BufferTotalLength, 1, 23482);
    this->ClearBuffer();
    }
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::RenderServerZeroReceiveFromDataServerZero(vtkDataObject* data)
{
  int myId = this->Controller->GetLocalProcessId();

  if (myId == 0)
    {
    vtkSocketCommunicator* com =
        this->MPIMToNSocketConnection->GetSocketCommunicator();

    if (com == 0)
      {
      vtkErrorMacro("All render server processes should have sockets.");
      return;
      }

    this->ClearBuffer();
    com->Receive(&(this->NumberOfBuffers), 1, 1, 23480);
    this->BufferLengths = new vtkIdType[this->NumberOfBuffers];
    com->Receive(this->BufferLengths, this->NumberOfBuffers, 1, 23481);
    // Compute additional buffer information.
    this->BufferOffsets = new vtkIdType[this->NumberOfBuffers];
    this->BufferTotalLength = 0;
    for (int idx = 0; idx < this->NumberOfBuffers; ++idx)
      {
      this->BufferOffsets[idx] = this->BufferTotalLength;
      this->BufferTotalLength += this->BufferLengths[idx];
      }
    this->Buffers = new char[this->BufferTotalLength];
    com->Receive(this->Buffers, this->BufferTotalLength, 1, 23482);

    //int fixme;  // Can we avoid this?
    this->ReconstructDataFromBuffer(data);
    this->ClearBuffer();
    }
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::DataServerSendToClient(vtkDataObject* output)
{
  int myId = this->Controller->GetLocalProcessId();

  if (myId == 0)
    {
    vtkTimerLog::MarkStartEvent("Dataserver sending to client");

    vtkSmartPointer<vtkDataObject> tosend = output;
    if (this->DeliverOutlineToClient)
      {
      // reduce data using outline filter.
      if (output->IsA("vtkPolyData"))
        {
        vtkDataSet* clone = vtkPolyData::SafeDownCast(output)->NewInstance();
        clone->ShallowCopy(output);

        vtkOutlineFilter* filter = vtkOutlineFilter::New();
        filter->SetInput(clone);
        filter->Update();
        tosend = filter->GetOutput();
        filter->Delete();
        clone->Delete();
        }
      else
        {
        vtkErrorMacro("DeliverOutlineToClient can only be used for vtkPolyData.");
        }
      }

    this->ClearBuffer();
    this->MarshalDataToBuffer(tosend);
    this->ClientDataServerSocketController->Send(
                                     &(this->NumberOfBuffers), 1, 1, 23490);
    this->ClientDataServerSocketController->Send(this->BufferLengths,
                                     this->NumberOfBuffers, 1, 23491);
    this->ClientDataServerSocketController->Send(this->Buffers,
                                     this->BufferTotalLength, 1, 23492);
    this->ClearBuffer();
    vtkTimerLog::MarkEndEvent("Dataserver sending to client");
    }
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::ClientReceiveFromDataServer(vtkDataObject* output)
{
  vtkCommunicator* com = 0;
  com = this->ClientDataServerSocketController->GetCommunicator();
  if (com == 0)
    {
    vtkErrorMacro("Missing socket controler on cleint.");
    return;
    }

  this->ClearBuffer();
  com->Receive(&(this->NumberOfBuffers), 1, 1, 23490);
  this->BufferLengths = new vtkIdType[this->NumberOfBuffers];
  com->Receive(this->BufferLengths, this->NumberOfBuffers,
                                  1, 23491);
  // Compute additional buffer information.
  this->BufferOffsets = new vtkIdType[this->NumberOfBuffers];
  this->BufferTotalLength = 0;
  for (int idx = 0; idx < this->NumberOfBuffers; ++idx)
    {
    this->BufferOffsets[idx] = this->BufferTotalLength;
    this->BufferTotalLength += this->BufferLengths[idx];
    }
  this->Buffers = new char[this->BufferTotalLength];
  com->Receive(this->Buffers, this->BufferTotalLength,
                                  1, 23492);
  this->ReconstructDataFromBuffer(output);
  this->ClearBuffer();
}


//-----------------------------------------------------------------------------
void vtkMPIMoveData::RenderServerZeroBroadcast(vtkDataObject* data)
{
  (void)data; // shut up warning
  int numProcs= this->Controller->GetNumberOfProcesses();
  if (numProcs <= 1)
    {
    return;
    }

#ifdef VTK_USE_MPI
  int myId= this->Controller->GetLocalProcessId();

  vtkMPICommunicator* com = vtkMPICommunicator::SafeDownCast(
                                         this->Controller->GetCommunicator());

  if (com == 0)
    {
    vtkErrorMacro("MPICommunicator neededfor this operation.");
    return;
    }

  int bufferLength = 0;
  if (myId == 0)
    {
    this->ClearBuffer();
    this->MarshalDataToBuffer(data);
    bufferLength = this->BufferLengths[0];
    }

  // Broadcast the size of the buffer.
  com->Broadcast(&bufferLength, 1, 0);

  // Allocate buffers for all receiving nodes.
  if (myId != 0)
    {
    this->NumberOfBuffers = 1;
    this->BufferLengths = new vtkIdType[1];
    this->BufferLengths[0] = bufferLength;
    this->BufferOffsets = new vtkIdType[1];
    this->BufferOffsets[0] = 0;
    this->BufferTotalLength = this->BufferLengths[0];
    this->Buffers = new char[bufferLength];
    }

  // Broadcast the buffer.
  com->Broadcast(this->Buffers, bufferLength, 0);

  // Reconstruct the output on nodes other than 0.
  if (myId != 0)
    {
    this->ReconstructDataFromBuffer(data);
    }

  this->ClearBuffer();
#endif
}




//-----------------------------------------------------------------------------
void vtkMPIMoveData::ClearBuffer()
{
  this->NumberOfBuffers = 0;
  if (this->BufferLengths)
    {
    delete [] this->BufferLengths;
    this->BufferLengths = 0;
    }
  if (this->BufferOffsets)
    {
    delete [] this->BufferOffsets;
    this->BufferOffsets = 0;
    }
  if (this->Buffers)
    {
    delete [] this->Buffers;
    this->Buffers = 0;
    }
  this->BufferTotalLength = 0;
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::MarshalDataToBuffer(vtkDataObject* data)
{
  vtkDataSet* dataSet = vtkDataSet::SafeDownCast(data);
  vtkImageData* imageData = vtkImageData::SafeDownCast(data);
  vtkGraph* graph = vtkGraph::SafeDownCast(data);

  // Protect from empty data.
  if ((dataSet && dataSet->GetNumberOfPoints() == 0) || (graph && graph->GetNumberOfVertices() == 0))
    {
    this->NumberOfBuffers = 0;
    }

  // Copy input to isolate reader from the pipeline.
  vtkDataWriter* writer = 0;
  if (dataSet)
    {
    vtkDataSet* d = dataSet->NewInstance();
    d->CopyStructure(dataSet);
    d->GetPointData()->PassData(dataSet->GetPointData());
    d->GetCellData()->PassData(dataSet->GetCellData());
    writer = vtkDataSetWriter::New();
    writer->SetInput(d);
    d->Delete();
    if (imageData)
      {
      // We add the image extents to the header, since the writer doesn't preserve
      // the extents.
      int *extent = imageData->GetExtent();
      double* origin = imageData->GetOrigin();
      vtksys_ios::ostringstream stream;
      stream << "EXTENT " << extent[0] << " " <<
        extent[1] << " " <<
        extent[2] << " " <<
        extent[3] << " " <<
        extent[4] << " " <<
        extent[5];
      stream << " ORIGIN: " << origin[0] << " " << origin[1] << " " << origin[2];
      writer->SetHeader(stream.str().c_str());
      }
    }
  if (graph)
    {
    vtkGraph* g = graph->NewInstance();
    g->ShallowCopy(graph);
    writer = vtkGraphWriter::New();
    writer->SetInput(g);
    g->Delete();
    }
  writer->SetFileTypeToBinary();
  writer->WriteToOutputStringOn();
  writer->Write();

  char* buffer =NULL;
  vtkIdType buffer_length = 0;

  if (vtkMPIMoveData::UseZLibCompression)
    {
    vtkTimerLog::MarkStartEvent("Zlib compress");
    // Use z-lib compression.
    uLongf out_size =compressBound(writer->GetOutputStringLength());
    buffer = new char[out_size + 8]; 
    memcpy(buffer, "zlib0000", 8);

    compress2(reinterpret_cast<Bytef*>(buffer + 8), 
      &out_size,
      reinterpret_cast<const Bytef*>(writer->GetOutputString()),
      writer->GetOutputStringLength(), /* compression_level */ Z_DEFAULT_COMPRESSION);
    vtkTimerLog::MarkEndEvent("Zlib compress");
    int in_size = static_cast<int>(writer->GetOutputStringLength());
    for (int cc=0; cc < 4; cc++)
      {
      // the first 4 bytes in the header are "zlib" which helps the receiver
      // identify that zlib compression has been used.
      // the next 4 bytes are the original length since zlib doesn't provide
      // that to the receiver.
      buffer[4+cc] = (in_size & 0x0ff);
      in_size = in_size >> 8;
      }
    buffer_length = out_size + 8;
    }
  else
    {
    buffer_length = writer->GetOutputStringLength();
    buffer = writer->RegisterAndGetOutputString();
    }

  // Get string.
  this->NumberOfBuffers = 1;
  this->BufferLengths = new vtkIdType[1];
  this->BufferLengths[0] = buffer_length;
  this->BufferOffsets = new vtkIdType[1];
  this->BufferOffsets[0] = 0;
  this->Buffers = buffer;
  this->BufferTotalLength = this->BufferLengths[0];

  writer->Delete();
  writer = 0;
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::ReconstructDataFromBuffer(vtkDataObject* data)
{
  vtkDataSet *dataSet = vtkDataSet::SafeDownCast(data);
  vtkPolyData *polyData = vtkPolyData::SafeDownCast(data);
  vtkUnstructuredGrid *unstructuredGrid = vtkUnstructuredGrid::SafeDownCast(data);
  vtkImageData *imageData = vtkImageData::SafeDownCast(data);
  vtkGraph *graph = vtkGraph::SafeDownCast(data);

  if (this->NumberOfBuffers == 0 || this->Buffers == 0)
    {
    data->Initialize();
    return;
    }

  // PolyData and Unstructured grid need different append filters.
  vtkAppendPolyData* appendPd = NULL;
  vtkAppendFilter*   appendUg = NULL;
  vtkImageAppend* appendId = NULL;
  vtkMergeGraphs* mergeGraphs = NULL;
  if (this->NumberOfBuffers > 1)
    {
    if (polyData)
      {
      appendPd = vtkAppendPolyData::New();
      }
    else if (unstructuredGrid)
      {
      appendUg = vtkAppendFilter::New();
      }
    else if (imageData)
      {
      appendId = vtkImageAppend::New();
      appendId->PreserveExtentsOn();
      }
    else if (graph)
      {
      mergeGraphs = vtkMergeGraphs::New();
      }
    else
      {
      vtkErrorMacro("This filter only handles unstructured data.");
      return;
      }
    }

  for (int idx = 0; idx < this->NumberOfBuffers; ++idx)
    {
    char* bufferArray = this->Buffers+this->BufferOffsets[idx];
    vtkIdType bufferLength = this->BufferLengths[idx];

    char* realBuffer = 0;
    if (bufferLength > 4 && strncmp(bufferArray, "zlib", 4) == 0)
      {
      // sender used zlib compression. Decompress it.
      vtkIdType compressed_length = bufferLength - 8; // remove the zlib header.
      vtkIdType uncompressed_length = 0;
      for (int cc=0; cc < 4; cc++)
        {
        uncompressed_length = uncompressed_length | 
          ((0xff & (bufferArray[4+cc])) <<8 *cc);
        }

      // using zlib compression.
      realBuffer = new char[uncompressed_length];
      uLongf destLen = uncompressed_length;
      vtkTimerLog::MarkStartEvent("Zlib uncompress");
      uncompress(reinterpret_cast<Bytef*>(realBuffer), &destLen,
        reinterpret_cast<const Bytef*>(bufferArray+8), compressed_length);
      vtkTimerLog::MarkEndEvent("Zlib uncompress");

      bufferArray = realBuffer;
      bufferLength = uncompressed_length;
      }

    // Setup a reader.
    vtkDataReader *reader = NULL;
    if (dataSet)
      {
      reader = vtkDataSetReader::New();
      }
    else if (graph)
      {
      reader = vtkGraphReader::New();
      }
    reader->ReadFromInputStringOn();

    int extent[6]= {0, 0, 0, 0, 0, 0};
    float origin[3] = {0, 0, 0};
    bool extentAvailable = false;
    vtkCharArray* mystring = vtkCharArray::New();
    mystring->SetArray(bufferArray, bufferLength, 1);
    reader->SetInputArray(mystring);
    reader->Modified(); // For append loop
    reader->Update();

    if (imageData)
      {
      sscanf(reader->GetHeader(),
        "EXTENT %d %d %d %d %d %d ORIGIN %f %f %f", &extent[0], &extent[1],
        &extent[2], &extent[3], &extent[4], &extent[5],
        &origin[0], &origin[1], &origin[2]);
      extentAvailable = true;
      }

    if (appendPd)
      {
      appendPd->AddInput(vtkPolyData::SafeDownCast(reader->GetOutputDataObject(0)));
      }
    else if (appendUg)
      {
      appendUg->AddInput(vtkUnstructuredGrid::SafeDownCast(reader->GetOutputDataObject(0)));
      }
    else if (appendId)
      {
      vtkImageData* curInput = vtkImageData::SafeDownCast(
        reader->GetOutputDataObject(0));
      if (curInput->GetNumberOfPoints() >0)
        {
        if (extentAvailable)
          {
          vtkImageData* clone = vtkImageData::New();
          clone->ShallowCopy(curInput);
          clone->SetOrigin(origin[0], origin[1], origin[2]);
          clone->SetExtent(extent);
          appendId->AddInput(clone);
          clone->Delete();
          }
        else
          {
          appendId->AddInput(curInput);
          }
        }
      }
    else if (mergeGraphs)
      {
      if (!mergeGraphs->GetInputDataObject(0, 0))
        {
        mergeGraphs->SetInput(0, reader->GetOutputDataObject(0));
        }
      else
        {
        mergeGraphs->SetInput(1, reader->GetOutputDataObject(0));
        mergeGraphs->Update();
        vtkGraph* output = mergeGraphs->GetOutput();
        vtkGraph* outputCopy = output->NewInstance();
        outputCopy->ShallowCopy(output);
        mergeGraphs->SetInput(0, outputCopy);
        }
      }
    else if (dataSet)
      {
      vtkDataSet* out = vtkDataSet::SafeDownCast(reader->GetOutputDataObject(0));
      dataSet->CopyStructure(out);
      dataSet->GetPointData()->PassData(out->GetPointData());
      dataSet->GetCellData()->PassData(out->GetCellData());
      }
    else if (graph)
      {
      vtkGraph* out = vtkGraph::SafeDownCast(reader->GetOutputDataObject(0));
      graph->ShallowCopy(out);
      }
    mystring->Delete();
    mystring = 0;
    reader->Delete();
    reader = NULL;
    delete [] realBuffer;
    realBuffer = 0;
    }

  if (appendPd)
    {
    vtkDataSet* out = appendPd->GetOutput();
    out->Update();
    polyData->CopyStructure(out);
    polyData->GetPointData()->PassData(out->GetPointData());
    polyData->GetCellData()->PassData(out->GetCellData());
    appendPd->Delete();
    appendPd = NULL;
    }
  if (appendUg)
    {
    vtkDataSet* out = appendUg->GetOutput();
    out->Update();
    unstructuredGrid->CopyStructure(out);
    unstructuredGrid->GetPointData()->PassData(out->GetPointData());
    unstructuredGrid->GetCellData()->PassData(out->GetCellData());
    appendUg->Delete();
    appendUg = NULL;
    }
  if (appendId)
    {
    appendId->Update();
    vtkDataSet* out = appendId->GetOutput();
    imageData->CopyStructure(out);
    imageData->GetPointData()->PassData(out->GetPointData());
    imageData->GetCellData()->PassData(out->GetCellData());
    appendId->Delete();
    appendId = NULL;
    }
  if (mergeGraphs)
    {
    vtkGraph* out = vtkGraph::SafeDownCast(mergeGraphs->GetInputDataObject(0, 0));
    graph->ShallowCopy(out);
    mergeGraphs->Delete();
    mergeGraphs = NULL;
    }
}

//-----------------------------------------------------------------------------
void vtkMPIMoveData::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
  os << indent << "NumberOfBuffers: " << this->NumberOfBuffers << endl;
  os << indent << "Server: " << this->Server << endl;
  os << indent << "MoveMode: " << this->MoveMode << endl;
  os << indent << "DeliverOutlineToClient : "
    << this->DeliverOutlineToClient << endl;
  os << indent << "OutputDataType: ";
  if (this->OutputDataType == VTK_POLY_DATA)
    {
    os << "VTK_POLY_DATA";
    }
  else if (this->OutputDataType == VTK_UNSTRUCTURED_GRID)
    {
    os << "VTK_UNSTRUCTURED_GRID";
    }
  else if (this->OutputDataType == VTK_IMAGE_DATA)
    {
    os << "VTK_IMAGE_DATA";
    }
  else if (this->OutputDataType == VTK_DIRECTED_GRAPH)
    {
    os << "VTK_DIRECTED_GRAPH";
    }
  else if (this->OutputDataType == VTK_UNDIRECTED_GRAPH)
    {
    os << "VTK_UNDIRECTED_GRAPH";
    }
  else
    {
    os << "Unrecognized output type " << this->OutputDataType;
    }
  os << endl;
  //os << indent << "MToN
}

