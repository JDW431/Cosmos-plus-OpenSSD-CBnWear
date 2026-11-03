//////////////////////////////////////////////////////////////////////////////////
// garbage_collection.c for Cosmos+ OpenSSD
// Copyright (c) 2017 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: Garbage Collector
// File Name: garbage_collection.c
//
// Version: v1.0.0
//
// Description:
//   - select a victim block
//   - collect valid pages to a free block
//   - erase a victim block to make a free block
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include <assert.h>
#include "memory_map.h"

P_GC_VICTIM_MAP gcVictimMapPtr;

void InitGcVictimMap()
{
	int dieNo, invalidSliceCnt;

	gcVictimMapPtr = (P_GC_VICTIM_MAP) GC_VICTIM_MAP_ADDR;

	for(dieNo=0 ; dieNo<USER_DIES; dieNo++)
	{
		for(invalidSliceCnt=0 ; invalidSliceCnt<SLICES_PER_BLOCK+1; invalidSliceCnt++)
		{
			gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
			gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
		}
	}
}


unsigned long long getCBScore(unsigned int dieNo, unsigned int blockNo)
{
    P_VIRTUAL_BLOCK_ENTRY candidateBlock;
    unsigned int age, invalidCnt;

    candidateBlock = &virtualBlockMapPtr->block[dieNo][blockNo];
    age = currentPageWrite - candidateBlock->mtime;
    invalidCnt = candidateBlock->invalidSliceCnt;

    if(invalidCnt == SLICES_PER_BLOCK)
        return ~0ULL;

    return ((unsigned long long)invalidCnt * age * CB_SCORE_SCALE)
        / (SLICES_PER_BLOCK - invalidCnt);
}

typedef struct _CB_VICTIM_ENTRY {
    unsigned int blockNo;
    unsigned long long score;
} CB_VICTIM_ENTRY;

/* Higher score wins; lower block number breaks equal-score ties. */
static int CbVictimHigherPriority(CB_VICTIM_ENTRY a, CB_VICTIM_ENTRY b)
{
    if(a.score != b.score)
        return a.score > b.score;

    return a.blockNo < b.blockNo;
}

/* Retain the ten highest-scoring candidates in a bounded min-heap. */
static unsigned short SelectCbVictims(unsigned int dieNo,
    unsigned int victimBlockNos[GC_VICTIM_COUNT])
{
    CB_VICTIM_ENTRY heap[GC_VICTIM_COUNT], candidate, temp;
    P_VIRTUAL_BLOCK_ENTRY block;
    unsigned int blockNo;
    unsigned short heapCount, i, parent, child, right;

    heapCount = 0;

    for(blockNo = 0; blockNo < USER_BLOCKS_PER_DIE; blockNo++)
    {
        block = &virtualBlockMapPtr->block[dieNo][blockNo];
        if(block->bad || block->free ||
            block->currentPage != USER_PAGES_PER_BLOCK ||
            block->invalidSliceCnt == 0 ||
            blockNo == virtualDieMapPtr->die[dieNo].currentBlock)
            continue;

        candidate.blockNo = blockNo;
        candidate.score = getCBScore(dieNo, blockNo);

        if(heapCount < GC_VICTIM_COUNT)
        {
            i = heapCount++;
            heap[i] = candidate;
            while(i > 0)
            {
                parent = (i - 1) / 2;
                if(!CbVictimHigherPriority(heap[parent], heap[i]))
                    break;

                temp = heap[parent];
                heap[parent] = heap[i];
                heap[i] = temp;
                i = parent;
            }
        }
        else if(CbVictimHigherPriority(candidate, heap[0]))
        {
            heap[0] = candidate;
            i = 0;
            while((unsigned int)(2 * i + 1) < heapCount)
            {
                child = 2 * i + 1;
                right = child + 1;
                if(right < heapCount && CbVictimHigherPriority(heap[child], heap[right]))
                    child = right;
                if(!CbVictimHigherPriority(heap[i], heap[child]))
                    break;

                temp = heap[i];
                heap[i] = heap[child];
                heap[child] = temp;
                i = child;
            }
        }
    }

    for(i = 0; i < heapCount; i++)
        victimBlockNos[i] = heap[i].blockNo;

    return heapCount;
}

static void SortVictimsByAge(unsigned int dieNo,
    unsigned int *victimBlockNos, unsigned short victimCount)
{
    unsigned int key;
    unsigned short i, j;

    for(i = 1; i < victimCount; i++)
    {
        key = victimBlockNos[i];
        j = i;
        while(j > 0 &&
            virtualBlockMapPtr->block[dieNo][victimBlockNos[j - 1]].mtime >
            virtualBlockMapPtr->block[dieNo][key].mtime)
        {
            victimBlockNos[j] = victimBlockNos[j - 1];
            j--;
        }
        victimBlockNos[j] = key;
    }
}

void GarbageCollection(unsigned int dieNo)
{
    unsigned int victimBlockNos[GC_VICTIM_COUNT];
    unsigned int groupMtime[GC_VICTIM_COUNT];
    unsigned int pageNo, victimBlockNo, virtualSliceAddr, logicalSliceAddr;
    unsigned int reqSlotTag, destVsa, destBlockNo, nextDestPage;
    unsigned int groupNo, fullGroupCount, age, totalValidPages, copiedPages;
    unsigned int remaining, fill, take, groupMtimeValue, gcNow;
    P_VIRTUAL_BLOCK_ENTRY destBlock;
    unsigned short victimCount, victimIdx, i;

    victimCount = SelectCbVictims(dieNo, victimBlockNos);
    if(victimCount == 0)
    {
        assert(!"[WARNING] No GC victim candidates [WARNING]");
        return;
    }

    SortVictimsByAge(dieNo, victimBlockNos, victimCount);
    gcNow = currentPageWrite;

    /* Precompute max source mtime for each group of SLICES_PER_BLOCK valid slices. */
    totalValidPages = 0;
    for(victimIdx = 0; victimIdx < victimCount; victimIdx++)
    {
        victimBlockNo = victimBlockNos[victimIdx];
        remaining = SLICES_PER_BLOCK -
            virtualBlockMapPtr->block[dieNo][victimBlockNo].invalidSliceCnt;
        groupMtimeValue = virtualBlockMapPtr->block[dieNo][victimBlockNo].mtime;

        while(remaining > 0)
        {
            groupNo = totalValidPages / SLICES_PER_BLOCK;
            fill = totalValidPages % SLICES_PER_BLOCK;
            take = SLICES_PER_BLOCK - fill;
            if(take > remaining)
                take = remaining;

            if(fill == 0 || groupMtime[groupNo] < groupMtimeValue)
                groupMtime[groupNo] = groupMtimeValue;

            totalValidPages += take;
            remaining -= take;
        }
    }
    fullGroupCount = totalValidPages / SLICES_PER_BLOCK;

    /* Keep all victims unavailable as destinations until every copy is enqueued. */
    for(victimIdx = 0; victimIdx < victimCount; victimIdx++)
        SelectiveGetFromGcVictimList(dieNo, victimBlockNos[victimIdx]);

    copiedPages = 0;
    destBlockNo = BLOCK_NONE;
    nextDestPage = SLICES_PER_BLOCK;

    for(victimIdx = 0; victimIdx < victimCount; victimIdx++)
    {
        victimBlockNo = victimBlockNos[victimIdx];
        for(pageNo = 0; pageNo < USER_PAGES_PER_BLOCK; pageNo++)
        {
            virtualSliceAddr = Vorg2VsaTranslation(dieNo, victimBlockNo, pageNo);
            logicalSliceAddr = virtualSliceMapPtr->virtualSlice[virtualSliceAddr].logicalSliceAddr;
            if(logicalSliceAddr == LSA_NONE ||
                logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr != virtualSliceAddr)
                continue;

            groupNo = copiedPages / SLICES_PER_BLOCK;

            /* Only at a group boundary or when the existing log is exhausted
               do we ask the allocator for another destination. */
            if((copiedPages % SLICES_PER_BLOCK) == 0 ||
                nextDestPage == SLICES_PER_BLOCK)
            {
                if(groupNo < fullGroupCount)
                {
                    age = gcNow - groupMtime[groupNo];
                    if(age == 0)
                        age = 1; /* 0 is reserved for the existing-log path. */
                }
                else
                    age = 0; /* Final partial group always uses the existing log. */

                destVsa = FindFreeVirtualSliceForGc(
                    dieNo, victimBlockNos, victimCount, age);
                destBlockNo = Vsa2VblockTranslation(destVsa);
                nextDestPage = Vsa2VpageTranslation(destVsa) + 1;
                destBlock = &virtualBlockMapPtr->block[dieNo][destBlockNo];

                assert(Vsa2VdieTranslation(destVsa) == dieNo);
                assert(!destBlock->free && !destBlock->bad);
                assert(destBlock->currentPage == nextDestPage);
                if(age != 0)
                    assert(Vsa2VpageTranslation(destVsa) == 0);
                for(i = 0; i < victimCount; i++)
                    assert(destBlockNo != victimBlockNos[i]);

                /* The full group's maximum mtime is known before the first write. */
                if(groupNo < fullGroupCount &&
                    destBlock->mtime < groupMtime[groupNo])
                    destBlock->mtime = groupMtime[groupNo];
            }
            else
            {
                destBlock = &virtualBlockMapPtr->block[dieNo][destBlockNo];
                assert(destBlock->currentPage == nextDestPage);
                destVsa = Vorg2VsaTranslation(dieNo, destBlockNo, nextDestPage);
                destBlock->currentPage++;
                nextDestPage++;
            }

            /* An old-log tail can span blocks; preserve actual source mtime. */
            if(destBlock->mtime < virtualBlockMapPtr->block[dieNo][victimBlockNo].mtime)
                destBlock->mtime = virtualBlockMapPtr->block[dieNo][victimBlockNo].mtime;

            //read
            reqSlotTag = GetFromFreeReqQ();
            reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
            reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_READ;
            reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
            reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
            UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
            reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = virtualSliceAddr;
            SelectLowLevelReqQ(reqSlotTag);

            //write
            reqSlotTag = GetFromFreeReqQ();
            reqPoolPtr->reqPool[reqSlotTag].reqType = REQ_TYPE_NAND;
            reqPoolPtr->reqPool[reqSlotTag].reqCode = REQ_CODE_WRITE;
            reqPoolPtr->reqPool[reqSlotTag].logicalSliceAddr = logicalSliceAddr;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.dataBufFormat = REQ_OPT_DATA_BUF_TEMP_ENTRY;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandAddr = REQ_OPT_NAND_ADDR_VSA;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEcc = REQ_OPT_NAND_ECC_ON;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.nandEccWarning = REQ_OPT_NAND_ECC_WARNING_OFF;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.rowAddrDependencyCheck = REQ_OPT_ROW_ADDR_DEPENDENCY_CHECK;
            reqPoolPtr->reqPool[reqSlotTag].reqOpt.blockSpace = REQ_OPT_BLOCK_SPACE_MAIN;
            reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry = AllocateTempDataBuf(dieNo);
            UpdateTempDataBufEntryInfoBlockingReq(reqPoolPtr->reqPool[reqSlotTag].dataBufInfo.entry, reqSlotTag);
            reqPoolPtr->reqPool[reqSlotTag].nandInfo.virtualSliceAddr = destVsa;
            logicalSliceMapPtr->logicalSlice[logicalSliceAddr].virtualSliceAddr = destVsa;
            virtualSliceMapPtr->virtualSlice[destVsa].logicalSliceAddr = logicalSliceAddr;
            SelectLowLevelReqQ(reqSlotTag);

            copiedPages++;
        }
    }

    assert(copiedPages == totalValidPages);

    for(victimIdx = 0; victimIdx < victimCount; victimIdx++)
        EraseBlock(dieNo, victimBlockNos[victimIdx]);
}


void PutToGcVictimList(unsigned int dieNo, unsigned int blockNo, unsigned int invalidSliceCnt)
{
	if(gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock != BLOCK_NONE)
	{
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock;
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock].nextBlock = blockNo;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
	}
	else
	{
		virtualBlockMapPtr->block[dieNo][blockNo].prevBlock = BLOCK_NONE;
		virtualBlockMapPtr->block[dieNo][blockNo].nextBlock = BLOCK_NONE;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = blockNo;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = blockNo;
	}
}

unsigned int GetFromGcVictimList(unsigned int dieNo)
{
	unsigned int evictedBlockNo;
	int invalidSliceCnt;

	for(invalidSliceCnt = SLICES_PER_BLOCK; invalidSliceCnt > 0 ; invalidSliceCnt--)
	{
		if(gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock != BLOCK_NONE)
		{
			evictedBlockNo = gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock;

			if(virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock != BLOCK_NONE)
			{
				virtualBlockMapPtr->block[dieNo][virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock].prevBlock = BLOCK_NONE;
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = virtualBlockMapPtr->block[dieNo][evictedBlockNo].nextBlock;

			}
			else
			{
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
				gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
			}
			return evictedBlockNo;

		}
	}

	assert(!"[WARNING] There are no free blocks. Abort terminate this ssd. [WARNING]");
	return BLOCK_FAIL;
}


void SelectiveGetFromGcVictimList(unsigned int dieNo, unsigned int blockNo)
{
	unsigned int nextBlock, prevBlock, invalidSliceCnt;

	nextBlock = virtualBlockMapPtr->block[dieNo][blockNo].nextBlock;
	prevBlock = virtualBlockMapPtr->block[dieNo][blockNo].prevBlock;
	invalidSliceCnt = virtualBlockMapPtr->block[dieNo][blockNo].invalidSliceCnt;

	if((nextBlock != BLOCK_NONE) && (prevBlock != BLOCK_NONE))
	{
		virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = nextBlock;
		virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = prevBlock;
	}
	else if((nextBlock == BLOCK_NONE) && (prevBlock != BLOCK_NONE))
	{
		virtualBlockMapPtr->block[dieNo][prevBlock].nextBlock = BLOCK_NONE;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = prevBlock;
	}
	else if((nextBlock != BLOCK_NONE) && (prevBlock == BLOCK_NONE))
	{
		virtualBlockMapPtr->block[dieNo][nextBlock].prevBlock = BLOCK_NONE;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = nextBlock;
	}
	else
	{
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].headBlock = BLOCK_NONE;
		gcVictimMapPtr->gcVictimList[dieNo][invalidSliceCnt].tailBlock = BLOCK_NONE;
	}
}

