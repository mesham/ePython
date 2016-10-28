/*
 * Copyright (c) 2015, Nick Brown
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "functions.h"
#include "main.h"
#include "basictokens.h"
#include "interpreter.h"
#include "shared.h"
#include <e-lib.h>

volatile static unsigned int sharedStackEntries=0, localStackEntries=0;
volatile static unsigned char communication_data[6];

static void sendDataToDeviceCore(struct value_defn, int);
static void sendDataToHostProcess(struct value_defn, int);
static struct value_defn recvDataFromHostProcess(int);
static struct value_defn recvDataFromDeviceCore(int);
static struct value_defn sendRecvDataWithHostProcess(struct value_defn, int);
static struct value_defn sendRecvDataWithDeviceCore(struct value_defn, int);
static void performBarrier(volatile e_barrier_t[], e_barrier_t*[]);
static char* copyStringToSharedMemoryAndSetLocation(char*,int);
static struct value_defn doGetInputFromUser();
static int stringCmp(char*, char*);
static void consolidateHeapChunks(char);
static char * allocateChunkInHeapMemory(int, char);

/**
 * Displays a message to the user and waits for the host to have done this
 */
void displayToUser(struct value_defn value) {
	sharedData->core_ctrl[myId].data[0]=value.type;
	char* tempStr=NULL;
	if (value.type == STRING_TYPE) {
		char * v;
		cpy(&v, &value.data, sizeof(char*));
		tempStr=copyStringToSharedMemoryAndSetLocation(v, 1);
	} else {
		cpy(&sharedData->core_ctrl[myId].data[1], value.data, 4);
	}
	sharedData->core_ctrl[myId].core_command=1;
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	if (tempStr != NULL) freeMemoryInHeap(tempStr); // Clears up the temporary memory used
}

/**
 * Checks two strings for equality
 */
int checkStringEquality(struct value_defn str1, struct value_defn str2) {
	char *c_str1, *c_str2;
	cpy(&c_str1, &str1.data, sizeof(char*));
	cpy(&c_str2, &str2.data, sizeof(char*));
	return stringCmp(c_str1, c_str2);
}

/**
 * Requests input from the host with a string to display
 */
struct value_defn getInputFromUserWithString(struct value_defn toDisplay) {
	if (toDisplay.type != STRING_TYPE) raiseError("Can only display strings with input statement");
	sharedData->core_ctrl[myId].data[0]=toDisplay.type;
	char * msg=NULL;
	if (toDisplay.type == STRING_TYPE) {
		char * v;
		cpy(&v, &toDisplay.data, sizeof(char*));
		msg=copyStringToSharedMemoryAndSetLocation(v, 1);
	}
	struct value_defn inputValue=doGetInputFromUser();
	if (msg != NULL) freeMemoryInHeap(msg);
	return inputValue;
}

/**
 * Requests input from the host (no string to display)
 */
struct value_defn getInputFromUser() {
	sharedData->core_ctrl[myId].data[0]=0;
	return doGetInputFromUser();
}

/**
 * Does the copying required to get input from the host, waits until this is ready and then sets the
 * type and data correctly
 */
static struct value_defn doGetInputFromUser() {
	struct value_defn v;
	v.dtype=SCALAR;
	sharedData->core_ctrl[myId].core_command=2;
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	v.type=sharedData->core_ctrl[myId].data[0];
	if (v.type==STRING_TYPE) {
        unsigned int relativeLocation;
        cpy(&relativeLocation, &sharedData->core_ctrl[myId].data[1], sizeof(unsigned int));
        char * ptr=sharedData->core_ctrl[myId].shared_heap_start + relativeLocation;
		cpy(&v.data, &ptr, sizeof(char*));
	} else {
		cpy(v.data, &sharedData->core_ctrl[myId].data[1], 4);
	}
	return v;
}

/**
 * Requests the host to perform some maths operation and blocks on this
 */
struct value_defn performMathsOp(unsigned short operation, struct value_defn value) {
	struct value_defn v;
	if (operation != RANDOM_MATHS_OP) {
		sharedData->core_ctrl[myId].data[0]=value.type;
		cpy(&sharedData->core_ctrl[myId].data[1], value.data, 4);
	}

	sharedData->core_ctrl[myId].core_command=1000+(unsigned int) operation;

	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	v.type=sharedData->core_ctrl[myId].data[0];
	cpy(v.data, &sharedData->core_ctrl[myId].data[1], 4);
	v.dtype=SCALAR;
	return v;
}

/**
 * String concatenation performed on the host (and any needed data transformations)
 */
struct value_defn performStringConcatenation(struct value_defn v1, struct value_defn v2) {
	struct value_defn v;
	v.type=STRING_TYPE;
	v.dtype=SCALAR;

	sharedData->core_ctrl[myId].data[0]=v1.type;
	char * tmpStr1, * tmpStr2;
	if (v1.type == STRING_TYPE) {
		char * v;
		cpy(&v, &v1.data, sizeof(char*));
		tmpStr1=copyStringToSharedMemoryAndSetLocation(v, 1);
	} else {
		cpy(&sharedData->core_ctrl[myId].data[1], v1.data, 4);
	}
	sharedData->core_ctrl[myId].data[5]=v2.type;
	if (v2.type == STRING_TYPE) {
		char * v;
		cpy(&v, &v2.data, sizeof(char*));
		tmpStr2=copyStringToSharedMemoryAndSetLocation(v, 6);
	} else {
		cpy(&sharedData->core_ctrl[myId].data[6], v2.data, 4);
	}
	sharedData->core_ctrl[myId].core_command=4;
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }

	v.type=STRING_TYPE;
	unsigned int relativeLocation;
    cpy(&relativeLocation, &sharedData->core_ctrl[myId].data[11], sizeof(unsigned int));
    char * ptr=sharedData->core_ctrl[myId].shared_heap_start + relativeLocation;
	cpy(&v.data, &ptr, sizeof(char*));
	if (tmpStr1 != NULL) freeMemoryInHeap(tmpStr1);
	if (tmpStr2 != NULL) freeMemoryInHeap(tmpStr2);
	return v;
}

/**
 * Raises an error to the host and quits
 */
void raiseError(char * error) {
	stopInterpreter=1;
	sharedData->core_ctrl[myId].core_command=3;
	sharedData->core_ctrl[myId].data[0]=STRING_TYPE;

	char * msg=copyStringToSharedMemoryAndSetLocation(error, 1);
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	freeMemoryInHeap(msg);
}

/**
 * Initialises the symbol table in core memory
 */
struct symbol_node* initialiseSymbolTable(int numberSymbols) {
    unsigned char heapInUse=0;
    unsigned short coreHeapChunkLength;
    unsigned int sharedHeapChunkLength;
    coreHeapChunkLength=LOCAL_CORE_MEMORY_MAP_TOP-((int) sharedData->core_ctrl[myId].shared_heap_start)-(sizeof(unsigned short) + sizeof(unsigned char));
    sharedHeapChunkLength=SHARED_HEAP_DATA_AREA_PER_CORE-(sizeof(unsigned int) + sizeof(unsigned char));
    cpy(sharedData->core_ctrl[myId].heap_start, &coreHeapChunkLength, sizeof(unsigned short));
    cpy(&sharedData->core_ctrl[myId].heap_start[2], &heapInUse, sizeof(unsigned char));
    cpy(sharedData->core_ctrl[myId].shared_heap_start, &sharedHeapChunkLength, sizeof(unsigned int));
    cpy(&sharedData->core_ctrl[myId].shared_heap_start[4], &heapInUse, sizeof(unsigned char));

	return (void*) sharedData->core_ctrl[myId].symbol_table;
}

/**
 * Allocates some memory in the heap
 */
char* getHeapMemory(int size, char isShared) {
	if (sharedData->allInSharedMemory || isShared) {
		char * dS=allocateChunkInHeapMemory(size, 1);
		if (dS == NULL) raiseError("Out of shared heap memory for data");
		return dS;
	} else {
		char * dS=allocateChunkInHeapMemory(size, 0);
		if (dS == NULL) {
			dS=allocateChunkInHeapMemory(size, 1);
			if (dS == NULL) raiseError("Out of core and shared heap memory for data");
		}
		return dS;
	}
}

void freeMemoryInHeap(char * address) {
    unsigned chunkInUse=0;
    cpy(address-1, &chunkInUse, sizeof(unsigned char));
    consolidateHeapChunks((int) address > LOCAL_CORE_MEMORY_MAP_TOP);
}

static void consolidateHeapChunks(char inSharedMemory) {
    unsigned char chunkInUse;
    unsigned short coreChunkLength, nextCoreChunkLength;
    unsigned int chunkLength, nextChunkLength;
    size_t headersize, lenStride;
    char * heapPtr;
    if (inSharedMemory) {
        heapPtr=sharedData->core_ctrl[myId].shared_heap_start;
        headersize=sizeof(unsigned int) + sizeof(unsigned char);
        lenStride=sizeof(unsigned int);
    } else {
        heapPtr=sharedData->core_ctrl[myId].heap_start;
        headersize=sizeof(unsigned short) + sizeof(unsigned char);
        lenStride=sizeof(unsigned short);
    }
    while (1==1) {
        if (inSharedMemory) {
            cpy(&chunkLength, heapPtr, sizeof(unsigned int));
        } else {
            cpy(&coreChunkLength, heapPtr, sizeof(unsigned short));
            chunkLength=coreChunkLength;
        }
        cpy(&chunkInUse, &heapPtr[lenStride], sizeof(unsigned char));
        while (!chunkInUse) {
            cpy(&chunkInUse, &heapPtr[chunkLength + headersize +lenStride], sizeof(unsigned char));
            if (!chunkInUse) {
                if (inSharedMemory) {
                    cpy(&nextChunkLength, &heapPtr[chunkLength + headersize], sizeof(unsigned int));
                } else {
                    cpy(&nextCoreChunkLength, &heapPtr[chunkLength + headersize], sizeof(unsigned short));
                    nextChunkLength=nextCoreChunkLength;
                }
                chunkLength+=nextChunkLength+headersize;
                if (inSharedMemory) {
                    cpy(heapPtr, &chunkLength, sizeof(unsigned int));
                } else {
                    coreChunkLength=chunkLength;
                    cpy(heapPtr, &coreChunkLength, sizeof(unsigned short));
                }
            }
        }
        heapPtr+=chunkLength + headersize;
        if (inSharedMemory && heapPtr  >= sharedData->core_ctrl[myId].shared_heap_start + SHARED_HEAP_DATA_AREA_PER_CORE) {
            break;
        } else if ((int) heapPtr  >= LOCAL_CORE_MEMORY_MAP_TOP) {
            break;
        }
    }
}

static char * allocateChunkInHeapMemory(int size, char inSharedMemory) {
    unsigned char chunkInUse;
    unsigned short coreSplitChunkLength, coreChunkLength;
    unsigned int chunkLength, splitChunkLength;
    char * heapPtr;

    size_t headersize, lenStride;
    if (inSharedMemory) {
        heapPtr=sharedData->core_ctrl[myId].shared_heap_start;
        headersize=sizeof(unsigned char) + sizeof(unsigned int);
        lenStride=sizeof(unsigned int);
    } else {
        heapPtr=sharedData->core_ctrl[myId].heap_start;
        headersize=sizeof(unsigned short) + sizeof(unsigned char);
        lenStride=sizeof(unsigned short);
    }
    while (1==1) {
        if (inSharedMemory) {
            cpy(&chunkLength, heapPtr, sizeof(unsigned int));
        } else {
            cpy(&coreChunkLength, heapPtr, sizeof(unsigned short));
            chunkLength=coreChunkLength;
        }
        cpy(&chunkInUse, &heapPtr[lenStride], sizeof(unsigned char));
        if (!chunkInUse && chunkLength >= size) {
            char * splitChunk=(char*) (heapPtr + size + headersize);
            splitChunkLength=chunkLength - size - headersize;
            if (inSharedMemory) {
                cpy(splitChunk, &splitChunkLength, sizeof(unsigned int));
            } else {
                coreSplitChunkLength=splitChunkLength;
                cpy(splitChunk, &coreSplitChunkLength, sizeof(unsigned short));
            }
            cpy(&splitChunk[lenStride], &chunkInUse, sizeof(unsigned char));
            chunkLength=size;
            if (inSharedMemory) {
                cpy(heapPtr, &chunkLength, sizeof(unsigned int));
            } else {
                coreChunkLength=chunkLength;
                cpy(heapPtr, &coreChunkLength, sizeof(unsigned short));
            }
            chunkInUse=1;
            cpy(&heapPtr[lenStride], &chunkInUse, sizeof(unsigned char));
            return heapPtr + headersize;
        } else {
            heapPtr+=chunkLength + headersize;
            if (inSharedMemory && heapPtr  >= sharedData->core_ctrl[myId].shared_heap_start + SHARED_HEAP_DATA_AREA_PER_CORE) {
                break;
            } else if ((int) heapPtr  >= LOCAL_CORE_MEMORY_MAP_TOP) {
                break;
            }
        }
    }
    return NULL;
}

/**
 * Allocates some memory in the stack
 */
char* getStackMemory(int size, char isShared) {
	if (sharedData->allInSharedMemory || isShared) {
			char * dS= (char*) (sharedData->core_ctrl[myId].shared_stack_start + sharedStackEntries);
			sharedStackEntries+=size;
			if (sharedStackEntries >= SHARED_STACK_DATA_AREA_PER_CORE) raiseError("Out of shared stack memory for data");
			return dS;
		} else {
			char * dS= (char*) (sharedData->core_ctrl[myId].stack_start + localStackEntries);
			localStackEntries+=size;
			if (localStackEntries >= LOCAL_CORE_STACK_SIZE) {
				dS= (char*) (sharedData->core_ctrl[myId].shared_stack_start + sharedStackEntries);
				sharedStackEntries+=size;
				if (sharedStackEntries >= SHARED_STACK_DATA_AREA_PER_CORE) raiseError("Out of core and shared stack memory for data");
			}
			return dS;
		}
}

/**
 * Removes items from the stack which are no longer needed (i.e. the reference has been removed due to the function returning.)
 */
void clearFreedStackFrames(char* targetPointer) {
	if (targetPointer >= sharedData->core_ctrl[myId].shared_stack_start) {
		sharedStackEntries=targetPointer-sharedData->core_ctrl[myId].shared_stack_start;
	} else {
		sharedStackEntries=0;
		localStackEntries=targetPointer-sharedData->core_ctrl[myId].stack_start;
	}
}

/**
 * Sends data to some other core and blocks on this being received
 */
void sendData(struct value_defn to_send, int target) {
	int largestCoreId=sharedData->baseHostPid;
	if (to_send.type == STRING_TYPE) raiseError("Can only send integers and reals between cores");
	if (target >= sharedData->num_procs) {
		if (target < TOTAL_CORES && sharedData->core_ctrl[target].active) {
			int i;
			for (i=0;i<TOTAL_CORES;i++) {
				if (sharedData->core_ctrl[i].active) largestCoreId=i+1;
			}
		} else {
			raiseError("Attempting to send to non-existent or inactive process");
		}
	}
	if (target < largestCoreId) {
		sendDataToDeviceCore(to_send, target);
	} else {
		sendDataToHostProcess(to_send, target);
	}
}

static void sendDataToHostProcess(struct value_defn to_send, int hostProcessTarget) {
	cpy(sharedData->core_ctrl[myId].data, &hostProcessTarget, 4);
	sharedData->core_ctrl[myId].data[5]=to_send.type;
	cpy(&sharedData->core_ctrl[myId].data[6], to_send.data, 4);
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_command=5;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
}

static void sendDataToDeviceCore(struct value_defn to_send, int target) {
	if (!sharedData->core_ctrl[target].active) {
		raiseError("Attempting to send to inactive core");
	} else {
		communication_data[0]=to_send.type;
		cpy(&communication_data[1], to_send.data, 4);
		syncValues[target]=syncValues[target]==255 ? 0 : syncValues[target]+1;
		communication_data[5]=syncValues[target];
		int row=target/e_group_config.group_cols;
		int col=target-(row*e_group_config.group_cols);
		char * remoteMemory=(char*) e_get_global_address(row, col, sharedData->core_ctrl[target].postbox_start + (myId*6));
		cpy(remoteMemory, communication_data, 6);
		syncValues[target]=syncValues[target]==255 ? 0 : syncValues[target]+1;
		while (communication_data[5] != syncValues[target]) {
			cpy(communication_data, remoteMemory, 6);
		}
	}
}

struct value_defn recvData(int source) {
	int largestCoreId=sharedData->baseHostPid;
	if (source >= sharedData->num_procs) {
		if (source < TOTAL_CORES && sharedData->core_ctrl[source].active) {
			int i;
			for (i=0;i<TOTAL_CORES;i++) {
				if (sharedData->core_ctrl[i].active) largestCoreId=i+1;
			}
		} else {
			raiseError("Attempting to receive from non-existent or inactive process");
		}
	}
	if (source < largestCoreId) {
		return recvDataFromDeviceCore(source);
	} else {
		return recvDataFromHostProcess(source);
	}
}

static struct value_defn recvDataFromHostProcess(int hostSource) {
	struct value_defn to_recv;
	cpy(sharedData->core_ctrl[myId].data, &hostSource, 4);
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_command=6;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	to_recv.type=sharedData->core_ctrl[myId].data[5];
	cpy(to_recv.data, &sharedData->core_ctrl[myId].data[6], 4);
	to_recv.dtype=SCALAR;
	return to_recv;
}

/**
 * Gets some data from another core (blocking operation)
 */
static struct value_defn recvDataFromDeviceCore(int source) {
	struct value_defn to_recv;
	if (!sharedData->core_ctrl[source].active) {
		raiseError("Attempting to receive from inactive core");
	} else {
		cpy(communication_data, sharedData->core_ctrl[myId].postbox_start + (source*6), 6);
		syncValues[source]=syncValues[source]==255 ? 0 : syncValues[source]+1;
		while (communication_data[5] != syncValues[source]) {
			cpy(communication_data, sharedData->core_ctrl[myId].postbox_start + (source*6), 6);
		}
		syncValues[source]=syncValues[source]==255 ? 0 : syncValues[source]+1;
		communication_data[5]=syncValues[source];
		cpy(sharedData->core_ctrl[myId].postbox_start + (source*6), communication_data, 6);
		to_recv.type=communication_data[0];
		cpy(to_recv.data, &communication_data[1], 4);
	}
	to_recv.dtype=SCALAR;
	return to_recv;
}

/**
 * Combines the send and receive operations for a matching core. This is useful as it will block on the entire operation
 * rather than individual ones, so can greatly ease considerations of synchronisation. Basically it sends a message,
 * blocks on receive and then blocks on the initial send to form one overall block
 */
struct value_defn sendRecvData(struct value_defn to_send, int target) {
	int largestCoreId=sharedData->baseHostPid;
	if (to_send.type == STRING_TYPE) raiseError("Can only send integers and reals between cores");
	if (target >= sharedData->num_procs) {
		if (target < TOTAL_CORES && sharedData->core_ctrl[target].active) {
			int i;
			for (i=0;i<TOTAL_CORES;i++) {
				if (sharedData->core_ctrl[i].active) largestCoreId=i+1;
			}
		} else {
			raiseError("Attempting to sendrecv with non-existent or inactive process");
		}
	}
	if (target < largestCoreId) {
		return sendRecvDataWithDeviceCore(to_send, target);
	} else {
		return sendRecvDataWithHostProcess(to_send, target);
	}
}

static struct value_defn sendRecvDataWithHostProcess(struct value_defn to_send, int hostProcessTarget) {
	struct value_defn receivedData;
	cpy(sharedData->core_ctrl[myId].data, &hostProcessTarget, 4);
	sharedData->core_ctrl[myId].data[5]=to_send.type;
	cpy(&sharedData->core_ctrl[myId].data[6], to_send.data, 4);
	unsigned int pb=sharedData->core_ctrl[myId].core_busy;
	sharedData->core_ctrl[myId].core_command=7;
	sharedData->core_ctrl[myId].core_busy=0;
	while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	receivedData.type=sharedData->core_ctrl[myId].data[11];
	cpy(receivedData.data, &sharedData->core_ctrl[myId].data[12], 4);
	receivedData.dtype=SCALAR;
	return receivedData;
}

static struct value_defn sendRecvDataWithDeviceCore(struct value_defn to_send, int target) {
	struct value_defn receivedData;
	if (!sharedData->core_ctrl[target].active) {
		raiseError("Attempting to send to inactive core");
	} else {
		communication_data[0]=to_send.type;
		cpy(&communication_data[1], to_send.data, 4);
		communication_data[5]=syncValues[target]==255 ? 0 : syncValues[target]+1;
		int row=target/e_group_config.group_cols;
		int col=target-(row*e_group_config.group_cols);
		char * remoteMemory=(char*) e_get_global_address(row, col, sharedData->core_ctrl[target].postbox_start + (myId*6));
		cpy(remoteMemory, communication_data, 6);
		receivedData=recvData(target);
		communication_data[5]=syncValues[target]==0 ? 255 : syncValues[target]-1;
		while (communication_data[5] != syncValues[target]) {
			cpy(communication_data, remoteMemory, 6);
		}
	}
	receivedData.dtype=SCALAR;
	return receivedData;
}

/**
 * Synchronises all cores
 */
void syncCores(int global) {
	if (global && myId == lowestCoreId && sharedData->baseHostPid != sharedData->num_procs) {
		unsigned int pb=sharedData->core_ctrl[myId].core_busy;
		sharedData->core_ctrl[myId].core_command=8;
		sharedData->core_ctrl[myId].core_busy=0;
		while (sharedData->core_ctrl[myId].core_busy==0 || sharedData->core_ctrl[myId].core_busy<=pb) { }
	}
	performBarrier(syncbarriers, sync_tgt_bars);
}

/**
 * Performs a barrier, based on the version in elib but works over a subset of cores (based on coreid) and when
 * core 0 is not in use
 */
static void performBarrier(volatile e_barrier_t barrier_array[], e_barrier_t  * target_barrier_array[]) {
	// Barrier as a Flip-Flop
	if (myId == lowestCoreId) {
		int i;
		barrier_array[myId] = 1;
		// poll on all slots
		for (i=1; i<TOTAL_CORES; i++) {
			if (sharedData->core_ctrl[i].active) while (barrier_array[i] == 0) {};
		}
		for (i=0; i<TOTAL_CORES; i++) {
			if (sharedData->core_ctrl[i].active) barrier_array[i] = 0;
		}
		// set remote slots
		for (i=1; i<TOTAL_CORES; i++) {
			if (sharedData->core_ctrl[i].active) *(target_barrier_array[i]) = 1;
		}
	} else {
		*(target_barrier_array[0]) = 1;
		while (barrier_array[0] == 0) {};
		barrier_array[0] = 0;
	}
}

/**
 * Broadcasts data, if this is the source then send it, all cores return the data (even the source)
 */
struct value_defn bcastData(struct value_defn to_send, int source, int totalProcesses) {
	if (myId==source) {
		int i, totalActioned=0;
		for (i=0;i<TOTAL_CORES && totalActioned<totalProcesses;i++) {
			if (sharedData->core_ctrl[i].active) {
				totalActioned++;
				if (i == myId) continue;
				sendData(to_send, i);
			}
		}
		return to_send;
	} else {
		return recvData(source);
	}
}

/**
 * Reduction of data amongst the cores with some operator
 */
struct value_defn reduceData(struct value_defn to_send, unsigned char operator, int totalProcesses) {
	struct value_defn returnValue, retrieved;
	int i, intV, tempInt, totalActioned=0;
	float floatV, tempFloat;
	if (to_send.type==INT_TYPE) {
		cpy(&intV, to_send.data, sizeof(int));
	} else {
		cpy(&floatV, to_send.data, sizeof(int));
	}
	syncCores(1);
	for (i=0;i<TOTAL_CORES && totalActioned<totalProcesses;i++) {
		if (sharedData->core_ctrl[i].active) {
			totalActioned++;
			if (i == myId) continue;
			retrieved=sendRecvData(to_send, i);
			if (to_send.type==INT_TYPE) {
				cpy(&tempInt, retrieved.data, sizeof(int));
				if (operator==0) intV+=tempInt;
				if (operator==1 && tempInt < intV) intV=tempInt;
				if (operator==2 && tempInt > intV) intV=tempInt;
				if (operator==3) intV*=tempInt;
			} else {
				cpy(&tempFloat, retrieved.data, sizeof(float));
				if (operator==0) floatV+=tempFloat;
				if (operator==1 && tempFloat < floatV) floatV=tempFloat;
				if (operator==2 && tempFloat > floatV) floatV=tempFloat;
				if (operator==3) floatV*=tempFloat;
			}
		}
	}
	returnValue.type=to_send.type;
	returnValue.dtype=SCALAR;
	if (to_send.type==INT_TYPE) {
		cpy(returnValue.data, &intV, sizeof(int));
	} else {
		cpy(returnValue.data, &floatV, sizeof(float));
	}
	return returnValue;
}

/**
 * Copies an amount of data from one core to another
 */
void cpy(volatile void* to, volatile void * from, unsigned int size) {
	unsigned int i;
	char *cto=(char*) to, *cfrom=(char*) from;
	for (i=0;i<size;i++) {
		cto[i]=cfrom[i];
	}
}

/**
 * Copies some string into shared memory and sets the location in the data core area
 */
static char* copyStringToSharedMemoryAndSetLocation(char * string, int start) {
	int len=slength(string)+1;
	char* ptr=getHeapMemory(len, 1);
	unsigned int relativeLocation;
	cpy(ptr, string, len);
	relativeLocation=ptr-sharedData->core_ctrl[myId].shared_heap_start;
	cpy(&sharedData->core_ctrl[myId].data[start], &relativeLocation, sizeof(unsigned int));
	return ptr;
}

/**
 * Gets the length of a string
 */
int slength(char * v) {
	int i=0;
	while (v[i]!='\0') i++;
	return i;
}

/**
 * Compares two strings together
 */
static int stringCmp(char * str1, char * str2) {
	int len1=slength(str1);
	int len2=slength(str2);
	if (len1 != len2) return 0;
	int i;
	for (i=0;i<len1;i++) {
		if (str1[i] != str2[i]) return 0;
	}
	return 1;
}
