/*
 * Simple file i/o operations over LEVCAN protocol, client side
 * levcan_fileclient.c
 *
 *  Created on: 10 July 2018
 *      Author: Vasiliy Sukhoparov (VasiliSk)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "levcan_fileclient.h"
#include "levcan_filedef.h"

#if LEVCAN_OBJECT_DATASIZE < 16
#error "Too small LEVCAN_OBJECT_DATASIZE size for file io!"
#endif
//extern functions
extern void *lcdelay(uint32_t time);
//private functions
LC_FileResult_t lc_client_sendwait(void* data, uint16_t size, void* sender_node, int16_t* retid);
//private variables
volatile fRead_t rxtoread[LEVCAN_MAX_OWN_NODES] = { 0 };
volatile uint32_t fpos[LEVCAN_MAX_OWN_NODES] = { 0 };
volatile uint8_t fnode[LEVCAN_MAX_OWN_NODES] = { [0 ... (LEVCAN_MAX_OWN_NODES - 1)] = LC_Broadcast_Address };
volatile fOpAck_t rxack[LEVCAN_MAX_OWN_NODES] = { 0 };

void proceedFileClient(LC_NodeDescription_t* node, LC_Header_t header, void* data, int32_t size) {
	if (size < 2)
		return;
	uint16_t* op = data;
	switch (*op) {
	case fOpAck: {
		if (sizeof(fOpAck_t) == size) {
			fOpAck_t* fac = data;
			int id = LC_GetMyNodeIndex(node);
			if (id >= 0) {
				rxack[id].Error = fac->Error;
				rxack[id].Operation = fac->Operation; //finish
				rxack[id].Position = fac->Position;
			}
		}
	}
		break;
	case fOpData: {
		fOpData_t* fop = data;
		int id = LC_GetMyNodeIndex(node);
		//check if it is okay
		if (id >= 0) {
			if (rxtoread[id].Buffer != 0 && fop->TotalRead <= rxtoread[id].ReadBytes && rxtoread[id].Position == UINT32_MAX
					&& size == (int32_t) (fop->TotalRead + sizeof(fOpData_t))) {
				memcpy(rxtoread[id].Buffer, fop->Data, fop->TotalRead);
				rxtoread[id].ReadBytes = fop->TotalRead;
				rxtoread[id].Error = fop->Error;
				rxtoread[id].Position = fop->Position; //trigger
			} else {
				rxtoread[id].Error = LC_FR_NetworkError; //data error
				rxtoread[id].ReadBytes = 0;
				rxtoread[id].Position = 0;
			}
		}
	}
		break;
	}
}

/// Open/Create a file
/// @param name File name
/// @param mode Mode flags LC_FileAccess_t
/// @param sender_node Own network node
/// @param server_node Server id, can be LC_Broadcast_Address to find first one
/// @return LC_FileResult_t
LC_FileResult_t LC_FileOpen(char* name, LC_FileAccess_t mode, void* sender_node, uint8_t server_node) {
	//look for any server node
	if (server_node == LC_Broadcast_Address)
		server_node = LC_FindFileServer(0).NodeID;

	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return LC_FR_NodeOffline;
	//save server
	fnode[id] = server_node;
	//create buffer
	int datasize = sizeof(fOpOpen_t) + strlen(name) + 1;
	char datasend[datasize];
	memset(datasend, 0, datasize);
	//buffer tx file operation
	fOpOpen_t* openf = (fOpOpen_t*) datasend;
	openf->Operation = fOpOpen;
	strcpy(&openf->Name[0], name);
	openf->Mode = mode;
	int16_t reid;
	LC_FileResult_t ret = lc_client_sendwait(openf, datasize, sender_node, &reid);
	if (ret != LC_FR_Ok)
		fnode[id] = LC_Broadcast_Address; //reset server
	return ret;
}

/// Read data from the file
/// @param buffer Buffer to store read data
/// @param btr Number of bytes to read
/// @param br Number of bytes read
/// @param sender_node Own network node
/// @param server_node Server id, can be LC_Broadcast_Address to find first one
/// @return LC_FileResult_t
LC_FileResult_t LC_FileRead(char* buffer, uint32_t btr, uint32_t* br, void* sender_node) {
	uint16_t attempt;
	LC_NodeShortName_t server;

	if (br == 0 || buffer == 0)
		return LC_FR_InvalidParameter;
	//reset
	*br = 0;
	attempt = 0;

	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return LC_FR_NodeOffline;
	//look for any server node
	if (fnode[id] == LC_Broadcast_Address)
		return LC_FR_FileNotOpened;
	else
		server = LC_GetNode(fnode[id]);
	//checks
	if (server.FileServer == 0 || server.NodeID == LC_Broadcast_Address)
		return LC_FR_NodeOffline;

	fOpRead_t readf;
	readf.Operation = fOpRead;
	LC_ObjectRecord_t rec = { 0 };
	rec.Attributes.TCP = 1;
	rec.Attributes.Priority = LC_Priority_Low;
	rec.NodeID = server.NodeID;
	rec.Address = &readf;
	rec.Size = sizeof(fOpRead_t);
	LC_FileResult_t ret = LC_FR_Ok;

	for (uint32_t position = 0; position < btr;) {
		uint32_t toreadnow = btr - position;
		uint32_t globalpos = position + fpos[id];
		//finish?
		if (toreadnow == 0)
			return LC_Ok;
		if (toreadnow > LEVCAN_OBJECT_DATASIZE - sizeof(fOpData_t))
			toreadnow = LEVCAN_OBJECT_DATASIZE - sizeof(fOpData_t);
		if (toreadnow > INT16_MAX)
			toreadnow = INT16_MAX;
		//Prepare receiver
		rxtoread[id].Buffer = &buffer[position];
		rxtoread[id].Error = 0;
		rxtoread[id].Position = -1;
		rxtoread[id].ReadBytes = toreadnow;
		readf.ToBeRead = toreadnow;
		readf.Position = globalpos; //add global position

		LC_Return_t sr = LC_SendMessage(sender_node, &rec, LC_SYS_FileClient);
		//send error?
		if (sr) {
			if (sr == LC_BufferFull)
				ret = LC_FR_NetworkBusy;
			else if (sr == LC_MallocFail)
				ret = LC_FR_MemoryFull;
			else
				ret = LC_FR_NetworkError;
			break;
		}
		//wait 500ms
		for (int time = 0; time < LEVCAN_FILE_TIMEOUT; time++) {
			lcdelay(1);
			if (rxtoread[id].Position != UINT32_MAX)
				break;
		}
		//just make some checks
		if (rxtoread[id].Position != UINT32_MAX && rxtoread[id].Position == globalpos && rxtoread[id].ReadBytes <= toreadnow) {
			//received something, data already filled, move pointers
			position += rxtoread[id].ReadBytes;
			*br += rxtoread[id].ReadBytes;
			attempt = 0;
			if (rxtoread[id].ReadBytes < toreadnow)
				break; //receive finished

		} else {
			attempt++;
			if (attempt > 3) {
				ret = LC_FR_NetworkTimeout;
				break;
			}
		}
		//got some errs?
		if (rxtoread[id].Error && rxtoread[id].Error != LC_FR_NetworkError) {
			ret = rxtoread[id].Error;
			break;
		}
	}
	//reset buffer
	fpos[id] += *br;
	rxtoread[id].Buffer = 0;
	return ret;
}

LC_FileResult_t LC_FileWrite(const char* buffer, uint32_t btr, uint32_t* br, void* sender_node) {
	uint16_t attempt;
	LC_NodeShortName_t server;

	if (br == 0 || buffer == 0)
		return LC_FR_InvalidParameter;
	//reset
	*br = 0;
	attempt = 0;

	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return LC_FR_NodeOffline;
	//look for any server node
	if (fnode[id] == LC_Broadcast_Address)
		return LC_FR_FileNotOpened;
	else
		server = LC_GetNode(fnode[id]);
	//checks
	if (server.FileServer == 0 || server.NodeID == LC_Broadcast_Address)
		return LC_FR_NodeOffline;

	fOpWrite_t readf;
	readf.Operation = fOpWrite;
	LC_ObjectRecord_t rec = { 0 };
	rec.Attributes.TCP = 1;
	rec.Attributes.Priority = LC_Priority_Low;
	rec.NodeID = server.NodeID;
	rec.Address = &readf;
	rec.Size = sizeof(fOpRead_t);
	//reset
	*br = 0;
	attempt = 0;
	LC_FileResult_t ret = LC_FR_Ok;

	for (uint32_t position = 0; position < btr;) {
		uint32_t toreadnow = btr - position;
		uint32_t globalpos = position + fpos[id];
		//finish?
		if (toreadnow == 0)
			return LC_Ok;
		if (toreadnow > LEVCAN_OBJECT_DATASIZE - sizeof(fOpData_t))
			toreadnow = LEVCAN_OBJECT_DATASIZE - sizeof(fOpData_t);
		if (toreadnow > INT16_MAX)
			toreadnow = INT16_MAX;
		//Prepare receiver
		rxtoread[id].Buffer = &buffer[position];
		rxtoread[id].Error = 0;
		rxtoread[id].Position = -1;
		rxtoread[id].ReadBytes = toreadnow;
		readf.ToBeWrite = toreadnow;
		readf.Position = globalpos; //add global position

		LC_Return_t sr = LC_SendMessage(sender_node, &rec, LC_SYS_FileClient);
		//send error?
		if (sr) {
			if (sr == LC_BufferFull)
				ret = LC_FR_NetworkBusy;
			else if (sr == LC_MallocFail)
				ret = LC_FR_MemoryFull;
			else
				ret = LC_FR_NetworkError;
			break;
		}
		//wait 500ms
		for (int time = 0; time < LEVCAN_FILE_TIMEOUT; time++) {
			lcdelay(1);
			if (rxtoread[id].Position != UINT32_MAX)
				break;
		}
		//just make some checks
		if (rxtoread[id].Position != UINT32_MAX && rxtoread[id].Position == globalpos && rxtoread[id].ReadBytes <= toreadnow) {
			//received something, data already filled, move pointers
			position += rxtoread[id].ReadBytes;
			*br += rxtoread[id].ReadBytes;
			attempt = 0;
			if (rxtoread[id].ReadBytes < toreadnow)
				break; //receive finished

		} else {
			attempt++;
			if (attempt > 3) {
				ret = LC_FR_NetworkTimeout;
				break;
			}
		}
		//got some errs?
		if (rxtoread[id].Error && rxtoread[id].Error != LC_FR_NetworkError) {
			ret = rxtoread[id].Error;
			break;
		}
	}
	//reset buffer
	fpos[id] += *br;
	rxtoread[id].Buffer = 0;
	return ret;
}

///  Move read/write pointer, Expand size
/// @param sender_node Own network node
/// @param server_node Server id, can be LC_Broadcast_Address to find first one
/// @return LC_FileResult_t
LC_FileResult_t LC_FileLseek(void* sender_node) {
	int16_t reid;
	//create buffer
	fOpLseek_t closef;
	closef.Operation = fOpLseek;
	LC_FileResult_t result = lc_client_sendwait(&closef, sizeof(fOpClose_t), sender_node, &reid);
	if (reid >= 0 && rxack[reid].Operation == fOpAck)
		fpos[reid] = rxack[reid].Position; //update position
	return result;
}

/// Get current read/write pointer
/// @param sender_node Own network node
/// @return Pointer to the open file.
uint32_t LC_FileTell(void* sender_node) {
	//checks
	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return LC_NodeOffline;
	return fpos[id];
}

/// Get file size
/// @param sender_node Own network node
/// @return Size of the open file. Can be null if file not opened.
uint32_t LC_FileSize(void* sender_node) {
	int16_t reid;
	//create buffer
	fOpAckSize_t closef;
	closef.Operation = fOpAckSize;
	lc_client_sendwait(&closef, sizeof(fOpAckSize_t), sender_node, &reid);
	if (reid >= 0 && rxack[reid].Operation == fOpAck)
		return rxack[reid].Position; //file size
	return 0;
}

/// Close an open file
/// @param sender_node Own network node
/// @param server_node Server id, can be LC_Broadcast_Address to find first one
/// @return LC_FileResult_t
LC_FileResult_t LC_FileClose(void* sender_node, uint8_t server_node) {
	LC_NodeShortName_t server;
	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return LC_FR_NodeOffline;
	//this node may be already closed here, but let it try again
	if (fnode[id] == LC_Broadcast_Address) {
		//incoming server known?
		if (server_node == LC_Broadcast_Address)
			server = LC_FindFileServer(0); //search
		else
			server = LC_GetNode(server_node); //get it
	} else
		server = LC_GetNode(fnode[id]);
	//checks
	if (server.FileServer == 0 || server.NodeID == LC_Broadcast_Address) {
		fnode[id] = LC_Broadcast_Address; //reset server anyway
		return LC_FR_NodeOffline;
	}
	fnode[id] = server.NodeID;
	int16_t reid;
	//create buffer
	fOpClose_t closef;
	closef.Operation = fOpClose;
	LC_FileResult_t result = lc_client_sendwait(&closef, sizeof(fOpClose_t), sender_node, &reid);
	//reset server
	fnode[id] = LC_Broadcast_Address;
	return result;
}

LC_NodeShortName_t LC_FileGetServer(void* sender_node) {
	const LC_NodeShortName_t nullname = { .NodeID = LC_Broadcast_Address };
	LC_NodeShortName_t server;
	int id = LC_GetMyNodeIndex(sender_node);
	if (id < 0)
		return nullname;
	//look for any server node
	if (fnode[id] == LC_Broadcast_Address) {
		return nullname;
	} else
		server = LC_GetNode(fnode[id]);
	//checks
	if (server.FileServer == 0 || server.NodeID == LC_Broadcast_Address)
		return nullname;

	return server;
}

/// Returns file server short name
/// @param scnt Pointer to stored position for search, can be null
/// @return LC_NodeShortName_t file server
LC_NodeShortName_t LC_FindFileServer(uint16_t* scnt) {
	uint16_t counter = 0;
	LC_NodeShortName_t node;
	if (scnt)
		counter = *scnt;
	while (node.NodeID != LC_Broadcast_Address) {
		node = LC_GetActiveNodes(&counter);
		if (node.FileServer)
			break;
	}
	if (scnt)
		*scnt = counter;
	return node;
}

LC_FileResult_t lc_client_sendwait(void* data, uint16_t size, void* sender_node, int16_t* retid) {
	LC_NodeShortName_t server;
	int id = LC_GetMyNodeIndex(sender_node);
	*retid = id;
	if (id < 0)
		return LC_FR_NodeOffline;
	//reset ACK
	rxack[id] = (fOpAck_t ) { 0 };
	//look for any server node
	if (fnode[id] == LC_Broadcast_Address)
		return LC_FR_FileNotOpened;
	else
		server = LC_GetNode(fnode[id]);
	//checks
	if (server.FileServer == 0 || server.NodeID == LC_Broadcast_Address)
		return LC_FR_NodeOffline;
	//prepare message
	LC_ObjectRecord_t rec = { 0 };
	rec.Address = data;
	rec.Size = size;
	rec.Attributes.TCP = 1;
	rec.Attributes.Priority = LC_Priority_Low;
	rec.NodeID = server.NodeID;
	uint16_t attempt = 0;
	for (attempt = 0; attempt < 3; attempt++) {
		LC_Return_t sr = LC_SendMessage(sender_node, &rec, LC_SYS_FileClient);
		//send error?
		if (sr) {
			if (sr == LC_BufferFull)
				return LC_FR_NetworkBusy;
			if (sr == LC_MallocFail)
				return LC_FR_MemoryFull;
			return LC_FR_NetworkError;
		}
		//wait
		for (int time = 0; time < LEVCAN_FILE_TIMEOUT; time++) {
			lcdelay(1);
			if (rxack[id].Operation == fOpAck)
				return rxack[id].Error; //Finish!
		}
	}

	return LC_FR_NetworkTimeout;
}
