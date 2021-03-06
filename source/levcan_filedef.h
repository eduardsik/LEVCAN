/*
 * levcan_filedef.h
 *
 *  Created on: 16 Aug 2018
 *      Author: Vasiliy Sukhoparov (VasiliSk)
 */

#pragma once

enum {
	fOpNoOp, fOpOpen, fOpRead, fOpWrite, fOpClose, fOpAck, fOpLseek, fOpData, fOpAckSize, fOpOpenDir, fOpReadDir, fOpTruncate
};

typedef struct {
	uint16_t Operation;
	LC_FileAccess_t Mode;
	char Name[];
} fOpOpen_t;

typedef struct {
	uint16_t Operation;
	uint16_t ToBeRead;
	uint32_t Position;
} fOpRead_t;

typedef struct {
	uint16_t Operation;
	uint32_t Position;
} fOpLseek_t;

typedef struct {
	uint16_t Operation;
} fOpOperation_t;

typedef struct {
	uint16_t Operation;
	uint16_t Error;
	uint32_t Position;
} fOpAck_t;

typedef struct {
	uint16_t Operation;
	uint16_t Error;
	uint32_t Position;
	uint16_t TotalBytes;
	char Data[];
} fOpData_t;

typedef struct {
	char* Buffer;
	uint32_t Position;
	uint16_t ReadBytes;
	uint16_t Error;
} fRead_t;

